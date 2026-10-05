// EmuCoreA: FileLoader backed by an Android SAF (content://) descriptor.
//
// SAF grants access to the open descriptor, not permission to reopen its
// /proc/self/fd path, so every read goes through pread64 on a private
// duplicate. The duplicate is closed only by this loader, which means the
// caller keeps ownership of the descriptor it passed in and must keep it
// open until the core has shut the session down.
#pragma once

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <string>
#include <utility>

#include "Common/File/Path.h"
#include "Core/Loaders.h"

class FdFileLoader final : public FileLoader {
public:
    FdFileLoader(int fd, std::string pathHint)
        : fd_(fd >= 0 ? dup(fd) : -1), pathHint_(std::move(pathHint)) {
        struct stat st {};
        if (fd_ >= 0 && fstat(fd_, &st) == 0 && S_ISREG(st.st_mode)) {
            size_ = st.st_size;
        }
        if (pathHint_.empty()) pathHint_ = "saf-image";
    }

    ~FdFileLoader() override {
        if (fd_ >= 0) close(fd_);
    }

    bool Exists() override { return fd_ >= 0 && size_ > 0; }
    bool IsDirectory() override { return false; }
    s64 FileSize() override { return size_; }
    Path GetPath() const override { return Path(pathHint_); }

    size_t ReadAt(s64 absolutePos, size_t bytes, size_t count, void *data,
                  Flags flags = Flags::NONE) override {
        if (fd_ < 0 || data == nullptr || bytes == 0 || count > SIZE_MAX / bytes) return 0;
        if (absolutePos < 0 || absolutePos >= size_) return 0;
        const size_t requested =
            std::min<size_t>(bytes * count, static_cast<size_t>(size_ - absolutePos));
        size_t done = 0;
        while (done < requested) {
            const ssize_t n = pread64(fd_, static_cast<uint8_t *>(data) + done, requested - done,
                                      static_cast<off64_t>(absolutePos + done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            done += static_cast<size_t>(n);
        }
        return done / bytes;
    }

private:
    int fd_;
    s64 size_ = 0;
    std::string pathHint_;
};
