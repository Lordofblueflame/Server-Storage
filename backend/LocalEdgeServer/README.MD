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
cmake -S LocalEdgeServer -B LocalEdgeServer/build
cmake --build LocalEdgeServer/build --target localedge_server
```

## Run

```powershell
localedge_server --host-id my-host --hostindexer-host 127.0.0.1 --hostindexer-port 9460 --hostindexer-path /hostindexer --api-address 0.0.0.0 --api-port 9440
```

## Frontend API Skeleton

See `docs/frontend-api-skeleton.md`.
