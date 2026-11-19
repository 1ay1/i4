// loom — the Wayland protocol as a compile-time algebra.
//
// This header knows *nothing* about Wayland. It defines the small algebra that
// generated protocol descriptions are written in, plus the type-level functions
// that interpret them. Everything downstream (wire codec, proxies, dispatch) is
// a fold over these types.
//
//   atoms      Int, Uint, Fixed, String, Array, Fd, Object<I>, NewId<I>, ...
//   product    Sig<A...>            a message payload  (Π of atoms)
//   coproduct  List<M...>           a message set      (Σ tagged by opcode)
//   record     an interface: { name, version, requests, events }
//
// C++20. No exceptions in this header, no allocations, no RTTI.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "fd.hpp"

namespace i4::wl {

// ---------------------------------------------------------------------------
// fixed: wayland's 24.8 signed fixed point number.
// ---------------------------------------------------------------------------
struct fixed {
    std::int32_t raw = 0;

    constexpr fixed() = default;
    constexpr explicit fixed(std::int32_t raw_, int /*tag*/) : raw(raw_) {}

    static constexpr fixed from_raw(std::int32_t r) { return fixed(r, 0); }
    static constexpr fixed from_int(std::int32_t v) { return fixed(v * 256, 0); }
    static constexpr fixed from_double(double v) {
        return fixed(static_cast<std::int32_t>(v * 256.0 + (v < 0 ? -0.5 : 0.5)), 0);
    }

    constexpr std::int32_t to_int() const { return raw >> 8; }
    constexpr double to_double() const { return static_cast<double>(raw) / 256.0; }

    constexpr fixed operator-() const { return from_raw(-raw); }
    constexpr fixed operator+(fixed o) const { return from_raw(raw + o.raw); }
    constexpr fixed operator-(fixed o) const { return from_raw(raw - o.raw); }
    constexpr fixed& operator+=(fixed o) { raw += o.raw; return *this; }
    constexpr fixed& operator-=(fixed o) { raw -= o.raw; return *this; }

