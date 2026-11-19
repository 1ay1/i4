#pragma once
// src/server/syncobj.hpp — DRM timeline syncobjs, for linux-drm-syncobj.
//
// The session, not a head, keeps a client's explicit-sync promises:
//
//   acquire  the client says "my buffer is ready when timeline T reaches A".
//            i4d registers an eventfd for that point (no thread, no CPU wait:
//            the kernel wakes the loop when the GPU gets there) and only then
//            treats the commit as content. A frame is never shown, nor sent
//            across a network, before it is finished.
//   release  "tell me on timeline T at R when you are done with it". i4d
//            signals R when every head has released the frame: the same
//            moment it would otherwise send wl_buffer.release.
//
// Heads never see a client's timelines: a remote head could not use them,
// and drivers commonly refuse to import a syncobj another driver created
// (NVIDIA's Vulkan rejects an OPAQUE_FD import of one, which once left
// clients blocked forever waiting for a release no one could signal).

#include <cstdint>
#include <optional>

#include <i4/wl/fd.hpp>

namespace i4::server {

class Syncobjs {
public:
    /// Opens the render node (once). Without one, explicit sync is off.
    Syncobjs();
    Syncobjs(const Syncobjs&) = delete;
    Syncobjs& operator=(const Syncobjs&) = delete;
    ~Syncobjs();

    [[nodiscard]] bool available() const noexcept { return drm_.valid(); }

    /// A client's timeline fd, as a syncobj handle on our device; 0 = refused.
    [[nodiscard]] std::uint32_t import(int fd);
    void destroy(std::uint32_t handle);

    /// An eventfd that becomes readable when `point` is signalled (or is
    /// already: then it is readable at once). nullopt: the kernel refused.
    [[nodiscard]] std::optional<wl::OwnedFd> when(std::uint32_t handle, std::uint64_t point);
    /// Signal `point` from the CPU (the GPU is long done with the buffer).
    void signal(std::uint32_t handle, std::uint64_t point);

private:
    wl::OwnedFd drm_;
};

}  // namespace i4::server
