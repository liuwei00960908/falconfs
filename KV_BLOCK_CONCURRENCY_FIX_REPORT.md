# KV Block Concurrency Fix Report

## Summary

This report documents the concurrency issues found in the FalconFS KV block cache path and the fixes applied in this workspace.

The failing scenario was concurrent `FalconBlockPut()` calls, especially many threads putting the same key while the size-file metadata and data file were being created. The original implementation could return `IO_ERROR`, `FILE_EXISTS`, or `UNKNOWN`, and PostgreSQL logs showed deadlocks and `tuple concurrently updated` errors.

## Reproduction

An isolated local metadata service was started with a separate port prefix and metadata workspace:

```bash
FALCON_UNIQUE_PORT_PREFIX=512 \
FALCON_META_WORKSPACE=/tmp/falconfs-kv-rerun \
FALCONFS_INSTALL_DIR=/usr/local/falconfs \
bash deploy/meta/falcon_meta_start.sh --comm-plugin=brpc
```

The concurrent repro was added as:

```bash
build/tests/block_api/BlockApiConcurrent 127.0.0.1 51210 32 64
```

The original failure produced mixed errors such as:

```text
FalconBlockPut ret=25
FalconBlockPut ret=33
FalconBlockPut ret=13
```

Relevant error-code meanings:

- `13`: `FILE_EXISTS`
- `25`: `UNKNOWN`
- `33`: `IO_ERROR`

PostgreSQL logs also showed:

```text
ERROR: deadlock detected
ERROR: tuple concurrently updated
```

## Root Causes

### 1. Deadlock in `BLOCK_ALLOC`

`KeyBlockAllocatorAlloc()` updated `falcon_size_file_table` under `AccessExclusiveLock`, returned to `FalconBlockAllocHandle()`, and then `FalconBlockAllocHandle()` called `LoadSizeFileInfo()` to read the same table under `AccessShareLock`.

Under concurrency, multiple sessions could form a lock cycle:

- one transaction held or waited on the size-file table update;
- another requested `AccessExclusiveLock`;
- another requested `AccessShareLock`;
- PostgreSQL detected a deadlock.

### 2. Concurrent Updates on the Same Metadata Row

`BLOCK_GET` updated `atime`, and `BLOCK_UPDATE` updated `mtime/ctime/version` using weaker write locking. Concurrent puts to the same key caused multiple transactions to update the same tuple, leading to:

```text
ERROR: tuple concurrently updated
```

### 3. Duplicate Metadata Rows Under Concurrent Create

Concurrent `SIZE_FILE_CREATE` and `BLOCK_INSERT` could insert duplicate logical rows before the operation observed an existing row. The unique indexes existed, but this path uses catalog tuple APIs, so the code needed explicit existence checks while holding the table lock.

Observed bad state before the fix:

```sql
select * from pg_catalog.falcon_size_file_table;
select key, size, "offset" from pg_catalog.falcon_key_block_table;
```

showed duplicate `size` rows and duplicate `key` rows.

### 4. Data File Creation Race

One thread could create size-file metadata, while another immediately observed that metadata, allocated a block, and attempted to write the data file before the first thread had created/truncated the local file. That produced `IO_ERROR` from `pwrite/open`.

### 5. Non-idempotent Client Create Path

`FalconBlockPut()` treated failed `BlockInsert()` as a final failure and aborted allocation. Under concurrency, another thread may have already inserted the key, so the correct behavior is to switch to update/upsert semantics.

## Fixes

### Metadata Allocator

Files:

- `falcon/include/metadb/key_block_allocator.h`
- `falcon/metadb/key_block_allocator.c`
- `falcon/metadb/meta_handle.c`

Changes:

- `KeyBlockAllocatorAlloc()` now returns `filePath`, `capacity`, and `state` along with the allocated offset.
- `FalconBlockAllocHandle()` no longer performs a second `LoadSizeFileInfo()` read after allocation.
- The allocator keeps the table lock until transaction end after updating `next_offset` by closing with `NoLock`.

### Key Block Metadata Locking

File:

- `falcon/metadb/meta_handle.c`

Changes:

- `BLOCK_GET` with atime update uses `AccessExclusiveLock`.
- `BLOCK_INSERT`, `BLOCK_UPDATE`, and `BLOCK_DEL` use `AccessExclusiveLock`.
- Locks are kept until transaction end for mutating operations by closing with `NoLock`.

