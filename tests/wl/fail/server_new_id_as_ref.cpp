// MUST NOT COMPILE: a server handler for a request that creates an object
// must take it as NewObject<I>, which knows the version the object speaks.
// Taking the bare id (Ref<I>) is how a compositor ends up binding it at the
// wrong version, so it's refused.
#include "common.hpp"

#include <i4/wl/server/display.hpp>

void f(i4::wl::server::RequestListener<wl_compositor>& l) {
    l.on<wl_compositor::requests::create_surface>([](Ref<wl_surface>) {});
}
