// MUST NOT COMPILE: get_xdg_surface's surface argument has no allow-null, so
// it takes NonNull<wl_surface>. A default-constructed Ref is id 0, i.e. null.
#include "common.hpp"
#include <i4/wl/proto/xdg_shell.hpp>

void f(Proxy<xdg_wm_base> wm) {
    (void)wm.send<xdg_wm_base::requests::get_xdg_surface>(Ref<wl_surface>{});
}
