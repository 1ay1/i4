// MUST NOT COMPILE: a negative extent, proven wrong at compile time.
// Positive<T>::of<V> is consteval, so the check happens during compilation and
// costs nothing at runtime.
#include "common.hpp"
#include <i4/wl/refine.hpp>

void f() {
    (void)Positive<std::int32_t>::of<-5>();
}
