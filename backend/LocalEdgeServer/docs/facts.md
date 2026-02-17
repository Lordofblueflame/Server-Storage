# HostIndexer Discovery Facts

This document records only facts backed by repository code inspection.

## Transport outbox storage

1. LMDB DB name is `transport_outbox`.  
   Source: `HostIndexer/storage/lmdb_store.cpp:19`.
2. Sequence allocator key is `meta["next_transport_sequence"]`.  
   Source: `HostIndexer/storage/lmdb_store.cpp:21`, `HostIndexer/storage/lmdb_store.cpp:709`.
3. Sequence starts at `1` when missing and increments by `+1` per enqueue.  
   Source: `HostIndexer/storage/lmdb_store.cpp:355`, `HostIndexer/storage/lmdb_store.cpp:753`, `HostIndexer/storage/lmdb_store.cpp:754`.
4. Sequence key encoding in outbox is 8-byte big-endian.  
   Source: `HostIndexer/storage/lmdb_store.cpp:27`, `HostIndexer/storage/lmdb_store.cpp:680`.
5. Outbox fetch reads from `MDB_FIRST` and returns oldest records first.  
   Source: `HostIndexer/storage/lmdb_store.cpp:790`.
6. ACK deletes all outbox records with key `<= inclusive_sequence`.  
   Source: `HostIndexer/storage/lmdb_store.cpp:822`, `HostIndexer/storage/lmdb_store.cpp:851`, `HostIndexer/storage/lmdb_store.cpp:856`.

## Outbox frame bytes

1. Stored value is an application frame serialized as:
   - `[1:type][8:created_at_be][4:routing_key_len_be][routing_key][4:payload_len_be][payload]`.
   Source: `HostIndexer/storage/lmdb_store.cpp:98`.
2. Supported `type` values are:
   - `1 = Snapshot`
   - `2 = Delta`
   Source: `HostIndexer/storage/lmdb_store.hpp:55`.
3. There is no frame magic, checksum, compression, or encryption at this layer.
   Source: frame serializer/parser in `HostIndexer/storage/lmdb_store.cpp:98` and `HostIndexer/storage/lmdb_store.cpp:125`.

## Payload codecs

1. Snapshot payload binary header:
   - magic `0x48534E50` (`"HSNP"`), format version `major=1`, `minor=0`.
   Source: `HostIndexer/domain/serialization/serialization_helper.hpp:13`, `HostIndexer/domain/serialization/serialization_helper.hpp:16`, `HostIndexer/domain/serialization/serialization.cpp:350`.
2. Delta payload binary header:
   - magic `0x48444C54` (`"HDLT"`), format version `major=1`, `minor=0`.
   Source: `HostIndexer/domain/serialization/serialization_helper.hpp:14`, `HostIndexer/domain/serialization/serialization.cpp:419`.
3. Snapshot and delta codec primitive endianness is little-endian.
   Source: `HostIndexer/domain/serialization/serialization_helper.hpp:54` through `HostIndexer/domain/serialization/serialization_helper.hpp:103`.
4. Snapshot and delta entry-count limits are `kMaxBinaryEntries = 5,000,000`.
   Source: `HostIndexer/domain/serialization/serialization_helper.hpp:19`, `HostIndexer/domain/serialization/serialization.cpp:359`, `HostIndexer/domain/serialization/serialization.cpp:426`.

## Message production

1. `enqueue_snapshot` serializes snapshot binary payload and uses routing key `snapshot.host_identifier`.
   Source: `HostIndexer/storage/lmdb_store.cpp:594`, `HostIndexer/storage/lmdb_store.cpp:616`.
2. `enqueue_delta` serializes delta payload and uses routing key `"<base_snapshot_id>-><target_snapshot_id>"`.
   Source: `HostIndexer/storage/lmdb_store.cpp:623`, `HostIndexer/storage/lmdb_store.cpp:645`.
3. Pipeline produces outbox records after snapshot/delta persistence when `enqueue_transport=true`.
   Source: `HostIndexer/api/indexing_pipeline.cpp:27`, `HostIndexer/api/indexing_pipeline.cpp:33`, `HostIndexer/api/indexing_pipeline.cpp:77`, `HostIndexer/api/indexing_pipeline.cpp:88`.

## Identity and snapshot semantics

1. Snapshot has `host_identifier` field.
   Source: `HostIndexer/domain/model/models.hpp:61`.
2. Delta does not include `host_identifier`.
   Source: `HostIndexer/domain/model/models.hpp:87`.
3. Scanner host identity defaults to hostname/`HOSTNAME`/`COMPUTERNAME` fallback.
   Source: `HostIndexer/filesystem/filesystem_scanner.cpp:34`, `HostIndexer/filesystem/filesystem_scanner.cpp:43`, `HostIndexer/filesystem/filesystem_scanner.cpp:46`.
4. Snapshot ID is deterministic hash of `host_identifier|root_path|scanned_at|entry_count`.
   Source: `HostIndexer/snapshot/snapshot_builder.cpp:75`.
5. Delta links `base_snapshot_id -> target_snapshot_id` and sort order is deterministic.
   Source: `HostIndexer/snapshot/delta_engine.cpp:120`, `HostIndexer/snapshot/delta_engine.cpp:69`, `HostIndexer/snapshot/delta_engine.cpp:254`.

## Validation reuse

1. Public validation entry points:
   - `validate_snapshot`
   - `validate_delta`
   Source: `HostIndexer/domain/validation/validate_snapshot.hpp:9`.
2. Snapshot validation passes include header/identity/path/graph/timestamp checks.
   Source: `HostIndexer/domain/validation/validators.cpp:742` through `HostIndexer/domain/validation/validators.cpp:746`.
3. Delta validation includes referential and conflict checks.
   Source: `HostIndexer/domain/validation/validators.cpp:764`.
