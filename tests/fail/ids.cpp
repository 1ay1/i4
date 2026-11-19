// A container id is not a window id.
#include "i4/wm/state.hpp"

using namespace i4::wm;

static bool closes(WindowId w) { return w.value != 0; }

int main() {
#ifdef I4_FAIL
    return closes(ConId{1}) ? 0 : 1;
#else
    return closes(WindowId{1}) ? 0 : 1;
#endif
}
