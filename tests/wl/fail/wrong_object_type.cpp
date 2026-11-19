// MUST NOT COMPILE: wl_surface.attach takes a wl_buffer. A wl_output is a
// different interface, so its Ref is a different type.
#include "common.hpp"

void f(Proxy<wl_surface> s, Ref<wl_output> screen) {
    s.send<wl_surface::requests::attach>(screen, 0, 0);
}
