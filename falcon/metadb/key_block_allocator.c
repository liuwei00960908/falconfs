/* Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * SPDX-License-Identifier: MulanPSL-2.0
 */

#include "metadb/key_block_allocator.h"

#include <stdint.h>

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/hsearch.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "metadb/meta_handle_helper.h"
#include "metadb/size_file_table.h"
#include "utils/shmem_control.h"
#include "utils/utils.h"

#define KEY_BLOCK_ALLOCATOR_SIZE_CLASS_MAX 4096
#define KEY_BLOCK_ALLOCATOR_FREE_NODE_MAX 1500000
#define KEY_BLOCK_ALLOCATOR_INVALID_NODE (-1)

typedef struct KeyBlockAllocatorFreeNode
{
    uint64_t offset;
    int32_t nextIndex;
} KeyBlockAllocatorFreeNode;

typedef struct KeyBlockAllocatorHeader
{
    uint32_t allocatedNodeCount;
    int32_t reusableNodeHead;
} KeyBlockAllocatorHeader;

typedef struct KeyBlockAllocatorFreeListEntry
{
    uint64_t size;
    int32_t headIndex;
    uint32_t count;
    bool reclaimEnabled;
} KeyBlockAllocatorFreeListEntry;

static ShmemControlData *KeyBlockAllocatorShmemControl = NULL;
static KeyBlockAllocatorHeader *KeyBlockAllocatorShmemHeader = NULL;
static KeyBlockAllocatorFreeNode *KeyBlockAllocatorFreeNodes = NULL;
static HTAB *KeyBlockAllocatorFreeLists = NULL;

static Oid KeyBlockAllocatorTableIndexOid(const char *tableName)
{
    char *indexName = psprintf("%s_index", tableName);
    Oid indexOid = GetRelationOidByName_FALCON(indexName);
    pfree(indexName);
    return indexOid;
}

size_t KeyBlockAllocatorShmemsize(void)
{
    return sizeof(ShmemControlData) + sizeof(KeyBlockAllocatorHeader) +
           sizeof(KeyBlockAllocatorFreeNode) * KEY_BLOCK_ALLOCATOR_FREE_NODE_MAX +
           hash_estimate_size(KEY_BLOCK_ALLOCATOR_SIZE_CLASS_MAX, sizeof(KeyBlockAllocatorFreeListEntry));
}

void KeyBlockAllocatorShmemInit(void)
{
    bool initialized;
    size_t structSize = sizeof(ShmemControlData) + sizeof(KeyBlockAllocatorHeader) +
                        sizeof(KeyBlockAllocatorFreeNode) * KEY_BLOCK_ALLOCATOR_FREE_NODE_MAX;

    KeyBlockAllocatorShmemControl = ShmemInitStruct("Falcon Key Block Allocator Control", structSize, &initialized);
    KeyBlockAllocatorShmemHeader = (KeyBlockAllocatorHeader *)(KeyBlockAllocatorShmemControl + 1);
    KeyBlockAllocatorFreeNodes = (KeyBlockAllocatorFreeNode *)(KeyBlockAllocatorShmemHeader + 1);
    if (!initialized) {
        KeyBlockAllocatorShmemControl->trancheId = LWLockNewTrancheId();
        KeyBlockAllocatorShmemControl->lockTrancheName = "Falcon Key Block Allocator";
        LWLockRegisterTranche(KeyBlockAllocatorShmemControl->trancheId,
                              KeyBlockAllocatorShmemControl->lockTrancheName);
        LWLockInitialize(&KeyBlockAllocatorShmemControl->lock, KeyBlockAllocatorShmemControl->trancheId);

        KeyBlockAllocatorShmemHeader->allocatedNodeCount = 0;
        KeyBlockAllocatorShmemHeader->reusableNodeHead = KEY_BLOCK_ALLOCATOR_INVALID_NODE;
    }

    HASHCTL hashCtl;
    memset(&hashCtl, 0, sizeof(hashCtl));
    hashCtl.keysize = sizeof(uint64_t);
    hashCtl.entrysize = sizeof(KeyBlockAllocatorFreeListEntry);
    KeyBlockAllocatorFreeLists = ShmemInitHash("Falcon Key Block Allocator Free Lists",
                                               KEY_BLOCK_ALLOCATOR_SIZE_CLASS_MAX,
                                               KEY_BLOCK_ALLOCATOR_SIZE_CLASS_MAX,
                                               &hashCtl,
                                               HASH_ELEM | HASH_BLOBS);
    if (KeyBlockAllocatorFreeLists == NULL) {
        elog(FATAL, "invalid shmem status when creating key block allocator free-list hashtable");
    }
}

