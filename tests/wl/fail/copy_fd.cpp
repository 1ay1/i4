// MUST NOT COMPILE: an OwnedFd is move-only. Copying it would mean two owners
// each closing the same descriptor.
#include "common.hpp"

void f(OwnedFd& fd) {
    OwnedFd copy = fd;
    (void)copy;
}
