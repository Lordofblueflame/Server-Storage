# Hugin

Hugin is now a stateless gateway layer:

1. Accepts frontend API requests.
2. Forwards CRUD commands to Muninn through a transport adapter (current implementation: websocket).
3. Streams Muninn `crud_result` messages back to frontend clients.

Hugin does **not** own LMDB storage or Muninn codec validation logic.

## Role Split

1. Muninn:
   - LMDB persistence (`snapshots`, `deltas`, `transport_outbox`, `meta`).
   - Validation + serialization/deserialization.
   - Websocket CRUD endpoint.
2. Hugin:
   - API facade for frontend.
   - Command forwarding and result fan-out.

## Build

```powershell
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug
```

System dependency fallback (no vcpkg):

```powershell
cmake --preset dev-debug-system
cmake --build --preset dev-debug-system
ctest --preset dev-debug-system
```

## Run

```powershell
hugin --host-id my-host --muninn-host 127.0.0.1 --muninn-port 9460 --muninn-path /muninn --api-address 0.0.0.0 --api-port 9440
```

Optional filesystem sandboxing for backend file endpoints:

```powershell
hugin --allowed-read-root C:\data\readonly --allowed-write-root C:\data\uploads
```

Environment equivalents:

1. `HUGIN_ALLOWED_READ_ROOT`
2. `HUGIN_ALLOWED_WRITE_ROOT`

Upload guardrails (Phase C):

1. `--max-upload-file-bytes`
2. `--max-upload-directory-files`
3. `--max-upload-total-decoded-bytes`

Environment equivalents:

1. `HUGIN_MAX_UPLOAD_FILE_BYTES`
2. `HUGIN_MAX_UPLOAD_DIRECTORY_FILES`
3. `HUGIN_MAX_UPLOAD_TOTAL_DECODED_BYTES`

Bridge reconnect policy (Phase C):

1. `--reconnect-ms`
2. `--reconnect-max-ms`
3. `--reconnect-backoff-factor`
4. `--reconnect-jitter-percent`

Environment equivalents:

1. `HUGIN_RECONNECT_MAX_MS`
2. `HUGIN_RECONNECT_BACKOFF_FACTOR`
3. `HUGIN_RECONNECT_JITTER_PERCENT`
