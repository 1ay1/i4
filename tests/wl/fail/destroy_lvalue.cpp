// MUST NOT COMPILE: destroy() is &&-qualified, so it consumes the handle.
// Calling it on an lvalue would leave a live-looking handle to a dead object.
#include "common.hpp"
#include <i4/wl/affine.hpp>

void f(Unique<wl_surface> s) {
    s.destroy();  // needs std::move(s).destroy()
}