    friend constexpr bool operator==(fixed, fixed) = default;
    friend constexpr auto operator<=>(fixed a, fixed b) { return a.raw <=> b.raw; }
};

constexpr fixed operator""_fx(long double v) { return fixed::from_double(static_cast<double>(v)); }
constexpr fixed operator""_fx(unsigned long long v) { return fixed::from_int(static_cast<std::int32_t>(v)); }

// ---------------------------------------------------------------------------
// Type-level list. The coproduct carrier.
// ---------------------------------------------------------------------------
template <class... T>
struct List {
    static constexpr std::size_t size = sizeof...(T);
};

namespace detail {
template <std::size_t I, class L>
struct at;
template <std::size_t I, class H, class... T>
struct at<I, List<H, T...>> : at<I - 1, List<T...>> {};
template <class H, class... T>
struct at<0, List<H, T...>> {
    using type = H;
};
}  // namespace detail

template <std::size_t I, class L>
using at_t = typename detail::at<I, L>::type;

/// Is type T a member of list L?
template <class L, class T>
struct contains : std::false_type {};
template <class... E, class T>
struct contains<List<E...>, T> : std::bool_constant<(std::is_same_v<E, T> || ...)> {};
template <class L, class T>
inline constexpr bool contains_v = contains<L, T>::value;

// ---------------------------------------------------------------------------
// Atoms. Each atom is a *type* that names a wire representation and the C++
// value that inhabits it. `wire_words` is the fixed size in 32-bit words, or
// dynamic_extent for length-prefixed payloads.
// ---------------------------------------------------------------------------
inline constexpr std::size_t dyn = static_cast<std::size_t>(-1);

struct Int {
    using value = std::int32_t;
    static constexpr std::size_t words = 1;
};
struct Uint {
    using value = std::uint32_t;
    static constexpr std::size_t words = 1;
};
struct Fixed {
    using value = fixed;
    static constexpr std::size_t words = 1;
};
struct String {
    using value = std::string_view;
    static constexpr std::size_t words = dyn;
};
struct Array {
    using value = std::span<const std::byte>;
    static constexpr std::size_t words = dyn;
};
struct Fd {
    using value = BorrowedFd;  // sending lends; the kernel dups into the socket
    static constexpr std::size_t words = 0;  // travels out-of-band, SCM_RIGHTS
};

// An enumeration argument. `E` is a scoped enum, `Base` is Int or Uint.
// This is what stops you passing a raw integer where a protocol enum is meant.
template <class E, class Base = Uint>
struct Enum {
    using value = E;
    using base = Base;
    static constexpr std::size_t words = 1;
};

// A reference to an existing object of interface `I`.
template <class I>
struct Object {
    using iface = I;
    static constexpr std::size_t words = 1;
};

// The id of an object this message *creates*. Produces a value rather than
// consuming one, so it does not appear in a request's parameter list.
template <class I>
struct NewId {
    using iface = I;
    static constexpr std::size_t words = 1;
};

// new_id with no static interface: wire is (string iface, uint version, uint id).
// Exactly one message in core Wayland uses it (wl_registry.bind).
struct DynNewId {
    static constexpr std::size_t words = dyn;
};

// Nullable wrapper: `allow-null="true"` in the XML.
template <class A>
struct Nullable {
    using inner = A;
    static constexpr std::size_t words = A::words;
};

// ---------------------------------------------------------------------------
// Classifiers
// ---------------------------------------------------------------------------
template <class A> struct is_new_id : std::false_type {};
template <class I> struct is_new_id<NewId<I>> : std::true_type {};
template <>        struct is_new_id<DynNewId> : std::true_type {};
template <class A> inline constexpr bool is_new_id_v = is_new_id<A>::value;

template <class A> struct is_enum_atom : std::false_type {};
template <class E, class B> struct is_enum_atom<Enum<E, B>> : std::true_type {};
template <class A> inline constexpr bool is_enum_atom_v = is_enum_atom<A>::value;

template <class A> struct is_dyn_new_id : std::false_type {};
template <>        struct is_dyn_new_id<DynNewId> : std::true_type {};
template <class A> inline constexpr bool is_dyn_new_id_v = is_dyn_new_id<A>::value;

template <class A> struct is_fd : std::false_type {};
template <>        struct is_fd<Fd> : std::true_type {};
template <class A> struct is_nullable : std::false_type {};
template <class A> struct is_nullable<Nullable<A>> : std::true_type {};
template <class A> inline constexpr bool is_nullable_v = is_nullable<A>::value;

// ---------------------------------------------------------------------------
// Signature: the product of a message's arguments.
// ---------------------------------------------------------------------------
template <class... A>
struct Sig {
    static constexpr std::size_t arity = sizeof...(A);
    using args = List<A...>;
};

// Number of file descriptors a signature carries out-of-band.
template <class S> struct fd_count;
template <class... A>
struct fd_count<Sig<A...>> {
    static constexpr std::size_t value = (0u + ... + (is_fd<A>::value ? 1u : 0u));
};
template <class S> inline constexpr std::size_t fd_count_v = fd_count<S>::value;

// A signature is `static` when every argument has a known wire width, which
// lets the encoder compute the message length at compile time.
template <class S> struct static_words;
template <class... A>
struct static_words<Sig<A...>> {
    static constexpr bool ok = ((A::words != dyn) && ...);
    static constexpr std::size_t value = ok ? (0u + ... + A::words) : dyn;
};

// ---------------------------------------------------------------------------
// The result type function. A request's "return type" is read off its
// signature: the interface of its new_id argument, or void when it has none.
// This is the whole reason `create_surface()` can be strongly typed without a
// single hand-written line per request.
// ---------------------------------------------------------------------------
namespace detail {
template <class... A>
struct find_new_id {
    using type = void;
};
template <class A, class... R>
struct find_new_id<A, R...> {
    using type = std::conditional_t<is_new_id_v<A>, A, typename find_new_id<R...>::type>;
};
template <>
struct find_new_id<> {
    using type = void;
};
}  // namespace detail

template <class S> struct new_id_arg;
template <class... A>
struct new_id_arg<Sig<A...>> {
    using type = typename detail::find_new_id<A...>::type;
};
template <class S>
using new_id_arg_t = typename new_id_arg<S>::type;

template <class S>
inline constexpr bool creates_object_v = !std::is_void_v<new_id_arg_t<S>>;

// ---------------------------------------------------------------------------
// value_t: atom -> the C++ type a caller supplies / a handler receives.
// `Proxy` is injected by the runtime layer via the `Ref` template below so this
// header stays free of runtime dependencies.
// ---------------------------------------------------------------------------
template <class I>
struct Ref;  // defined in proxy.hpp: a typed object id, possibly null
template <class I>
class NonNull;  // defined in proxy.hpp: a typed object id, never null

// wl_registry.bind's caller-supplied argument: which global, at what version.
struct BindSpec {
    std::string_view interface;
    std::uint32_t version;
};

namespace detail {
template <class A>
struct value_of {
    using type = typename A::value;
};
template <class I>
struct value_of<Object<I>> {
    // Not nullable in the XML, so not nullable in the type.
    using type = NonNull<I>;
};
template <class I>
struct value_of<NewId<I>> {
    using type = Ref<I>;  // in an *event*, the server tells us the new id
};
template <>
struct value_of<DynNewId> {
    using type = std::uint32_t;
};
template <class E, class B>
struct value_of<Enum<E, B>> {
    using type = E;
};
template <class A>
struct value_of<Nullable<A>> {
    using type = std::optional<typename value_of<A>::type>;
};
template <class I>
struct value_of<Nullable<Object<I>>> {
    using type = Ref<I>;  // a null Ref already means "no object"
};
}  // namespace detail

template <class A>
using value_t = typename detail::value_of<A>::type;

// The receive side is DIFFERENT, and pretending otherwise would be a lie:
// an object id arriving from the compositor is just a number. We cannot prove
// it is live (the object may have been destroyed while the event was in
// flight), so an event's object argument decodes to a nullable `Ref<I>` and
// the handler decides. Outgoing arguments get `NonNull` because there the
// caller is the one making the claim.
namespace detail {
template <class A>
struct decode_value_of {
    using type = typename value_of<A>::type;
};
template <class I>
struct decode_value_of<Object<I>> {
    using type = Ref<I>;
};
template <class I>
struct decode_value_of<Nullable<Object<I>>> {
    using type = Ref<I>;
};
}  // namespace detail

template <class A>
using decode_value_t = typename detail::decode_value_of<A>::type;

// The tuple an event decodes into: every argument, in order.
namespace detail {
template <class S> struct decoded;
template <class... A>
struct decoded<Sig<A...>> {
    using type = std::tuple<decode_value_t<A>...>;
};
}  // namespace detail
template <class S>
using decoded_t = typename detail::decoded<S>::type;

// The parameter pack a *caller* supplies for a request: new_id arguments are
// dropped (the library allocates the id), dyn new_id becomes a version number.
namespace detail {
template <class... Acc>
struct params_acc {
    using type = std::tuple<Acc...>;
};

template <class Out, class In>
struct params_impl;
template <class... Acc>
struct params_impl<std::tuple<Acc...>, List<>> {
    using type = std::tuple<Acc...>;
};
template <class... Acc, class H, class... T>
struct params_impl<std::tuple<Acc...>, List<H, T...>> {
    using next = std::conditional_t<
        is_dyn_new_id_v<H>, std::tuple<Acc..., BindSpec>,
        std::conditional_t<is_new_id_v<H>, std::tuple<Acc...>, std::tuple<Acc..., value_t<H>>>>;
    using type = typename params_impl<next, List<T...>>::type;
};
}  // namespace detail

template <class S> struct params;
template <class... A>
struct params<Sig<A...>> {
    using type = typename detail::params_impl<std::tuple<>, List<A...>>::type;
};
template <class S>
using params_t = typename params<S>::type;

// ---------------------------------------------------------------------------
// Interface records. A generated interface is a struct with:
//   static constexpr std::string_view wl_name;
//   static constexpr std::uint32_t    wl_version;
//   struct requests { <message types>; using all = List<...>; };
//   struct events   { ... };
// A message type is:
//   static constexpr std::uint32_t opcode, since;
//   static constexpr std::string_view wl_name;
//   using iface = <owner>; using sig = Sig<...>;
// ---------------------------------------------------------------------------
template <class T>
concept Interface = requires {
    { T::wl_name } -> std::convertible_to<std::string_view>;
    { T::wl_version } -> std::convertible_to<std::uint32_t>;
    typename T::requests::all;
    typename T::events::all;
};

template <class M>
concept Message = requires {
    { M::opcode } -> std::convertible_to<std::uint32_t>;
    { M::since } -> std::convertible_to<std::uint32_t>;
    typename M::sig;
    typename M::iface;
};

// Version inheritance: a child object created through a parent bound at
// version V is itself at version min(V, child's own maximum). Wayland says so;
// here it is a type-level function, so it is checked by the compiler.
template <class Child, std::uint32_t V>
inline constexpr std::uint32_t inherited_version = (V < Child::wl_version ? V : Child::wl_version);

// ---------------------------------------------------------------------------
// Algebraic laws, checked on every generated protocol by static_assert.
// ---------------------------------------------------------------------------
namespace laws {

// Opcodes of a message set are dense and ordered: 0, 1, 2, ... n-1.
// That is what makes a coproduct dispatchable by an array index.
template <class L>
struct dense_opcodes;
template <class... M>
struct dense_opcodes<List<M...>> {
    static constexpr bool check() {
        std::uint32_t expect = 0;
        bool ok = true;
        (((ok = ok && (M::opcode == expect++)), ...));
        return ok;
    }
    static constexpr bool value = check();
};
template <class... M>
struct dense_opcodes<List<M...>*> {};
template <>
struct dense_opcodes<List<>> {
    static constexpr bool value = true;
};

// A message may not be newer than the interface that declares it.
template <class L>
struct since_within_version;
template <class... M>
struct since_within_version<List<M...>> {
    static constexpr bool value = ((M::since <= M::iface::wl_version) && ...);
};
template <>
struct since_within_version<List<>> {
    static constexpr bool value = true;
};

// At most one new_id per message: an object is created once or not at all.
template <class S> struct single_new_id;
template <class... A>
struct single_new_id<Sig<A...>> {
    static constexpr bool value = (0u + ... + (is_new_id_v<A> ? 1u : 0u)) <= 1u;
};

template <class L> struct all_single_new_id;
template <class... M>
struct all_single_new_id<List<M...>> {
    static constexpr bool value = (single_new_id<typename M::sig>::value && ...);
};
template <>
struct all_single_new_id<List<>> {
    static constexpr bool value = true;
};

template <class I>
inline constexpr bool well_formed =
    dense_opcodes<typename I::requests::all>::value &&
    dense_opcodes<typename I::events::all>::value &&
    since_within_version<typename I::requests::all>::value &&
    since_within_version<typename I::events::all>::value &&
    all_single_new_id<typename I::requests::all>::value &&
    all_single_new_id<typename I::events::all>::value;

}  // namespace laws
}  // namespace i4::wl
