// A point in one coordinate space is not a point in another.
#include "i4/base/geom.hpp"

using namespace i4;

int main() {
    const Point<space::Surface> p{1, 2};
#ifdef I4_FAIL
    const Point<space::Desktop> q = p;
#else
    const Point<space::Surface> q = p;
#endif
    return q.x - 1;
}
