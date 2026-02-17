# HostIndexer

HostIndexer is a layered C++20 filesystem indexing engine:

1. `filesystem/`: read-only host scan (raw records only).
2. `snapshot/`: canonical snapshot build and deterministic delta computation.
3. `domain/`: core models, validation passes, serialization, migration contracts.
4. `storage/`: thread-safe LMDB persistence and transport outbox.
5. `api/`: pipeline orchestration from scan to persisted transport records.
6. `transport/`: async websocket CRUD server for LocalEdge control + outbox streaming.

## Build Targets

1. `host_indexer_codec`: reusable serialization/deserialization/validation library.
2. `host_indexer_lib`: indexing engine + LMDB integration + websocket transport server.
3. `host_indexer_cli`: CLI entry point.
4. `host_indexer_tests`: unit/integration tests.

## LMDB Storage Layout

1. `snapshots`: snapshot ID -> binary snapshot payload.
2. `deltas`: `(base_snapshot_id,target_snapshot_id)` -> binary delta payload.
3. `transport_outbox`: monotonic sequence -> binary websocket frame payload.
4. `meta`: store metadata (`next_transport_sequence`).

`transport_outbox` is consumed by LocalEdge gateway through websocket CRUD (`shared/crud_protocol.hpp`):

1. LocalEdge sends `hello`.
2. LocalEdge forwards frontend `crud_command` (`read/create/update/delete`).
3. HostIndexer replies with `crud_result` and base64 payload records.
4. LocalEdge sends `ingest_ack`, HostIndexer applies `ack_transport_until`.

## CLI Modes

### One-shot scan/persist (existing behavior)

```powershell
host_indexer_cli <rootPath> [targetPath] --db <lmdbDir>
```

### Async websocket server mode

```powershell
host_indexer_cli --serve --db <lmdbDir> --listen-address 0.0.0.0 --listen-port 9460 --ws-path /hostindexer --default-root <rootPath>
```

Important flags:

1. `--serve`: run websocket CRUD server (long-running).
2. `--db`: LMDB directory (required in serve mode).
3. `--default-root`: fallback root for `create/update` commands when command root is empty.
4. `--outbox-batch`: default batch size for outbox reads.
5. `--io-threads`: websocket I/O thread count.
