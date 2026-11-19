// tests/test_wire.cpp — the codec: round trips, canonical bytes, totality.
#include "check.hpp"
#include "i4/link/predict.hpp"
#include "i4/link/protocol.hpp"
#include "i4/wm/bindings.hpp"
#include "session.hpp"

using namespace i4;
using namespace i4::link;

namespace {

template <class T>
std::vector<std::byte> bytes_of(std::initializer_list<int> xs) {
    std::vector<std::byte> out;
    for (int x : xs) out.push_back(static_cast<std::byte>(x));
    return out;
}

enum class Colour : std::uint8_t { red, green, blue };

struct Inner {
    std::int32_t x;
    Colour c;
    friend bool operator==(const Inner&, const Inner&) = default;
};

struct Outer {
    std::string name;
    std::vector<Inner> items;
    std::optional<std::uint64_t> big;
    std::map<std::uint32_t, std::string> by_id;
    std::variant<bool, Inner, std::string> any;
    friend bool operator==(const Outer&, const Outer&) = default;
};

// What the wire refuses, refused at compile time.
struct HasFloat { float f; };
struct HasPointer { int* p; };
enum Unscoped { u0 };
static_assert(!Wire<HasFloat>);
static_assert(!Wire<HasPointer>);
static_assert(!Wire<double>);
static_assert(!Wire<Unscoped>);
static_assert(Wire<Outer>);

// Shape, not names: renaming changes nothing, reordering or retyping does.
struct A1 { std::uint32_t a; std::string b; };
struct A2 { std::uint32_t renamed; std::string also; };
struct A3 { std::string b; std::uint32_t a; };
struct A4 { std::uint64_t a; std::string b; };
static_assert(fingerprint<A1> == fingerprint<A2>);
static_assert(fingerprint<A1> != fingerprint<A3>);
static_assert(fingerprint<A1> != fingerprint<A4>);

// Adding an enumerator changes the shape.
enum class E3 : std::uint8_t { a, b, c };
enum class E4 : std::uint8_t { a, b, c, d };
static_assert(fingerprint<E3> != fingerprint<E4>);

}  // namespace

TEST(round_trip) {
    const Outer o{"hi", {{-5, Colour::blue}, {1 << 30, Colour::red}}, 1ull << 63, {{3, "c"}, {9, "nine"}},
                  Inner{-1, Colour::green}};
    const auto b = encode(o);
    const auto back = decode<Outer>(b);
    CHECK(back.has_value());
    CHECK(back && *back == o);
}

TEST(integers_are_compact) {
    CHECK(encode(std::uint32_t{0}) == bytes_of<int>({0}));
    CHECK(encode(std::uint32_t{300}) == bytes_of<int>({0xac, 0x02}));
    CHECK(encode(std::int32_t{-1}) == bytes_of<int>({0x01}));  // zigzag
    CHECK(encode(std::int32_t{1}) == bytes_of<int>({0x02}));
}

TEST(decoding_is_total) {
    CHECK(decode<std::uint8_t>(bytes_of<int>({0x80, 0x02})).error() == WireError::out_of_range);
    CHECK(decode<std::uint32_t>(bytes_of<int>({0x80})).error() == WireError::truncated);
    CHECK(decode<std::uint32_t>(bytes_of<int>({0x80, 0x00})).error() == WireError::overlong);
    CHECK(decode<bool>(bytes_of<int>({2})).error() == WireError::bad_bool);
    CHECK(decode<Colour>(bytes_of<int>({3})).error() == WireError::bad_enum);
    CHECK((decode<std::variant<bool, std::string>>(bytes_of<int>({2, 0})).error() == WireError::bad_variant));
    CHECK(decode<std::uint32_t>(bytes_of<int>({1, 0})).error() == WireError::trailing);
    // A count larger than the bytes behind it: refused before allocating.
    CHECK(decode<std::string>(bytes_of<int>({0xff, 0xff, 0xff, 0x7f})).error() == WireError::too_large);
    // Map keys must rise: the same map has one spelling.
    CHECK((decode<std::map<std::uint32_t, bool>>(bytes_of<int>({2, 5, 1, 3, 0})).error() == WireError::not_canonical));
}

TEST(random_bytes_never_crash_and_what_decodes_is_canonical) {
    // Every byte string decodes to a valid value or is refused; whatever
    // decodes re-encodes to the same bytes.
    std::mt19937 rng(7);
    int decoded = 0;
    for (int i = 0; i < 200000; ++i) {
        std::vector<std::byte> b(rng() % 24);
        for (auto& x : b) x = static_cast<std::byte>(rng() % 8 == 0 ? rng() : rng() % 6);
        if (auto v = decode<Outer>(b)) {
            ++decoded;
            if (encode(*v) != b) {
                i4::test::fail("decoded but not canonical");
                return;
            }
        }
        if (auto v = decode<Up>(b); v && encode(*v) != b) {
            i4::test::fail("Up decoded but not canonical");
            return;
        }
    }
    CHECK(decoded > 0);
}

TEST(whole_window_manager_states_round_trip) {
    // The replica crosses the wire whole at Welcome: every reachable State
    // must survive it exactly.
    for (unsigned seed = 1; seed <= 4; ++seed) {
        i4::test::Generator gen(seed);
        wm::State s;
        (void)wm::apply(s, wm::ev::OutputAdded{wm::OutputInfo{"A", {0, 0, 1000, 800}}});
        for (int i = 0; i < 400; ++i) {
            auto [e, note] = gen.next(s);
            const auto eb = encode(e);
            auto eback = decode<wm::Event>(eb);
            if (!eback || !(*eback == e)) {
                i4::test::fail("event did not round-trip: " + note);
                return;
            }
            (void)wm::apply(s, e);
            if (i % 50 == 0) {
                auto back = decode<wm::State>(encode(s));
                if (!back || !(*back == s)) {
                    i4::test::fail("state did not round-trip at seed " + std::to_string(seed));
                    return;
                }
            }
        }
    }
}

TEST(refinement_types_refuse_what_they_forbid) {
    // Mods has four enumerators, so bit 4 is not a modifier.
    CHECK(decode<wm::Mods>(bytes_of<int>({0x10})).error() == WireError::invalid);
    CHECK(decode<wm::Mods>(bytes_of<int>({0x09})).value() == (wm::Mod::shift | wm::Mod::super));
}

TEST(protocol_is_fingerprinted) {
    CHECK(protocol != 0);
    CHECK(Replica::hello() == Up{up::Hello{protocol}});
}
