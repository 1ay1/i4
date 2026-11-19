// src/server/syncobj.cpp — the four ioctls linux-drm-syncobj needs.
#include "syncobj.hpp"

#include <cerrno>

#include <drm/drm.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace i4::server {

namespace {

int ioctl_retry(int fd, unsigned long request, void* arg) {
    int r;
    do r = ::ioctl(fd, request, arg);
    while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

}  // namespace

Syncobjs::Syncobjs() {
    for (const char* node : {"/dev/dri/renderD128", "/dev/dri/renderD129", "/dev/dri/renderD130"}) {
        wl::OwnedFd fd{::open(node, O_RDWR | O_CLOEXEC)};
        if (!fd.valid()) continue;
        drm_get_cap cap{DRM_CAP_SYNCOBJ_TIMELINE, 0};
        if (ioctl_retry(fd.get(), DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value != 0) {
            drm_ = std::move(fd);
            return;
        }
    }
}

Syncobjs::~Syncobjs() = default;

std::uint32_t Syncobjs::import(int fd) {
    if (!drm_.valid() || fd < 0) return 0;
    drm_syncobj_handle h{};
    h.fd = fd;
    if (ioctl_retry(drm_.get(), DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &h) != 0) return 0;
    return h.handle;
}

void Syncobjs::destroy(std::uint32_t handle) {
    if (!drm_.valid() || handle == 0) return;
    drm_syncobj_destroy d{handle, 0};
    (void)ioctl_retry(drm_.get(), DRM_IOCTL_SYNCOBJ_DESTROY, &d);
}

std::optional<wl::OwnedFd> Syncobjs::when(std::uint32_t handle, std::uint64_t point) {
    if (!drm_.valid() || handle == 0) return std::nullopt;
    wl::OwnedFd ev{::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
    if (!ev.valid()) return std::nullopt;
    drm_syncobj_eventfd e{};
    e.handle = handle;
    // Wait for the point to have a fence AND for that fence to signal (a
    // client may commit before it has even submitted the work).
    e.flags = 0;
    e.point = point;
    e.fd = ev.get();
    if (ioctl_retry(drm_.get(), DRM_IOCTL_SYNCOBJ_EVENTFD, &e) != 0) return std::nullopt;
    return ev;
}

void Syncobjs::signal(std::uint32_t handle, std::uint64_t point) {
    if (!drm_.valid() || handle == 0) return;
    drm_syncobj_timeline_array a{};
    a.handles = reinterpret_cast<std::uintptr_t>(&handle);
    a.points = reinterpret_cast<std::uintptr_t>(&point);
    a.count_handles = 1;
    (void)ioctl_retry(drm_.get(), DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &a);
}

}  // namespace i4::server
