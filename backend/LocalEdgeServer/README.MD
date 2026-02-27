# LocalEdgeServer

LocalEdgeServer is now a stateless gateway layer:

1. Accepts frontend API requests.
2. Forwards CRUD commands to HostIndexer over websocket.
3. Streams HostIndexer `crud_result` messages back to frontend clients.

LocalEdgeServer does **not** own LMDB storage or HostIndexer codec validation logic.

## Role Split

1. HostIndexer:
   - LMDB persistence (`snapshots`, `deltas`, `transport_outbox`, `meta`).
   - Validation + serialization/deserialization.
   - Websocket CRUD endpoint.
2. LocalEdgeServer:
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
localedge_server --host-id my-host --hostindexer-host 127.0.0.1 --hostindexer-port 9460 --hostindexer-path /hostindexer --api-address 0.0.0.0 --api-port 9440
```

Optional filesystem sandboxing for backend file endpoints:

```powershell
localedge_server --allowed-read-root C:\data\readonly --allowed-write-root C:\data\uploads
```

Environment equivalents:

1. `LOCALEDGE_ALLOWED_READ_ROOT`
2. `LOCALEDGE_ALLOWED_WRITE_ROOT`

Upload guardrails (Phase C):

1. `--max-upload-file-bytes`
2. `--max-upload-directory-files`
3. `--max-upload-total-decoded-bytes`

Environment equivalents:

1. `LOCALEDGE_MAX_UPLOAD_FILE_BYTES`
2. `LOCALEDGE_MAX_UPLOAD_DIRECTORY_FILES`
3. `LOCALEDGE_MAX_UPLOAD_TOTAL_DECODED_BYTES`

Bridge reconnect policy (Phase C):

1. `--reconnect-ms`
2. `--reconnect-max-ms`
3. `--reconnect-backoff-factor`
4. `--reconnect-jitter-percent`

Environment equivalents:

1. `LOCALEDGE_RECONNECT_MAX_MS`
2. `LOCALEDGE_RECONNECT_BACKOFF_FACTOR`
3. `LOCALEDGE_RECONNECT_JITTER_PERCENT`
