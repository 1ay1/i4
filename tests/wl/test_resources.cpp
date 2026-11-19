// Runtime proof that the resource types do what their names claim.
// The compile-time half lives in tests/fail/ and is checked by ctest.
#include <fcntl.h>
#include <i4/wl/affine.hpp>
#include <i4/wl/proto/wayland.hpp>
#include <i4/wl/typestate.hpp>

#include <cstdio>

using namespace i4::wl;
using namespace i4::wl::proto;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

// ===========================================================================
// Compile-time: the destructor of an interface is read off the generated data.
// ===========================================================================
static_assert(has_destructor<wl_surface>);
static_assert(std::is_same_v<destructor_of<wl_surface>, wl_surface::requests::destroy>);

// wl_display has no destructor request: you close the connection instead.
static_assert(!has_destructor<wl_display>);
static_assert(std::is_void_v<destructor_of<wl_display>>);

// wl_callback fires once and the server destroys it; no destructor request.
static_assert(!has_destructor<wl_callback>);

// The nullability distinction comes straight from the XML, and shows up as a
// difference in the lowered parameter type:
//   attach(buffer)             allow-null="true" -> Ref<wl_buffer>, 0 is legal
//   get_xdg_surface(surface)   not nullable      -> NonNull<wl_surface>
// A Ref already represents null as id 0, so a nullable object argument needs
// no optional wrapper; the type difference IS the nullability.
static_assert(std::is_same_v<params_t<wl_surface::requests::attach::sig>,
                             std::tuple<Ref<wl_buffer>, std::int32_t, std::int32_t>>);

// The non-nullable case: damage_buffer has no object argument, so use a
// request that does. wl_surface.frame creates an object, so its parameter list
// is empty; wl_subcompositor.get_subsurface takes two non-null surfaces.
static_assert(
    std::is_same_v<params_t<wl_subcompositor::requests::get_subsurface::sig>,
                   std::tuple<NonNull<wl_surface>, NonNull<wl_surface>>>);

// wl_shm.create_pool takes an fd: the parameter is a BorrowedFd, never an int.
static_assert(std::is_same_v<params_t<wl_shm::requests::create_pool::sig>,
                             std::tuple<BorrowedFd, std::int32_t>>);

// Resource types have the right capabilities, and only those.
static_assert(!std::is_copy_constructible_v<OwnedFd>);
static_assert(std::is_move_constructible_v<OwnedFd>);
static_assert(std::is_copy_constructible_v<BorrowedFd>);  // a view may be copied
static_assert(!std::is_copy_constructible_v<Unique<wl_surface>>);
static_assert(std::is_move_constructible_v<Unique<wl_surface>>);

// NonNull cannot be default-constructed: that is what makes it non-null.
static_assert(!std::is_default_constructible_v<NonNull<wl_surface>>);
static_assert(std::is_default_constructible_v<Ref<wl_surface>>);

// ===========================================================================
// Runtime
// ===========================================================================

static void test_owned_fd() {
    std::printf("OwnedFd closes exactly once\n");
    int raw = ::dup(0);
    CHECK(raw >= 0);
    {
        OwnedFd fd{raw};
        CHECK(fd.valid());
        CHECK(fd.borrow().get() == raw);
    }
    // After the scope, the descriptor is closed: fcntl must now fail.
    CHECK(::fcntl(raw, F_GETFD) == -1);

    // Move transfers ownership; the source goes empty.
    int raw2 = ::dup(0);
    OwnedFd a{raw2};
    OwnedFd b = std::move(a);
    CHECK(!a.valid());
    CHECK(b.get() == raw2);
    CHECK(::fcntl(raw2, F_GETFD) != -1);  // still open: b owns it

    // release() hands it off without closing.
    const int taken = b.release();
    CHECK(!b.valid());
    CHECK(taken == raw2);
    CHECK(::fcntl(taken, F_GETFD) != -1);
    ::close(taken);
}

static void test_nonnull() {
    std::printf("NonNull refuses null\n");
    CHECK(!NonNull<wl_surface>::from(Ref<wl_surface>{}).has_value());
    auto ok = NonNull<wl_surface>::from(Ref<wl_surface>{7});
    CHECK(ok.has_value());
    CHECK(ok->id() == 7);
}

static void test_invalid_enum_rejected() {
    std::printf("invalid enum from the wire never reaches a handler\n");

    // wl_keyboard.key(serial, time, key, state): state is an enum whose only
    // legal values are 0 (released) and 1 (pressed). Forge one that is not.
    using key_ev = wl_keyboard::events::key;
    const std::uint32_t body[] = {1u, 2u, 30u, 0xdeadbeefu};

    bool handler_ran = false;
    Listener<wl_keyboard> l;
    l.on<key_ev>([&](std::uint32_t, std::uint32_t, std::uint32_t,
                     wl_keyboard::key_state) { handler_ran = true; });

    const RawEvent bad{7, key_ev::opcode, std::span<const wire::word>(body, 4), {}};
    l.dispatch(bad);
    CHECK(!handler_ran);  // rejected: the validator comes from the XML

    // The same event with a legal value does get through.
    const std::uint32_t good_body[] = {1u, 2u, 30u, 1u};
    wl_keyboard::key_state seen{};
    Listener<wl_keyboard> l2;
    l2.on<key_ev>([&](std::uint32_t, std::uint32_t, std::uint32_t,
                      wl_keyboard::key_state st) {
        handler_ran = true;
        seen = st;
    });
    const RawEvent good{7, key_ev::opcode, std::span<const wire::word>(good_body, 4), {}};
    l2.dispatch(good);
    CHECK(handler_ran);
    CHECK(seen == wl_keyboard::key_state::pressed);
}

// A fake connection: we only need the wire bytes, not a compositor.
static void test_unique_sends_destructor() {
    std::printf("Unique sends the protocol destructor\n");

    // Socketpair stands in for the compositor so flush() has somewhere to go.
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    auto conn = Connection::adopt(sv[0]);

    const wire::id_t id = conn->allocate_id();
    {
        Unique<wl_surface> s{Proxy<wl_surface>{conn.get(), id}};
        CHECK(s.live());
        CHECK(s.id() == id);
    }  // scope exit must emit wl_surface.destroy
    CHECK(conn->flush());

    // Read it back off the socket and check the header by hand.
    std::uint32_t buf[8]{};
    const ssize_t n = ::recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT);
    CHECK(n == 8);  // one header, no arguments
    CHECK(buf[0] == id);
    CHECK((buf[1] & 0xffff) == wl_surface::requests::destroy::opcode);
    CHECK((buf[1] >> 16) == 8);

    ::close(sv[1]);
}

static void test_explicit_destroy_consumes() {
    std::printf("std::move(obj).destroy() empties the handle\n");
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    auto conn = Connection::adopt(sv[0]);

    Unique<wl_surface> s{Proxy<wl_surface>{conn.get(), conn->allocate_id()}};
    CHECK(s.live());
    std::move(s).destroy();
    CHECK(!s.live());  // consumed: a second destroy cannot reach the wire

    CHECK(conn->flush());
    std::uint32_t buf[8]{};
    CHECK(::recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT) == 8);  // exactly one
    CHECK(::recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT) == -1);  // and no more
    ::close(sv[1]);
}

int main() {
    std::printf("i4::wl resource tests\n\n");
    test_owned_fd();
    test_nonnull();
    test_invalid_enum_rejected();
    test_unique_sends_destructor();
    test_explicit_destroy_consumes();
    std::printf("\n%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
