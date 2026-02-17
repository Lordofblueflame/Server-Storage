# LocalEdge Gateway Protocol v1

LocalEdge uses `shared/crud_protocol.hpp` JSON messages to communicate with HostIndexer.

## HostIndexer Bridge

1. `hello`
2. `hello_ack`
3. `crud_command`
4. `crud_result`
5. `ingest_ack`

Transport is websocket text frames carrying JSON payloads.

## Frontend Facade

1. `POST /api/v1/commands`
   - accepts `crud_command` JSON.
2. `GET /api/v1/stream` (websocket)
   - pushes `crud_result` JSON.
3. `GET /api/v1/health`
   - health probe.
