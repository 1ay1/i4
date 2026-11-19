// fd.hpp — file descriptors as a linear resource.
//
// Wayland passes fds over SCM_RIGHTS. The C type is `int`, which owns nothing,
// converts from anything, and makes double-close and use-after-close invisible.
// `Fd` is a move-only owner and `BorrowedFd` is a non-owning view, so the
// protocol's fd arguments can say which one they mean.
//
// The algebra's `Fd` atom lowers to `BorrowedFd`: sending a descriptor does
// not transfer ownership (the kernel duplicates it into the socket), so the
// caller still closes their copy. That is the actual semantics, now written
// in a type rather than in a comment nobody reads.
//
// Both types are JAAL'S. They used to be loom's own, written out here so the
// library depended on nothing — and heddle had another copy, and shed passed
// bare ints. A descriptor opened by shed, watched by jaal and sent over a
// Wayland socket by loom crossed three incompatible wrappers, and every
// crossing was somewhere to close it twice. One type, one close.
#pragma once

#include <jaal/platform/handle.hpp>

namespace i4::wl::os {

/// A non-owning descriptor. Cannot be closed through this type.
using BorrowedFd = jaal::platform::borrowed_handle;

/// An owning descriptor: move-only, closed exactly once.
using OwnedFd = jaal::platform::owned_handle;

}  // namespace i4::wl::os

namespace i4::wl {
using os::BorrowedFd;
using os::OwnedFd;
}  // namespace i4::wl
