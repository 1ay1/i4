// MUST NOT COMPILE: a Serial must come from an event. There is no constructor
// that takes a literal, so an invented serial cannot be built.
#include "common.hpp"
#include <i4/wl/refine.hpp>

void f() {
    Serial s{12345};
    (void)s;
}
