// MUST NOT COMPILE: a client cannot send an event. Direction is part of the
// interface's structure, so this is a category error rather than a runtime
// protocol violation.
#include "common.hpp"
#include <i4/wl/role.hpp>

static_assert(sends<ClientRole, wl_surface, wl_surface::events::enter>,
              "a client cannot send wl_surface.enter");

void f() {}