static KeyBlockAllocatorFreeListEntry *KeyBlockAllocatorGetFreeList(uint64_t size, bool create)
{
    bool found;
    KeyBlockAllocatorFreeListEntry *entry = hash_search(KeyBlockAllocatorFreeLists,
                                                        &size,
                                                        create ? HASH_ENTER : HASH_FIND,
                                                        &found);
    if (entry != NULL && !found) {
        entry->size = size;
        entry->headIndex = KEY_BLOCK_ALLOCATOR_INVALID_NODE;
        entry->count = 0;
        entry->reclaimEnabled = false;
    }
    return entry;
}

static bool KeyBlockAllocatorPopFreeOffset(uint64_t size, uint64_t *offset)
{
    bool popped = false;

    LWLockAcquire(&KeyBlockAllocatorShmemControl->lock, LW_EXCLUSIVE);

    KeyBlockAllocatorFreeListEntry *entry = KeyBlockAllocatorGetFreeList(size, false);
    if (entry != NULL && entry->count > 0 && entry->headIndex != KEY_BLOCK_ALLOCATOR_INVALID_NODE) {
        int32_t nodeIndex = entry->headIndex;
        KeyBlockAllocatorFreeNode *node = &KeyBlockAllocatorFreeNodes[nodeIndex];

        *offset = node->offset;
        entry->headIndex = node->nextIndex;
        --entry->count;

        node->nextIndex = KeyBlockAllocatorShmemHeader->reusableNodeHead;
        KeyBlockAllocatorShmemHeader->reusableNodeHead = nodeIndex;
        popped = true;
    }

    LWLockRelease(&KeyBlockAllocatorShmemControl->lock);
    return popped;
}

static void KeyBlockAllocatorEnableReclaim(uint64_t size)
{
    LWLockAcquire(&KeyBlockAllocatorShmemControl->lock, LW_EXCLUSIVE);

    KeyBlockAllocatorFreeListEntry *entry = KeyBlockAllocatorGetFreeList(size, true);
    if (entry != NULL) {
        entry->reclaimEnabled = true;
    }

    LWLockRelease(&KeyBlockAllocatorShmemControl->lock);
}

FalconErrorCode KeyBlockAllocatorCreateSize(uint64_t size, uint64_t capacity)
{
    if (size == 0 || capacity < size) {
        return INVALID_PARAMETER;
    }
    return SUCCESS;
}

