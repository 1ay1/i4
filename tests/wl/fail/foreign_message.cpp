// MUST NOT COMPILE: commit belongs to wl_surface. An interface may only be
// sent the messages it declares.
#include "common.hpp"

void f(Proxy<wl_output> o) {
    o.send<wl_surface::requests::commit>();
}
