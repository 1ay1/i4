// MUST NOT COMPILE: wl_surface.damage takes four ints (x, y, width, height).
#include "common.hpp"

void f(Proxy<wl_surface> s) {
    s.send<wl_surface::requests::damage>(0, 0);
}
