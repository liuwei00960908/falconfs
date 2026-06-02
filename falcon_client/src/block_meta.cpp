/* Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * SPDX-License-Identifier: MulanPSL-2.0
 */

#include "block_meta.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "block_store/size_file_store.h"
#include "falcon_meta.h"
#include "remote_connection_utils/error_code_def.h"

namespace {
constexpr uint64_t DEFAULT_BLOCK_FILE_CAPACITY = 20ULL * 1024 * 1024 * 1024 * 1024;

std::shared_ptr<Connection> GetBlockMetaConnection()
{
    if (!router) {
        return nullptr;
    }
    return router->GetCoordinatorConn();
}

void FillStatResult(const Connection::BlockLocationResult &location, FalconBlockStatResult *result)
{
    if (result == nullptr) {
        return;
    }
    result->key = location.key;
    result->size = location.size;
    result->offset = location.offset;
    result->filePath = location.filePath;
    result->atime = location.atime;
    result->mtime = location.mtime;
    result->ctime = location.ctime;
    result->version = location.version;
    result->state = location.state;
}

uint64_t DefaultSizeFileCapacity(uint64_t size)
{
    uint64_t capacity = DEFAULT_BLOCK_FILE_CAPACITY;
    const char *envCapacity = std::getenv("FALCON_BLOCK_FILE_CAPACITY");
    if (envCapacity != nullptr && envCapacity[0] != '\0') {
        errno = 0;
        char *end = nullptr;
        unsigned long long parsed = std::strtoull(envCapacity, &end, 10);
        if (errno == 0 && end != envCapacity && *end == '\0') {
            capacity = parsed;
        }
    }
    return std::max(capacity, size);
}

int EnsureSizeFile(const std::shared_ptr<Connection> &conn, uint64_t size)
{
    if (!conn) {
        return PROGRAM_ERROR;
    }
    if (size == 0) {
        return INVALID_PARAMETER;
    }

    uint64_t capacity = DefaultSizeFileCapacity(size);
    int ret = conn->SizeFileCreate(size, capacity);
    if (ret != SUCCESS && ret != FILE_EXISTS) {
        return ret;
    }

    Connection::SizeFileResult sizeFile;
    ret = conn->SizeFileStat(size, sizeFile);
    if (ret != SUCCESS) {
        return ret;
    }
    return SizeFileStore::CreateSizeFile(sizeFile.filePath, sizeFile.capacity);
}

int WriteExistingBlock(const std::shared_ptr<Connection> &conn,
                       const std::string &key,
                       const char *buffer,
                       size_t size,
                       const Connection::BlockLocationResult &location)
{
    if (location.size != size) {
        return INVALID_PARAMETER;
    }
    int ret = SizeFileStore::Write(location.filePath, location.offset, buffer, location.size);
    if (ret != SUCCESS) {
        return ret;
    }
    return conn->BlockUpdate(key.c_str());
}

std::shared_ptr<std::mutex> GetKeyMutex(const std::string &key)
{
    static std::mutex mapMutex;
    static std::unordered_map<std::string, std::weak_ptr<std::mutex>> keyMutexes;

    std::lock_guard<std::mutex> guard(mapMutex);
    auto &weakMutex = keyMutexes[key];
    std::shared_ptr<std::mutex> keyMutex = weakMutex.lock();
    if (keyMutex == nullptr) {
        keyMutex = std::make_shared<std::mutex>();
        weakMutex = keyMutex;
    }
    return keyMutex;
}
} // namespace

int FalconBlockPut(const std::string &key, const char *buffer, size_t size)
{
    auto conn = GetBlockMetaConnection();
    if (!conn) {
        return PROGRAM_ERROR;
    }
    if (key.empty() || buffer == nullptr || size == 0) {
        return INVALID_PARAMETER;
    }

    std::shared_ptr<std::mutex> keyMutex = GetKeyMutex(key);
    std::lock_guard<std::mutex> keyGuard(*keyMutex);

    Connection::BlockLocationResult location;
    int ret = conn->BlockGet(key.c_str(), location);
    if (ret == SUCCESS) {
        return WriteExistingBlock(conn, key, buffer, size, location);
    }
    if (ret != FILE_NOT_EXISTS) {
        return ret;
    }

    ret = conn->BlockAlloc(size, location);
    if (ret == FILE_NOT_EXISTS) {
        ret = EnsureSizeFile(conn, size);
        if (ret != SUCCESS) {
            return ret;
        }
        ret = conn->BlockAlloc(size, location);
    }
    if (ret != SUCCESS) {
        return ret;
    }

    ret = EnsureSizeFile(conn, size);
    if (ret != SUCCESS) {
        conn->BlockAbortAlloc(location.size, location.offset);
        return ret;
    }

    ret = SizeFileStore::Write(location.filePath, location.offset, buffer, location.size);
    if (ret != SUCCESS) {
        conn->BlockAbortAlloc(location.size, location.offset);
        return ret;
    }

    ret = conn->BlockInsert(key.c_str(), location.size, location.offset);
    if (ret != SUCCESS) {
        conn->BlockAbortAlloc(location.size, location.offset);
        if (ret == FILE_EXISTS) {
            Connection::BlockLocationResult existing;
            int getRet = conn->BlockGet(key.c_str(), existing);
            if (getRet == SUCCESS) {
                return WriteExistingBlock(conn, key, buffer, size, existing);
            }
        }
    }
    return ret;
}

int FalconBlockGet(const std::string &key, char *buffer, size_t bufferSize)
{
    auto conn = GetBlockMetaConnection();
    if (!conn) {
        return PROGRAM_ERROR;
    }
    if (key.empty() || buffer == nullptr) {
        return INVALID_PARAMETER;
    }

    Connection::BlockLocationResult location;
    int ret = conn->BlockGet(key.c_str(), location);
    if (ret != SUCCESS) {
        return ret;
    }
    if (bufferSize < location.size) {
        return INVALID_PARAMETER;
    }
    return SizeFileStore::Read(location.filePath, location.offset, buffer, location.size);
}

int FalconBlockDel(const std::string &key)
{
    auto conn = GetBlockMetaConnection();
    if (!conn) {
        return PROGRAM_ERROR;
    }
    if (key.empty()) {
        return INVALID_PARAMETER;
    }
    return conn->BlockDel(key.c_str());
}

int FalconBlockStat(const std::string &key, FalconBlockStatResult *result)
{
    auto conn = GetBlockMetaConnection();
    if (!conn) {
        return PROGRAM_ERROR;
    }
    if (key.empty() || result == nullptr) {
        return INVALID_PARAMETER;
    }

    Connection::BlockLocationResult location;
    int ret = conn->BlockStat(key.c_str(), location);
    if (ret != SUCCESS) {
        return ret;
    }
    FillStatResult(location, result);
    return SUCCESS;
}
