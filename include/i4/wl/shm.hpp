// shm.hpp — anonymous shared memory buffers.
//
// The one piece of Wayland that isn't protocol: you need a file descriptor
// backed by memory both you and the compositor can map. Kept generic over the
// protocol namespace so it works with any generated wl_shm.
#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "listener.hpp"

#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace i4::wl {

/// A writable pixel buffer shared with the compositor.
class ShmPool {
public:
    ShmPool() = default;
    ~ShmPool() { reset(); }

    ShmPool(ShmPool&& o) noexcept { *this = std::move(o); }
    ShmPool& operator=(ShmPool&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::move(o.fd_);
            data_ = std::exchange(o.data_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }
    ShmPool(const ShmPool&) = delete;
    ShmPool& operator=(const ShmPool&) = delete;

    /// Create an anonymous, sealed, `size`-byte region.
    static ShmPool create(std::size_t size) {
        ShmPool p;
        int raw = -1;

#if defined(__linux__) && defined(SYS_memfd_create)
        raw = static_cast<int>(::syscall(SYS_memfd_create, "i4-wl-pool",
                                         MFD_CLOEXEC | MFD_ALLOW_SEALING));
#endif
        if (raw < 0) {
            // Portable fallback: an unlinked file in the runtime dir.
            const char* runtime = std::getenv("XDG_RUNTIME_DIR");
            std::string tmpl = std::string(runtime ? runtime : "/tmp") + "/i4-wl-XXXXXX";
            std::vector<char> path(tmpl.begin(), tmpl.end());
            path.push_back('\0');
            raw = ::mkstemp(path.data());
            if (raw >= 0) ::unlink(path.data());
        }
        if (raw < 0) return p;

        OwnedFd fd{raw};  // owned from here on: every early return closes it
        if (::ftruncate(fd.get(), static_cast<off_t>(size)) < 0) return p;

        void* m = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
        if (m == MAP_FAILED) return p;

        p.fd_ = std::move(fd);
        p.data_ = static_cast<std::byte*>(m);
        p.size_ = size;
        return p;
    }

    void reset() {
        if (data_) ::munmap(data_, size_);
        data_ = nullptr;
        fd_.reset();
        size_ = 0;
    }

    /// Lend the descriptor to wl_shm.create_pool. The compositor maps its own
    /// copy; ours stays ours.
    BorrowedFd fd() const { return fd_.borrow(); }
    std::size_t size() const { return size_; }
    std::byte* data() { return data_; }
    std::uint32_t* pixels() { return reinterpret_cast<std::uint32_t*>(data_); }
    explicit operator bool() const { return data_ != nullptr; }

private:
    OwnedFd fd_{};
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace i4::wl