This serializes mutating operations on `falcon_key_block_table` and avoids `tuple concurrently updated` failures.

### Explicit Duplicate Checks

File:

- `falcon/metadb/meta_handle.c`

Changes:

- Added `KeyBlockExists()` and `SizeFileExists()`.
- `BLOCK_INSERT` checks for an existing key while holding the key-block table lock.
- `SIZE_FILE_CREATE` checks for an existing size while holding the size-file table lock.

This prevents duplicate logical rows under concurrent create.

### Client Upsert and File Creation Ordering

File:

- `falcon_client/src/block_meta.cpp`

Changes:

- `FalconBlockPut()` now has bounded retry behavior for transient create races.
- If the key appears during a race, the operation writes to the existing location and calls `BlockUpdate()`.
- After `BlockAlloc()` succeeds, the client calls `EnsureSizeFile()` before writing, so local data-file creation is guaranteed before `pwrite`.
- `FILE_EXISTS`, `IO_ERROR`, and `UNKNOWN` during the create path are retried or converted to update when the key becomes visible.

### Test Coverage

Files:

- `tests/block_api/block_api_concurrent.cpp`
- `tests/block_api/CMakeLists.txt`

Added `BlockApiConcurrent`, which:

- starts many threads together;
- repeatedly calls `FalconBlockPut()` on the same key;
- verifies final `FalconBlockGet()`;
- deletes the key at the end.

## Validation

Build:

```bash
./build.sh build pg
```

Passed.

Single-key concurrent tests:

```bash
FALCON_BLOCK_DATA_DIR=/tmp/falconfs-kv-rerun-data \
FALCON_BLOCK_FILE_CAPACITY=128 \
build/tests/block_api/BlockApiConcurrent 127.0.0.1 51210 32 64
```

Ran successfully twice.

Multi-process concurrent tests:

```bash
for n in $(seq 1 6); do
  (
    FALCON_BLOCK_DATA_DIR=/tmp/falconfs-kv-rerun-data \
    FALCON_BLOCK_FILE_CAPACITY=512 \
    build/tests/block_api/BlockApiConcurrent 127.0.0.1 51210 16 32
  ) &
done
wait
```

All six concurrent processes passed.

Higher concurrency test:

```bash
FALCON_BLOCK_DATA_DIR=/tmp/falconfs-kv-rerun-data2 \
FALCON_BLOCK_FILE_CAPACITY=1024 \
build/tests/block_api/BlockApiConcurrent 127.0.0.1 51210 64 64
```

Passed.

Existing serial E2E:

```bash
FALCON_BLOCK_DATA_DIR=/tmp/falconfs-kv-rerun-data \
FALCON_BLOCK_FILE_CAPACITY=512 \
build/tests/block_api/BlockApiE2E 127.0.0.1 51210
```

Passed.

Metadata duplicate check:

```sql
select count(*) as size_rows, count(distinct size) as unique_sizes
from pg_catalog.falcon_size_file_table;

select size, count(*)
from pg_catalog.falcon_size_file_table
group by size
having count(*) > 1;

select count(*) as key_rows, count(distinct key) as unique_keys
from pg_catalog.falcon_key_block_table;

select key, count(*)
from pg_catalog.falcon_key_block_table
group by key
having count(*) > 1;
```

Result:

- `size_rows == unique_sizes`
- no duplicate `size`
- `key_rows == unique_keys`
- no duplicate `key`

Log check after the final rerun showed no new:

- `ERROR`
- `deadlock`
- `tuple concurrently updated`
- `resource was not closed`

## Notes and Remaining Risk

The current fix favors correctness by using table-level serialization for KV block metadata mutations. This removes the observed races, but it can limit throughput under high write concurrency.

A future optimization could replace table-level serialization with row-level locking and retry loops around PostgreSQL concurrent-update errors. That would preserve correctness while allowing independent keys or size classes to proceed in parallel.

The shared-memory free-list remains a later-phase design risk: it is not durable across restart and is not transaction-aware in the same way a PostgreSQL-backed free table would be. This was not the main failure in the reproduced same-key concurrent put path, but it should be revisited before relying on reclaim behavior for production capacity management.
