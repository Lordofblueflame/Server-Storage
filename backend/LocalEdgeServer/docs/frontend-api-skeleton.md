# Frontend API Skeleton

This is a first-pass API skeleton for approval.

## Transport

1. HTTP for command submission.
2. WebSocket for async result streaming.

## Endpoints

1. `GET /api/v1/health`
   - Returns gateway health:
   - `{ "ok": true, "service": "localedge-api-gateway" }`

2. `POST /api/v1/commands`
   - Body: `shared/crud_protocol.hpp` `crud_command` JSON.
   - If `request_id` is missing, LocalEdge assigns one (`frontend-*`).
   - Forwards command to HostIndexer websocket session.
   - Returns:
     - `202 Accepted` on accepted forwarding.
     - `503 Service Unavailable` if bridge is unavailable.

3. `GET /api/v1/stream` (WebSocket upgrade)
   - Pushes each HostIndexer `crud_result` JSON message as text frame.
   - Fan-out to all connected frontend subscribers.

## Example Command Body

```json
{
  "type": "crud_command",
  "protocol_version": 1,
  "request_id": "frontend-123",
  "operation": "create",
  "root_path": "/workspace/data",
  "snapshot_id": 0,
  "base_snapshot_id": 0,
  "ack_sequence": 0,
  "outbox_batch_size": 128
}
```

## Next Approval Steps

1. Auth: API token/JWT validation at gateway.
2. Multi-tenant routing: namespace host IDs per tenant/session.
3. Delivery contract:
   - keep full `crud_result` passthrough.
   - or map to frontend-specific DTO.
4. Backpressure policy for `/api/v1/stream` slow consumers.
