// MUST NOT COMPILE: wl_surface.offset is since 5; this surface is bound at 4.
#include "common.hpp"

void f(Proxy<wl_surface, 4> s) {
    s.send<wl_surface::requests::offset>(1, 1);
}
