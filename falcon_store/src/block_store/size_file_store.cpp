/* Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * SPDX-License-Identifier: MulanPSL-2.0
 */

#include "block_store/size_file_store.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <unordered_map>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "remote_connection_utils/error_code_def.h"

namespace {
class ThreadLocalFdCache {
  public:
    ThreadLocalFdCache() = default;
    ThreadLocalFdCache(const ThreadLocalFdCache &) = delete;
    ThreadLocalFdCache &operator=(const ThreadLocalFdCache &) = delete;

    ~ThreadLocalFdCache()
    {
        for (auto &entry : fds_) {
            close(entry.second);
        }
    }

    int Get(const std::string &path)
    {
        auto iter = fds_.find(path);
        if (iter != fds_.end()) {
            return iter->second;
        }

        int fd = open(path.c_str(), O_RDWR);
        if (fd < 0) {
            return -1;
        }
        fds_.emplace(path, fd);
        return fd;
    }

    void Invalidate(const std::string &path)
    {
        auto iter = fds_.find(path);
        if (iter == fds_.end()) {
            return;
        }
        close(iter->second);
        fds_.erase(iter);
    }

  private:
    std::unordered_map<std::string, int> fds_;
};

thread_local ThreadLocalFdCache SizeFileFdCache;

std::string ResolvePath(const std::string &filePath)
{
    if (filePath.empty() || filePath.front() == '/') {
        return filePath;
    }

    const char *baseDir = std::getenv("FALCON_BLOCK_DATA_DIR");
    if (baseDir == nullptr || baseDir[0] == '\0') {
        return filePath;
    }

    std::filesystem::path resolved(baseDir);
    resolved /= filePath;
    return resolved.string();
}

int WriteFull(int fd, const char *buffer, uint64_t size, uint64_t offset)
{
    uint64_t written = 0;
    while (written < size) {
        size_t chunk = static_cast<size_t>(std::min<uint64_t>(size - written, std::numeric_limits<size_t>::max()));
        ssize_t ret = pwrite(fd, buffer + written, chunk, static_cast<off_t>(offset + written));
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return IO_ERROR;
        }
        if (ret == 0) {
            return IO_ERROR;
        }
        written += static_cast<uint64_t>(ret);
    }
    return SUCCESS;
}

int ReadFull(int fd, char *buffer, uint64_t size, uint64_t offset)
{
    uint64_t readBytes = 0;
    while (readBytes < size) {
        size_t chunk = static_cast<size_t>(std::min<uint64_t>(size - readBytes, std::numeric_limits<size_t>::max()));
        ssize_t ret = pread(fd, buffer + readBytes, chunk, static_cast<off_t>(offset + readBytes));
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return IO_ERROR;
        }
        if (ret == 0) {
            return IO_ERROR;
        }
        readBytes += static_cast<uint64_t>(ret);
    }
    return SUCCESS;
}

bool ShouldRetryCachedFd(int error)
{
    return error == EBADF || error == ENOENT || error == ESTALE || error == EIO;
}

int WriteWithCachedFd(const std::string &resolved, const char *buffer, uint64_t size, uint64_t offset)
{
    int fd = SizeFileFdCache.Get(resolved);
    if (fd < 0) {
        return IO_ERROR;
    }

    errno = 0;
    int ret = WriteFull(fd, buffer, size, offset);
    int savedErrno = errno;
    if (ret == SUCCESS || !ShouldRetryCachedFd(savedErrno)) {
        return ret;
    }

    SizeFileFdCache.Invalidate(resolved);
    fd = SizeFileFdCache.Get(resolved);
    if (fd < 0) {
        return IO_ERROR;
    }
    return WriteFull(fd, buffer, size, offset);
}

int ReadWithCachedFd(const std::string &resolved, char *buffer, uint64_t size, uint64_t offset)
{
    int fd = SizeFileFdCache.Get(resolved);
    if (fd < 0) {
        return IO_ERROR;
    }

    errno = 0;
    int ret = ReadFull(fd, buffer, size, offset);
    int savedErrno = errno;
    if (ret == SUCCESS || !ShouldRetryCachedFd(savedErrno)) {
        return ret;
    }

    SizeFileFdCache.Invalidate(resolved);
    fd = SizeFileFdCache.Get(resolved);
    if (fd < 0) {
        return IO_ERROR;
    }
    return ReadFull(fd, buffer, size, offset);
}
} // namespace

int SizeFileStore::CreateSizeFile(const std::string &filePath, uint64_t capacity)
{
    std::string resolved = ResolvePath(filePath);
    if (resolved.empty() || capacity > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        return INVALID_PARAMETER;
    }

    std::filesystem::path path(resolved);
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return IO_ERROR;
        }
    }

    int fd = open(resolved.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return IO_ERROR;
    }

    int ret = SUCCESS;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        ret = IO_ERROR;
    } else if (static_cast<uint64_t>(st.st_size) < capacity && ftruncate(fd, static_cast<off_t>(capacity)) != 0) {
        ret = IO_ERROR;
    }

    close(fd);
    return ret;
}

int SizeFileStore::Write(const std::string &filePath, uint64_t offset, const char *buffer, uint64_t size)
{
    std::string resolved = ResolvePath(filePath);
    if (resolved.empty() || buffer == nullptr || offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        return INVALID_PARAMETER;
    }

    return WriteWithCachedFd(resolved, buffer, size, offset);
}

int SizeFileStore::Read(const std::string &filePath, uint64_t offset, char *buffer, uint64_t size)
{
    std::string resolved = ResolvePath(filePath);
    if (resolved.empty() || buffer == nullptr || offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        return INVALID_PARAMETER;
    }

    return ReadWithCachedFd(resolved, buffer, size, offset);
}
