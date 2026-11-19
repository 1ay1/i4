// A hand-written smoke test: no framework, exit code is the result.
#include <i4/wl/listener.hpp>
#include <i4/wl/proto/wayland.hpp>

#include <cstdio>
#include <string>

using namespace i4::wl;
using namespace i4::wl::proto;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

// ===========================================================================
// 1. The algebra is correct at compile time. These assertions ARE the test:
//    if this file compiles, the protocol's shape has been proven.
// ===========================================================================

// The XML was read correctly.
static_assert(wl_surface::wl_name == "wl_surface");
static_assert(wl_compositor::requests::create_surface::opcode == 0);
static_assert(wl_display::requests::get_registry::opcode == 1);
static_assert(wl_surface::requests::offset::since == 5);

// A request's return type is *derived* from its signature, not written by hand.
static_assert(creates_object_v<wl_compositor::requests::create_surface::sig>);
static_assert(!creates_object_v<wl_surface::requests::commit::sig>);
static_assert(std::is_same_v<new_id_arg_t<wl_compositor::requests::create_surface::sig>,
                             NewId<wl_surface>>);

// Version inheritance is a type-level function: a surface from a compositor
// bound at 4 is a version-4 surface, even though wl_surface's max is higher.
static_assert(std::is_same_v<send_result_t<wl_compositor::requests::create_surface, 4>,
                             Proxy<wl_surface, 4>>);
static_assert(std::is_same_v<send_result_t<wl_compositor::requests::create_surface, 6>,
                             Proxy<wl_surface, 6>>);

// ...and it saturates at the interface maximum, never above it.
static_assert(inherited_version<wl_callback, 99> == wl_callback::wl_version);

// Version gating. `offset` is since 5.
static_assert(Proxy<wl_surface, 6>::supports<wl_surface::requests::offset>);
static_assert(!Proxy<wl_surface, 4>::supports<wl_surface::requests::offset>);

// The caller's parameter list drops new_id arguments.
//   create_surface(new_id surface)                -> ()
//   attach(object buffer, int x, int y)           -> (Ref, int32, int32)
static_assert(std::tuple_size_v<params_t<wl_compositor::requests::create_surface::sig>> == 0);
static_assert(std::is_same_v<params_t<wl_surface::requests::attach::sig>,
                             std::tuple<Ref<wl_buffer>, std::int32_t, std::int32_t>>);

// Enums are distinct types, so a raw int cannot stand in for one.
static_assert(std::is_same_v<params_t<wl_shm_pool::requests::create_buffer::sig>,
                             std::tuple<std::int32_t, std::int32_t, std::int32_t,
                                        std::int32_t, wl_shm::format>>);

// Events decode to exactly their declared argument types.
static_assert(std::is_same_v<decoded_t<wl_registry::events::global::sig>,
                             std::tuple<std::uint32_t, std::string_view, std::uint32_t>>);
static_assert(std::is_same_v<decoded_t<wl_pointer::events::motion::sig>,
                             std::tuple<std::uint32_t, fixed, fixed>>);

// fd arguments are counted separately: they never occupy a wire word.
static_assert(fd_count_v<wl_shm::requests::create_pool::sig> == 1);
static_assert(fd_count_v<wl_surface::requests::commit::sig> == 0);

// Every generated interface satisfies the algebra's laws (also asserted inside
// the generated header itself, once per interface).
static_assert(laws::well_formed<wl_display>);
static_assert(laws::well_formed<wl_seat>);
static_assert(laws::well_formed<wl_pointer>);

// ===========================================================================
// 2. Runtime: the wire codec round-trips.
// ===========================================================================

static void test_fixed() {
    std::printf("fixed point\n");
    CHECK(fixed::from_int(1).raw == 256);
    CHECK(fixed::from_int(-1).raw == -256);
    CHECK(fixed::from_double(1.5).raw == 384);
    CHECK(fixed::from_double(-0.5).to_double() == -0.5);
    CHECK(fixed::from_raw(384).to_int() == 1);
    CHECK((2.25_fx).to_double() == 2.25);
}

static void test_header() {
    std::printf("message header\n");
    std::vector<wire::word> buf;
    std::vector<int> fds;
    wire::Encoder e{buf, fds, 0};
    e.begin(1, 7);
    e.put_u32(0xdeadbeef);
    e.end();

    CHECK(buf.size() == 3);
    CHECK(buf[0] == 1);                    // object id
    CHECK((buf[1] & 0xffff) == 7);         // opcode
    CHECK((buf[1] >> 16) == 12);           // size in bytes, header included
    CHECK(buf[2] == 0xdeadbeef);
}

