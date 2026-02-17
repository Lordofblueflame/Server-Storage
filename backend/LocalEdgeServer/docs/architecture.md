# LocalEdgeServer Architecture (Gateway Mode)

## Purpose

LocalEdgeServer is a stateless API gateway between frontend and HostIndexer.

## Data Flow

1. Frontend sends CRUD command to `POST /api/v1/commands`.
2. LocalEdge forwards command to HostIndexer websocket CRUD endpoint.
3. HostIndexer validates + persists to LMDB.
4. HostIndexer emits `crud_result`.
5. LocalEdge broadcasts `crud_result` to frontend websocket clients on `/api/v1/stream`.

## Modules

1. `src/api/frontend_api_server.*`
   - HTTP command endpoint (`/api/v1/commands`).
   - Health endpoint (`/api/v1/health`).
   - WebSocket stream endpoint (`/api/v1/stream`).
2. `src/net/hostindexer_client.*`
   - Persistent websocket client to HostIndexer.
   - Auto reconnect.
   - Optional outbox pull loop (`read` command).
   - Cumulative `ingest_ack` back to HostIndexer.

## Non-Goals (Gateway Mode)

1. No LMDB ownership in LocalEdge.
2. No authoritative snapshot/delta validation in LocalEdge.
3. No materialized storage state in LocalEdge.
