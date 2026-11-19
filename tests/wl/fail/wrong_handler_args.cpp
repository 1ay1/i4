// MUST NOT COMPILE: wl_registry.global carries (uint name, string interface,
// uint version). A handler taking a single int cannot receive it.
#include "common.hpp"

void f(Listener<wl_registry>& l) {
    l.on<wl_registry::events::global>([](int) {});
}
