// MUST NOT COMPILE: downgrade() may only lower a version. Raising one would
// conjure capabilities the compositor never granted.
#include "common.hpp"

void f(Proxy<wl_surface, 2> s) {
    (void)s.downgrade<6>();
}
