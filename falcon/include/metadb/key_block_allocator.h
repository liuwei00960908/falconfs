/* Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * SPDX-License-Identifier: MulanPSL-2.0
 */

#ifndef FALCON_KEY_BLOCK_ALLOCATOR_H
#define FALCON_KEY_BLOCK_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

#include "utils/error_code.h"

/*
 * Phase 1 allocator: centralizes the existing append-only allocation path.
 * Later phases add shared free_lists, reclaim, and rebuild behind this API.
 */
FalconErrorCode KeyBlockAllocatorCreateSize(uint64_t size, uint64_t capacity);
FalconErrorCode KeyBlockAllocatorAlloc(uint64_t size,
                                       uint64_t *offset,
                                       char **filePath,
                                       uint64_t *capacity,
                                       uint32_t *state);
FalconErrorCode KeyBlockAllocatorAbort(uint64_t size, uint64_t offset);
FalconErrorCode KeyBlockAllocatorFree(uint64_t size, uint64_t offset);

size_t KeyBlockAllocatorShmemsize(void);
void KeyBlockAllocatorShmemInit(void);

#endif
