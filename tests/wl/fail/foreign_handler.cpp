// MUST NOT COMPILE: wl_pointer.motion is not an event of wl_keyboard, so it
// cannot be installed on a keyboard's listener.
#include "common.hpp"

void f(Listener<wl_keyboard>& l) {
    l.on<wl_pointer::events::motion>([](std::uint32_t, fixed, fixed) {});
}
