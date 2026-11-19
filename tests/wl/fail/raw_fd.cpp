// MUST NOT COMPILE: an fd argument takes a BorrowedFd, which carries an
// ownership discipline. A bare int carries none, so it is not accepted.
#include "common.hpp"

void f(Proxy<wl_shm> shm) {
    (void)shm.send<wl_shm::requests::create_pool>(3, 4096);
}
