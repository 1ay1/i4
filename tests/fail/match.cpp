// match() is exhaustive: an unhandled alternative does not compile.
#include <string>
#include <variant>

#include "i4/base/match.hpp"

int main() {
    const std::variant<int, std::string> v = 1;
    return i4::match(
        v, [](int i) { return i - 1; }
#ifndef I4_FAIL
        ,
        [](const std::string& s) { return static_cast<int>(s.size()); }
#endif
    );
}
