// MUST NOT COMPILE: create_surface's only argument is the new_id, which the
// library supplies. The caller passes nothing at all.
#include "common.hpp"

void f(Proxy<wl_compositor> c) {
    (void)c.send<wl_compositor::requests::create_surface>(1);
}