static void test_string_padding() {
    std::printf("string padding\n");
    struct Case { const char* s; std::size_t words; };
    // len word + ceil((strlen+1)/4) words
    const Case cases[] = {
        {"", 1 + 1},          // just the NUL, padded to 4
        {"abc", 1 + 1},       // 4 bytes exactly
        {"abcd", 1 + 2},      // 5 bytes -> 8
        {"wl_compositor", 1 + 4},  // 14 -> 16
    };
    for (const auto& c : cases) {
        std::vector<wire::word> buf;
        std::vector<int> fds;
        wire::Encoder e{buf, fds, 0};
        e.put_string(c.s);
        CHECK(buf.size() == c.words);

        wire::Decoder d{buf, {}, 0, 0, false};
        CHECK(d.get_string() == c.s);
        CHECK(!d.bad);
        CHECK(d.pos == buf.size());  // consumed exactly, padding included
    }
}

// The generic round-trip: encode a signature's values, decode them back, and
// require equality. Works for any Sig because both sides are folds.
template <class Sig_, class Tup>
static bool roundtrip(const Tup& in) {
    std::vector<wire::word> buf;
    std::vector<int> fds;
    wire::Encoder e{buf, fds, 0};
    detail::encode_fold<0>(e, Sig_{}, in, 0);

    wire::Decoder d{buf, fds, 0, 0, false};
    auto out = detail::decode_fold(d, Sig_{});
    return !d.bad && d.pos == buf.size() && out == in;
}

static void test_roundtrip() {
    std::printf("signature round-trip\n");

    // wl_pointer.motion: uint, fixed, fixed
    CHECK((roundtrip<wl_pointer::events::motion::sig>(
        std::tuple<std::uint32_t, fixed, fixed>{42, 1.5_fx, (-3.25_fx)})));

    // wl_registry.global: uint, string, uint
    CHECK((roundtrip<wl_registry::events::global::sig>(
        std::tuple<std::uint32_t, std::string_view, std::uint32_t>{3, "wl_compositor", 6})));

    // wl_surface.damage: 4 ints, negatives included
    CHECK((roundtrip<wl_surface::requests::damage::sig>(
        std::tuple<std::int32_t, std::int32_t, std::int32_t, std::int32_t>{-1, 0, 1920, 1080})));

    // wl_keyboard.key: uint, uint, uint, enum
    CHECK((roundtrip<wl_keyboard::events::key::sig>(
        std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, wl_keyboard::key_state>{
            1, 2, 30, wl_keyboard::key_state::pressed})));

    // Empty signature.
    CHECK((roundtrip<wl_surface::requests::commit::sig>(std::tuple<>{})));
}

static void test_decoder_is_safe() {
    std::printf("decoder rejects malformed input\n");

    // A string claiming 1 GiB in a 2-word buffer must not read out of bounds.
    std::vector<wire::word> buf{0x40000000u, 0u};
    wire::Decoder d{buf, {}, 0, 0, false};
    (void)d.get_string();
    CHECK(d.bad);

    // Truncated body: ask for more words than exist.
    std::vector<wire::word> tiny{1u};
    wire::Decoder d2{tiny, {}, 0, 0, false};
    (void)d2.get_u32();
    CHECK(!d2.bad);
    (void)d2.get_u32();
    CHECK(d2.bad);

    // Requesting an fd that was never sent is an error, not a bad descriptor.
    wire::Decoder d3{tiny, {}, 0, 0, false};
    CHECK(d3.get_fd() == -1);
    CHECK(d3.bad);
}

static void test_id_allocator() {
    std::printf("id allocator\n");
    IdAllocator a;
    const auto x = a.allocate();
    const auto y = a.allocate();
    CHECK(x == 2);  // 1 is reserved for wl_display
    CHECK(y == 3);
    a.release(x);
    CHECK(a.allocate() == x);  // recycled
    CHECK(a.allocate() == 4);
}

static void test_enum_bitfield() {
    std::printf("bitfield enums\n");
    using cap = wl_seat::capability;
    const auto both = cap::pointer | cap::keyboard;
    CHECK(any(both & cap::pointer));
    CHECK(any(both & cap::keyboard));
    CHECK(!any(both & cap::touch));
}

int main() {
    std::printf("i4::wl tests\n\n");
    test_fixed();
    test_header();
    test_string_padding();
    test_roundtrip();
    test_decoder_is_safe();
    test_id_allocator();
    test_enum_bitfield();

    std::printf("\n%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
