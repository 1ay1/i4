// Shared preamble for the compile-fail cases. Each case file must fail for its
// own semantic reason, so this header itself must always compile cleanly.
#pragma once

#include <i4/wl/proto/wayland.hpp>
#include <i4/wl/registry.hpp>

using namespace i4::wl;
using namespace i4::wl::proto;

struct Core {
    using wl_display = proto::wl_display;
    using wl_registry = proto::wl_registry;
};