FalconErrorCode KeyBlockAllocatorAlloc(uint64_t size,
                                       uint64_t *offset,
                                       char **filePath,
                                       uint64_t *capacityOut,
                                       uint32_t *stateOut)
{
    if (size == 0 || offset == NULL) {
        return INVALID_PARAMETER;
    }

    SetUpScanCaches();

    Relation sizeFileRel = table_open(GetRelationOidByName_FALCON(SizeFileTableName), AccessExclusiveLock);
    Oid sizeFileIndexOid = KeyBlockAllocatorTableIndexOid(SizeFileTableName);
    TupleDesc tupleDesc = RelationGetDescr(sizeFileRel);

    ScanKeyData scanKey[LAST_FALCON_SIZE_FILE_TABLE_SCANKEY_TYPE];
    scanKey[SIZE_FILE_TABLE_SIZE_EQ] = SizeFileTableScanKey[SIZE_FILE_TABLE_SIZE_EQ];
    scanKey[SIZE_FILE_TABLE_SIZE_EQ].sk_argument = UInt64GetDatum(size);

    SysScanDesc scanDesc = systable_beginscan(sizeFileRel,
                                              sizeFileIndexOid,
                                              true,
                                              GetTransactionSnapshot(),
                                              LAST_FALCON_SIZE_FILE_TABLE_SCANKEY_TYPE,
                                              scanKey);
    HeapTuple heapTuple = systable_getnext(scanDesc);
    if (!HeapTupleIsValid(heapTuple)) {
        systable_endscan(scanDesc);
        table_close(sizeFileRel, AccessExclusiveLock);
        return FILE_NOT_EXISTS;
    }

    bool isNull;
    uint64_t nextOffset = DatumGetUInt64(heap_getattr(heapTuple,
                                                      Anum_falcon_size_file_table_next_offset,
                                                      tupleDesc,
                                                      &isNull));
    char *path = TextDatumGetCString(heap_getattr(heapTuple,
                                                  Anum_falcon_size_file_table_file_path,
                                                  tupleDesc,
                                                  &isNull));
    uint64_t capacity = DatumGetUInt64(heap_getattr(heapTuple,
                                                    Anum_falcon_size_file_table_capacity,
                                                    tupleDesc,
                                                    &isNull));
    uint32_t state = DatumGetUInt32(heap_getattr(heapTuple,
                                                 Anum_falcon_size_file_table_state,
                                                 tupleDesc,
                                                 &isNull));
    if (nextOffset > capacity || size > capacity - nextOffset) {
        KeyBlockAllocatorEnableReclaim(size);
        if (KeyBlockAllocatorPopFreeOffset(size, offset)) {
            if (filePath != NULL) {
                *filePath = path;
            }
            if (capacityOut != NULL) {
                *capacityOut = capacity;
            }
            if (stateOut != NULL) {
                *stateOut = state;
            }
            systable_endscan(scanDesc);
            table_close(sizeFileRel, AccessExclusiveLock);
            return SUCCESS;
        }
        systable_endscan(scanDesc);
        table_close(sizeFileRel, AccessExclusiveLock);
        return IO_ERROR;
    }

    *offset = nextOffset;
    nextOffset += size;
    if (filePath != NULL) {
        *filePath = path;
    }
    if (capacityOut != NULL) {
        *capacityOut = capacity;
    }
    if (stateOut != NULL) {
        *stateOut = state;
    }

    Datum values[Natts_falcon_size_file_table];
    bool isNulls[Natts_falcon_size_file_table];
    bool updates[Natts_falcon_size_file_table];
    memset(values, 0, sizeof(values));
    memset(isNulls, false, sizeof(isNulls));
    memset(updates, false, sizeof(updates));
    values[Anum_falcon_size_file_table_next_offset - 1] = UInt64GetDatum(nextOffset);
    values[Anum_falcon_size_file_table_update_time - 1] = TimestampTzGetDatum(GetCurrentTimestamp());
    updates[Anum_falcon_size_file_table_next_offset - 1] = true;
    updates[Anum_falcon_size_file_table_update_time - 1] = true;

    HeapTuple updatedTuple = heap_modify_tuple(heapTuple, tupleDesc, values, isNulls, updates);
    CatalogTupleUpdate(sizeFileRel, &updatedTuple->t_self, updatedTuple);
    heap_freetuple(updatedTuple);

    systable_endscan(scanDesc);
    table_close(sizeFileRel, NoLock);
    return SUCCESS;
}

