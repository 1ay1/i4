// Completeness: every protocol installed on this system, compiled together,
// with the algebra's laws checked on all of it.
//
// The generated `all.hpp` is written by the build from the actual contents of
// the wayland-protocols tree, so this test tracks the system rather than a
// list someone remembered to update.
#include <i4/wl/proto/all.hpp>
#include <i4/wl/refine.hpp>
#include <i4/wl/role.hpp>

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
// Role duality. The server side is not a second implementation: it is the same
// interface record read with the projections swapped.
// ===========================================================================

// What a client sends, a server receives, and vice versa.
static_assert(std::is_same_v<outbound_t<ClientRole, wl_surface>, wl_surface::requests>);
static_assert(std::is_same_v<inbound_t<ClientRole, wl_surface>, wl_surface::events>);
static_assert(std::is_same_v<outbound_t<ServerRole, wl_surface>, wl_surface::events>);
static_assert(std::is_same_v<inbound_t<ServerRole, wl_surface>, wl_surface::requests>);

// The duality holds definitionally, for every interface: a role's outbound is
// its dual's inbound.
template <Interface I>
constexpr bool duality_holds() {
    return std::is_same_v<outbound_t<ClientRole, I>, inbound_t<ServerRole, I>> &&
           std::is_same_v<outbound_t<ServerRole, I>, inbound_t<ClientRole, I>> &&
           std::is_same_v<outbound_t<dual_t<ClientRole>, I>, outbound_t<ServerRole, I>>;
}

static_assert(duality_holds<wl_surface>());
static_assert(duality_holds<wl_seat>());
static_assert(duality_holds<xdg_toplevel>());
static_assert(duality_holds<wl_display>());

// Direction is a type-level fact: a client cannot send an event.
static_assert(sends<ClientRole, wl_surface, wl_surface::requests::commit>);
static_assert(!sends<ClientRole, wl_surface, wl_surface::events::enter>);
static_assert(receives<ClientRole, wl_surface, wl_surface::events::enter>);

static_assert(sends<ServerRole, wl_surface, wl_surface::events::enter>);
static_assert(!sends<ServerRole, wl_surface, wl_surface::requests::commit>);

// The two roles partition the id space, so ids never collide.
static_assert(ClientRole::id_max < ServerRole::id_base);

// ===========================================================================
// Enum validation, derived from the XML's entry list.
// ===========================================================================

// Every listed enumerator is valid...
static_assert(valid(wl_shm::format::argb8888));
static_assert(valid(wl_shm::format::xrgb8888));
static_assert(valid(wl_output::transform::normal));
static_assert(valid(wl_pointer::button_state::pressed));

// ...and a value the protocol never defines is not.
static_assert(!valid(static_cast<wl_shm::format>(0xdeadbeef)));
static_assert(!valid(static_cast<wl_output::transform>(99)));
static_assert(!valid(static_cast<wl_pointer::button_state>(7)));

// Bitfields validate against the OR of their flags, so combinations are legal
// but stray bits are not.
static_assert(valid(wl_seat::capability::pointer | wl_seat::capability::keyboard));
static_assert(valid(static_cast<wl_seat::capability>(0)));  // no flags set
static_assert(!valid(static_cast<wl_seat::capability>(0x80000000u)));

// ===========================================================================
// Refinement types: total for constants, checked-once for runtime values.
// ===========================================================================
static_assert(Positive<std::int32_t>::of<640>().get() == 640);
static_assert(NonNegative<std::int32_t>::of<0>().get() == 0);
static_assert((100_pos).get() == 100);

// `Positive<int32>::of<-5>()` is a compile error; see tests/fail/negative_size.

static void test_refinements() {
    std::printf("refinement types\n");
    CHECK(!Positive<std::int32_t>::from(0).has_value());
    CHECK(!Positive<std::int32_t>::from(-1).has_value());
    CHECK(Positive<std::int32_t>::from(1).has_value());
    CHECK(Positive<std::int32_t>::from(1920)->get() == 1920);

    CHECK(NonNegative<std::int32_t>::from(0).has_value());
    CHECK(!NonNegative<std::int32_t>::from(-1).has_value());

    // A Serial can only come from an event, never from a literal.
    const auto s = Serial::from_event(4242);
    CHECK(s.get() == 4242);
}

// ===========================================================================
// Coverage. Counted from the generated types, not from a comment.
// ===========================================================================
template <class L>
struct count_messages;
template <class... M>
struct count_messages<List<M...>> {
    static constexpr std::size_t value = sizeof...(M);
};

template <class I>
constexpr std::size_t message_count() {
    return count_messages<typename I::requests::all>::value +
           count_messages<typename I::events::all>::value;
}

template <class... I>
constexpr std::size_t total_messages(List<I...>) {
    return (0u + ... + message_count<I>());
}

template <class... I>
constexpr std::size_t total_interfaces(List<I...>) {
    return sizeof...(I);
}

// Core Wayland, counted by the compiler from the generated records.
static constexpr std::size_t kCoreInterfaces = total_interfaces(wayland_interfaces{});
static constexpr std::size_t kCoreMessages = total_messages(wayland_interfaces{});
static_assert(kCoreInterfaces == 23);
static_assert(kCoreMessages == 134);

static void test_coverage() {
    std::printf("coverage\n");
    std::printf("  core wayland: %zu interfaces, %zu messages\n", kCoreInterfaces,
                kCoreMessages);
    CHECK(kCoreInterfaces == 23);

    // Every core interface satisfies the algebra's laws. (The generated header
    // asserts this per interface too; this is a second, aggregate check.)
    CHECK(laws::well_formed<wl_display>);
    CHECK(laws::well_formed<wl_pointer>);
    CHECK(laws::well_formed<xdg_toplevel>);
}

int main() {
    std::printf("i4::wl completeness tests\n\n");
    test_refinements();
    test_coverage();
    std::printf("\n%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
