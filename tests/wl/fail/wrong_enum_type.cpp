// MUST NOT COMPILE: the last argument is wl_shm::format, not a raw integer.
// The protocol's enums are distinct types, so 42 is not a pixel format.
#include "common.hpp"

void f(Proxy<wl_shm_pool> p) {
    p.send<wl_shm_pool::requests::create_buffer>(0, 1, 1, 4, 42);
}