FalconErrorCode KeyBlockAllocatorAbort(uint64_t size, uint64_t offset)
{
    if (size == 0) {
        return INVALID_PARAMETER;
    }

    SetUpScanCaches();

    Relation sizeFileRel = table_open(GetRelationOidByName_FALCON(SizeFileTableName), AccessExclusiveLock);
    Oid sizeFileIndexOid = KeyBlockAllocatorTableIndexOid(SizeFileTableName);
    TupleDesc tupleDesc = RelationGetDescr(sizeFileRel);

    ScanKeyData scanKey[LAST_FALCON_SIZE_FILE_TABLE_SCANKEY_TYPE];
    scanKey[SIZE_FILE_TABLE_SIZE_EQ] = SizeFileTableScanKey[SIZE_FILE_TABLE_SIZE_EQ];
    scanKey[SIZE_FILE_TABLE_SIZE_EQ].sk_argument = UInt64GetDatum(size);

    SysScanDesc scanDesc = systable_beginscan(sizeFileRel,
                                              sizeFileIndexOid,
                                              true,
                                              GetTransactionSnapshot(),
                                              LAST_FALCON_SIZE_FILE_TABLE_SCANKEY_TYPE,
                                              scanKey);
    HeapTuple heapTuple = systable_getnext(scanDesc);
    if (!HeapTupleIsValid(heapTuple)) {
        systable_endscan(scanDesc);
        table_close(sizeFileRel, AccessExclusiveLock);
        return FILE_NOT_EXISTS;
    }

    bool isNull;
    uint64_t nextOffset = DatumGetUInt64(heap_getattr(heapTuple,
                                                      Anum_falcon_size_file_table_next_offset,
                                                      tupleDesc,
                                                      &isNull));
    if (offset <= UINT64_MAX - size && offset + size == nextOffset) {
        Datum values[Natts_falcon_size_file_table];
        bool isNulls[Natts_falcon_size_file_table];
        bool updates[Natts_falcon_size_file_table];
        memset(values, 0, sizeof(values));
        memset(isNulls, false, sizeof(isNulls));
        memset(updates, false, sizeof(updates));
        values[Anum_falcon_size_file_table_next_offset - 1] = UInt64GetDatum(offset);
        values[Anum_falcon_size_file_table_update_time - 1] = TimestampTzGetDatum(GetCurrentTimestamp());
        updates[Anum_falcon_size_file_table_next_offset - 1] = true;
        updates[Anum_falcon_size_file_table_update_time - 1] = true;

        HeapTuple updatedTuple = heap_modify_tuple(heapTuple, tupleDesc, values, isNulls, updates);
        CatalogTupleUpdate(sizeFileRel, &updatedTuple->t_self, updatedTuple);
        heap_freetuple(updatedTuple);
        systable_endscan(scanDesc);
        table_close(sizeFileRel, NoLock);
        return SUCCESS;
    } else {
        (void)KeyBlockAllocatorFree(size, offset);
    }

    systable_endscan(scanDesc);
    table_close(sizeFileRel, AccessExclusiveLock);
    return SUCCESS;
}

FalconErrorCode KeyBlockAllocatorFree(uint64_t size, uint64_t offset)
{
    if (size == 0) {
        return INVALID_PARAMETER;
    }

    LWLockAcquire(&KeyBlockAllocatorShmemControl->lock, LW_EXCLUSIVE);

    KeyBlockAllocatorFreeListEntry *entry = KeyBlockAllocatorGetFreeList(size, true);
    if (entry == NULL) {
        LWLockRelease(&KeyBlockAllocatorShmemControl->lock);
        return IO_ERROR;
    }

    int32_t nodeIndex;
    if (KeyBlockAllocatorShmemHeader->reusableNodeHead != KEY_BLOCK_ALLOCATOR_INVALID_NODE) {
        nodeIndex = KeyBlockAllocatorShmemHeader->reusableNodeHead;
        KeyBlockAllocatorShmemHeader->reusableNodeHead = KeyBlockAllocatorFreeNodes[nodeIndex].nextIndex;
    } else if (KeyBlockAllocatorShmemHeader->allocatedNodeCount < KEY_BLOCK_ALLOCATOR_FREE_NODE_MAX) {
        nodeIndex = KeyBlockAllocatorShmemHeader->allocatedNodeCount++;
    } else {
        LWLockRelease(&KeyBlockAllocatorShmemControl->lock);
        elog(WARNING,
             "key block allocator free-list is full; size=" UINT64_PRINT_SYMBOL " offset=" UINT64_PRINT_SYMBOL
             " is not reusable",
             size,
             offset);
        return SUCCESS;
    }

    KeyBlockAllocatorFreeNode *node = &KeyBlockAllocatorFreeNodes[nodeIndex];
    node->offset = offset;
    node->nextIndex = entry->headIndex;
    entry->headIndex = nodeIndex;
    ++entry->count;

    LWLockRelease(&KeyBlockAllocatorShmemControl->lock);
    return SUCCESS;
}
