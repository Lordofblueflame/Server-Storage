#!/usr/bin/env python3

import base64
import hashlib
import hmac
import json
import os
import sys
import time
import urllib.error
import urllib.request


BASE_URL = os.environ.get("HUGIN_BASE_URL", "http://hugin:9440").rstrip("/")
JWT_SECRET = os.environ["HUGIN_JWT_SECRET"]
JWT_ISSUER = os.environ.get("HUGIN_JWT_ISSUER", "")
JWT_AUDIENCE = os.environ.get("HUGIN_JWT_AUDIENCE", "")
LOCAL_ACCESS_TOKEN = os.environ.get("HUGIN_LOCAL_ACCESS_TOKEN", "")
UPLOAD_DIRECTORY = os.environ.get("SMOKE_UPLOAD_DIRECTORY", "/data/watched")
READINESS_TIMEOUT_SECONDS = float(os.environ.get("SMOKE_READINESS_TIMEOUT_SECONDS", "30"))
PROPAGATION_TIMEOUT_SECONDS = float(os.environ.get("SMOKE_PROPAGATION_TIMEOUT_SECONDS", "30"))
POLL_INTERVAL_SECONDS = float(os.environ.get("SMOKE_POLL_INTERVAL_SECONDS", "1"))


def base64url_encode(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def make_jwt() -> str:
    header = {"alg": "HS256", "typ": "JWT"}
    now = int(time.time())
    payload = {"sub": "docker-smoke-test", "exp": now + 600, "iat": now, "nbf": now}
    if JWT_ISSUER:
        payload["iss"] = JWT_ISSUER
    if JWT_AUDIENCE:
        payload["aud"] = JWT_AUDIENCE

    signing_input = ".".join(
        (
            base64url_encode(json.dumps(header, separators=(",", ":")).encode("utf-8")),
            base64url_encode(json.dumps(payload, separators=(",", ":")).encode("utf-8")),
        )
    )
    signature = hmac.new(
        JWT_SECRET.encode("utf-8"),
        signing_input.encode("ascii"),
        hashlib.sha256,
    ).digest()
    return signing_input + "." + base64url_encode(signature)


AUTHORIZATION = "Bearer " + make_jwt()
LOCAL_AUTHORIZATION = "Bearer " + LOCAL_ACCESS_TOKEN if LOCAL_ACCESS_TOKEN else ""


def request_json(
    method: str,
    path: str,
    payload: dict | None = None,
    *,
    authorized: bool = False,
    authorization: str | None = None,
) -> tuple[int, dict]:
    body = None
    headers = {"Accept": "application/json"}
    if payload is not None:
        body = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if authorized:
        headers["Authorization"] = AUTHORIZATION
    if authorization is not None:
        headers["Authorization"] = authorization

    request = urllib.request.Request(BASE_URL + path, method=method, data=body, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            text = response.read().decode("utf-8")
            return response.status, json.loads(text)
    except urllib.error.HTTPError as error:
        text = error.read().decode("utf-8")
        try:
            payload = json.loads(text)
        except json.JSONDecodeError:
            payload = {"ok": False, "message": text}
        return error.code, payload


def wait_until(description: str, timeout_seconds: float, callback):
    deadline = time.time() + timeout_seconds
    last_error = None
    while time.time() < deadline:
        try:
            result = callback()
            if result is not None:
                return result
        except Exception as error:  # pragma: no cover - smoke-only failure reporting
            last_error = error
        time.sleep(POLL_INTERVAL_SECONDS)
    if last_error is not None:
        raise RuntimeError(f"{description} timed out: {last_error}") from last_error
    raise RuntimeError(f"{description} timed out")


def main() -> int:
    print(f"[smoke] waiting for {BASE_URL}/api/v1/health")
    wait_until(
        "gateway health",
        READINESS_TIMEOUT_SECONDS,
        lambda: request_json("GET", "/api/v1/health")[1].get("ok") and True,
    )

    print("[smoke] waiting for realtime snapshot materialization")

    def wait_for_snapshot():
        status, payload = request_json("POST", "/api/v1/realtime/snapshot", {}, authorized=True)
        if status == 200 and payload.get("ok"):
            return payload
        return None

    snapshot = wait_until("realtime snapshot readiness", READINESS_TIMEOUT_SECONDS, wait_for_snapshot)
    host_id = snapshot["host_id"]
    root_path = snapshot["path"]
    print(f"[smoke] latest host_id={host_id} root_path={root_path}")

    if LOCAL_AUTHORIZATION:
        print("[smoke] verifying local bearer token auth")
        status, local_snapshot = request_json(
            "POST",
            "/api/v1/realtime/snapshot",
            {},
            authorization=LOCAL_AUTHORIZATION,
        )
        if status != 200 or not local_snapshot.get("ok"):
            raise RuntimeError(f"local token auth failed: status={status} payload={local_snapshot}")

    file_name = f"smoke-upload-{int(time.time())}.txt"
    content = b"hello from docker smoke test\n"
    print(f"[smoke] uploading {file_name} to {UPLOAD_DIRECTORY}")
    status, upload = request_json(
        "POST",
        "/api/v1/files/upload",
        {
            "destination_dir": UPLOAD_DIRECTORY,
            "file_name": file_name,
            "content_b64": base64.b64encode(content).decode("ascii"),
        },
        authorized=True,
    )
    if status != 200 or not upload.get("ok"):
        raise RuntimeError(f"upload failed: status={status} payload={upload}")

    print("[smoke] waiting for uploaded file to appear in realtime tree")

    def wait_for_entry():
        status, payload = request_json(
            "POST",
            "/api/v1/realtime/tree",
            {"host_id": host_id, "path": UPLOAD_DIRECTORY},
            authorized=True,
        )
        if status != 200 or not payload.get("ok"):
            return None
        for entry in payload.get("entries", []):
            if entry.get("name") == file_name:
                return entry
        return None

    entry = wait_until("upload propagation", PROPAGATION_TIMEOUT_SECONDS, wait_for_entry)
    print(f"[smoke] upload propagated path={entry['path']}")

    print("[smoke] verifying file download round-trip")
    status, download = request_json(
        "POST",
        "/api/v1/files/download",
        {"path": UPLOAD_DIRECTORY + "/" + file_name},
        authorized=True,
    )
    if status != 200 or not download.get("ok"):
        raise RuntimeError(f"download failed: status={status} payload={download}")
    decoded = base64.b64decode(download["content_b64"])
    if decoded != content:
        raise RuntimeError("downloaded content does not match uploaded content")

    print("[smoke] success")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # pragma: no cover - smoke-only failure reporting
        print(f"[smoke] failure: {error}", file=sys.stderr)
        raise SystemExit(1)
