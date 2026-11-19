// MUST NOT COMPILE: attaching a buffer before acking a configure is an
// xdg-shell protocol error (unconfigured_buffer). The session type makes
// attach() exist only in the Configured state.
#include "common.hpp"
#include <i4/wl/proto/xdg_shell.hpp>
#include <i4/wl/typestate.hpp>

struct P {
    using xdg_surface = proto::xdg_surface;
    using wl_surface = proto::wl_surface;
};

void f(session::XdgSurface<P> fresh, Ref<wl_buffer> buf) {
    fresh.attach(buf);  // still Unconfigured
}
