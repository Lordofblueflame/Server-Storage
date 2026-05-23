# Muninn

Muninn is a layered C++20 filesystem indexing engine:

1. `filesystem/`: read-only host scan (raw records only).
2. `snapshot/`: canonical snapshot build and deterministic delta computation.
3. `domain/`: core models, validation passes, serialization, migration contracts.
4. `storage/`: thread-safe LMDB persistence and transport outbox.
5. `api/`: pipeline orchestration from scan to persisted transport records.
6. `transport/`: transport adapters plus shared CRUD command processing (current server implementation: websocket).

## Build Targets

1. `muninn_codec`: reusable serialization/deserialization/validation library.
2. `muninn_lib`: indexing engine + LMDB integration + transport server.
3. `muninn_cli`: CLI entry point.
4. `muninn_tests`: unit/integration tests.

## LMDB Storage Layout

1. `snapshots`: snapshot ID -> binary snapshot payload.
2. `deltas`: `(base_snapshot_id,target_snapshot_id)` -> binary delta payload.
3. `transport_outbox`: monotonic sequence -> binary websocket frame payload.
4. `meta`: store metadata (`next_transport_sequence`).

`transport_outbox` is consumed by Hugin gateway through the CRUD transport protocol (`shared/crud_protocol.hpp`):

1. Hugin sends `hello`.
2. Hugin forwards frontend `crud_command` (`read/create/update/delete`).
3. Muninn replies with `crud_result` and base64 payload records.
4. Hugin sends `ingest_ack`, Muninn applies `ack_transport_until`.

Ack behavior:

1. Outbox acknowledgement is tracked per consumer (`client_instance_id:host_id`).
2. One consumer ack does not remove unread records for other consumers.

## CLI Modes

### One-shot scan/persist (existing behavior)

```powershell
muninn_cli <rootPath> [targetPath] --db <lmdbDir>
```

### Async websocket server mode

```powershell
muninn_cli --serve --db <lmdbDir> --listen-address 0.0.0.0 --listen-port 9460 --ws-path /muninn --default-root <rootPath>
```

Important flags:

1. `--serve`: run the CRUD transport server (current implementation: websocket).
2. `--db`: LMDB directory (required in serve mode).
3. `--default-root`: fallback root for `create/update` commands when command root is empty.
4. `--outbox-batch`: default batch size for outbox reads.
5. `--io-threads`: websocket I/O thread count.
6. `--security-mode`: `dev` (relaxed token policy) or `prod` (bridge token required).
7. `--bridge-auth-token`: shared token validated during `hello` (required in `prod` mode).
8. `--allow-plain-websocket-in-prod`: explicit acknowledgement that TLS is terminated upstream.
9. `--scan-follow-symlinks`: include directory symlink traversal in scans.
10. `--scan-max-entries`: hard cap for discovered entries before scanner aborts.
11. `--scan-include <glob>`: include policy for files by wildcard (`*`, `?`); repeatable.
12. `--scan-exclude <glob>`: exclude policy for paths by wildcard (`*`, `?`); repeatable.
13. `--retention-keep-snapshots <n>`: retain newest `n` snapshots and prune older snapshots/deltas.
14. `--metrics-log-interval-seconds <n>`: periodically print LMDB object counters in serve mode.
15. `--watch-interval-seconds <n>`: enable periodic incremental capture in serve mode (seconds).
16. Operational HTTP endpoints are exposed on the same server port:
`/healthz`, `/readyz`, `/metrics`.

Scan policy notes:

1. `scan-include` filters non-directory entries; directories remain traversable unless excluded.
2. `scan-exclude` applied to directories prunes subtree traversal.

Environment overrides:

1. `MUNINN_SECURITY_MODE`: `dev` or `prod`.
2. `MUNINN_ALLOWED_CLIENT_INSTANCE_ID`: restrict allowed bridge client ID.
3. `MUNINN_BRIDGE_AUTH_TOKEN`: shared auth token for bridge handshake.
4. `MUNINN_ALLOW_PLAIN_WEBSOCKET_IN_PROD`: boolean override for upstream TLS-termination deployments.
5. `MUNINN_RETENTION_KEEP_SNAPSHOTS`: retention policy count for newest snapshots to keep.
6. `MUNINN_METRICS_LOG_INTERVAL_SECONDS`: periodic metrics log interval for serve mode.
7. `MUNINN_WATCH_INTERVAL_SECONDS`: periodic incremental capture interval for serve mode.

Watch mode behavior:

1. Watch mode runs only in `--serve` mode when `--watch-interval-seconds > 0`.
2. `--default-root` is required for watch mode; when missing, watch mode is disabled.
3. If no watch base snapshot exists, Muninn captures a full snapshot.
4. Otherwise Muninn captures incremental update (`snapshot + delta`) from the last watch snapshot.
5. If watch update base snapshot is missing (for example after retention pruning), Muninn resets watch state and retries with full snapshot on next cycle.

Operational endpoint behavior:

1. `GET /healthz`: liveness check, returns HTTP `200` with JSON status payload.
2. `GET /readyz`: readiness check, returns HTTP `200` with LMDB counters when store stats are readable;
returns HTTP `503` when stats read fails.
3. `GET /metrics`: Prometheus-style plaintext counters:
`muninn_snapshots_count`, `muninn_deltas_count`, `muninn_transport_outbox_count`,
`muninn_transport_consumers_count`, `muninn_next_transport_sequence`.

## Developer Setup

Preferred local flow (vcpkg + presets):

1. `cmake --preset dev-debug`
2. `cmake --build --preset dev-debug`
3. `ctest --preset dev-debug`

If using system dependencies instead of vcpkg:

1. `cmake --preset dev-debug-system`
2. `cmake --build --preset dev-debug-system`
3. `ctest --preset dev-debug-system`

Dependency license inventory:

1. `THIRD_PARTY_LICENSES.txt`
