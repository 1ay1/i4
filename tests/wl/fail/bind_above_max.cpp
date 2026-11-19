// MUST NOT COMPILE: wl_compositor's definition tops out far below 99. Asking
// the compositor for a version the protocol does not define is a bug in the
// program, not a runtime negotiation failure.
#include "common.hpp"

void f(const Registry<Core>& r) {
    (void)r.bind<wl_compositor, 99>();
}
