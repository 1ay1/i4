// MUST NOT COMPILE: a Unique handle is move-only. Copying it would mean two
// owners racing to send the same destructor.
#include "common.hpp"
#include <i4/wl/affine.hpp>

void f(Unique<wl_surface>& s) {
    Unique<wl_surface> copy = s;
    (void)copy;
}
