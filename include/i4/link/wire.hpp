#pragma once
// i4/link/wire.hpp — the wire format, derived from the types.
//
//   std::vector<std::byte> bytes = encode(state);
//   std::expected<wm::State, WireError> back = decode<wm::State>(bytes);
//
// No IDL and no hand-written serialisers. A struct crosses the wire because
// it is made of things that can: C++26 lets a template take any struct apart
// (`auto& [...members] = value;`), so the codec, the fingerprint and the
// checks below are all derived from the declaration itself. Add a field to
// wm::Window and it is on the wire, in both directions, and the schema
// fingerprint changes so an old head is refused instead of misreading it.
//
// The universe of wire types, and why each rule:
//
//   bool, integers           LEB128 varints (signed ones zigzagged)
//   scoped enums             their index, checked against enum_count
//   std::string              length + bytes
//   vector, optional, map    count/flag + elements; map keys strictly rising
//   variant                  alternative index + that alternative
//   structs                  their members, in order, structurally; a struct
//                            with `bool valid() const` is a REFINEMENT type
//                            and a decoded value it rejects is refused
//                            (Flags<E> with a bit past its last enumerator)
//
//   floating point           REFUSED, at compile time. The window manager is
//                            replicated: a head runs apply() itself to
//                            predict, and floats may differ across CPUs and
//                            compilers. Integers do not.
//   pointers, references     REFUSED: an address means nothing on another
//                            machine, and nothing at all to an attacker.
//
// Decoding is TOTAL: every byte string either decodes to a valid value or is
// refused with a reason. Never a crash, never a value the types forbid (an
// enum past its last enumerator, a variant index past its last alternative).
// Encoding is CANONICAL: encode(decode(b)) == b for every b that decodes, so
// two parties that agree on a value agree on its bytes.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "i4/base/enum.hpp"

namespace i4::link {

enum class WireError : std::uint8_t {
    truncated,      // the bytes ended mid-value
    overlong,       // a varint longer than its type, or not minimal
    out_of_range,   // a varint too big for its integer
    bad_bool,       // neither 0 nor 1
    bad_enum,       // past the last enumerator
    bad_variant,    // past the last alternative
    bad_optional,   // presence flag neither 0 nor 1
    not_canonical,  // map keys not strictly rising
    too_large,      // a count larger than the bytes that remain could hold
    trailing,       // bytes left over after the value
    invalid,        // well-formed, but the type's own valid() refuses it
};

[[nodiscard]] constexpr std::string_view describe(WireError e) noexcept {
    switch (e) {
        case WireError::truncated: return "truncated";
        case WireError::overlong: return "overlong varint";
        case WireError::out_of_range: return "integer out of range";
        case WireError::bad_bool: return "bad bool";
        case WireError::bad_enum: return "unknown enumerator";
        case WireError::bad_variant: return "unknown alternative";
        case WireError::bad_optional: return "bad optional flag";
        case WireError::not_canonical: return "map keys out of order";
        case WireError::too_large: return "count too large";
        case WireError::trailing: return "trailing bytes";
        case WireError::invalid: return "invalid value";
    }
    return "?";
}

// -- taking a struct apart -------------------------------------------------------

namespace detail {

template <class... Ts>
struct types {};

template <class T>
auto members_of(T& t) {
    [[maybe_unused]] auto& [... xs] = t;
    return types<std::remove_cvref_t<decltype(xs)>...>{};
}

template <class T>
using members = decltype(members_of(std::declval<T&>()));

template <class T>
struct is_vector : std::false_type {};
template <class T>
struct is_vector<std::vector<T>> : std::true_type {};
template <class T>
struct is_optional : std::false_type {};
template <class T>
struct is_optional<std::optional<T>> : std::true_type {};
template <class T>
struct is_variant : std::false_type {};
template <class... Ts>
struct is_variant<std::variant<Ts...>> : std::true_type {};
template <class T>
struct is_map : std::false_type {};
template <class K, class V>
struct is_map<std::map<K, V>> : std::true_type {};

}  // namespace detail

// -- the universe, as a check ----------------------------------------------------------
//
// wire_ok<T>() answers "can T cross the wire", recursively, at compile time;
// encode and decode static_assert it so the error names the rule, not a wall
// of template instantiations.

template <class T>
consteval bool wire_ok();

namespace detail {
template <class... Ts>
consteval bool all_ok(types<Ts...>) {
    return (wire_ok<Ts>() && ...);
}
}  // namespace detail

template <class T>
consteval bool wire_ok() {
    using namespace detail;
    if constexpr (std::is_same_v<T, bool>)
        return true;
    else if constexpr (std::is_enum_v<T>)
        return wire_enum<T>;
    else if constexpr (std::is_integral_v<T>)
        return sizeof(T) <= 8;
    else if constexpr (std::is_floating_point_v<T> || std::is_pointer_v<T> || std::is_reference_v<T> ||
                       std::is_member_pointer_v<T>)
        return false;
    else if constexpr (std::is_same_v<T, std::string>)
        return true;
    else if constexpr (is_vector<T>::value || is_optional<T>::value)
        return wire_ok<typename T::value_type>();
    else if constexpr (is_map<T>::value)
        return wire_ok<typename T::key_type>() && wire_ok<typename T::mapped_type>();
    else if constexpr (is_variant<T>::value)
        return []<class... As>(std::variant<As...>*) { return (wire_ok<As>() && ...); }(static_cast<T*>(nullptr));
    else if constexpr (std::is_class_v<T> && std::is_default_constructible_v<T>)
        return all_ok(members<T>{});
    else
        return false;
}

template <class T>
concept Wire = wire_ok<T>();

// -- the fingerprint --------------------------------------------------------------------
//
// A hash of a type's SHAPE: what it is made of, in what order. Names are not
// in it (renaming a field changes no bytes); everything that changes the
// bytes is. Two builds that print the same fingerprint read each other's
// messages; any other pair is refused at the handshake.

namespace detail {

consteval std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0x100000001b3ull;
}

template <class T>
consteval std::uint64_t shape();

template <class... Ts>
consteval std::uint64_t shapes(std::uint64_t h, types<Ts...>) {
    ((h = mix(h, shape<Ts>())), ...);
    return h;
}

template <class T>
consteval std::uint64_t shape() {
    if constexpr (std::is_same_v<T, bool>)
        return 'b';
    else if constexpr (std::is_enum_v<T>)
        return mix('e', enum_count<T>());
    else if constexpr (std::is_integral_v<T>)
        return mix(std::is_signed_v<T> ? 'i' : 'u', sizeof(T));
    else if constexpr (std::is_same_v<T, std::string>)
        return 's';
    else if constexpr (is_vector<T>::value)
        return mix('v', shape<typename T::value_type>());
    else if constexpr (is_optional<T>::value)
        return mix('o', shape<typename T::value_type>());
    else if constexpr (is_map<T>::value)
        return mix(mix('m', shape<typename T::key_type>()), shape<typename T::mapped_type>());
    else if constexpr (is_variant<T>::value)
        return []<class... As>(std::variant<As...>*) {
            return shapes(mix('V', sizeof...(As)), types<As...>{});
        }(static_cast<T*>(nullptr));
    else
        return []<class... Ms>(types<Ms...> t) { return shapes(mix('S', sizeof...(Ms)), t); }(members<T>{});
}

}  // namespace detail

template <Wire T>
inline constexpr std::uint64_t fingerprint = detail::shape<T>();

// -- writing ---------------------------------------------------------------------------------

class Writer {
public:
    void varint(std::uint64_t v) {
        while (v >= 0x80) {
            out_.push_back(static_cast<std::byte>((v & 0x7f) | 0x80));
            v >>= 7;
        }
        out_.push_back(static_cast<std::byte>(v));
    }
    void bytes(std::string_view s) {
        varint(s.size());
        for (char c : s) out_.push_back(static_cast<std::byte>(c));
    }
    [[nodiscard]] std::vector<std::byte> take() && { return std::move(out_); }
    [[nodiscard]] std::span<const std::byte> view() const { return out_; }

private:
    std::vector<std::byte> out_;
};

template <class T>
void put(Writer& w, const T& v) {
    using namespace detail;
    if constexpr (std::is_same_v<T, bool>) {
        w.varint(v ? 1 : 0);
    } else if constexpr (std::is_enum_v<T>) {
        w.varint(static_cast<std::uint64_t>(v));
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        const auto x = static_cast<std::int64_t>(v);
        w.varint((static_cast<std::uint64_t>(x) << 1) ^ static_cast<std::uint64_t>(x >> 63));
    } else if constexpr (std::is_integral_v<T>) {
        w.varint(v);
    } else if constexpr (std::is_same_v<T, std::string>) {
        w.bytes(v);
    } else if constexpr (is_vector<T>::value) {
        w.varint(v.size());
        for (const auto& x : v) put(w, x);
    } else if constexpr (is_optional<T>::value) {
        w.varint(v ? 1 : 0);
        if (v) put(w, *v);
    } else if constexpr (is_map<T>::value) {
        w.varint(v.size());
        for (const auto& [k, x] : v) {
            put(w, k);
            put(w, x);
        }
    } else if constexpr (is_variant<T>::value) {
        w.varint(v.index());
        std::visit([&](const auto& x) { put(w, x); }, v);
    } else {
        const auto& [... xs] = v;
        (put(w, xs), ...);
    }
}

template <Wire T>
[[nodiscard]] std::vector<std::byte> encode(const T& v) {
    Writer w;
    put(w, v);
    return std::move(w).take();
}

// -- reading ----------------------------------------------------------------------------------

class Reader {
public:
    explicit Reader(std::span<const std::byte> in) : in_(in) {}

    std::expected<std::uint64_t, WireError> varint() {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (at_ == in_.size()) return std::unexpected(WireError::truncated);
            const auto b = static_cast<std::uint8_t>(in_[at_++]);
            if (shift == 63 && b > 1) return std::unexpected(WireError::overlong);
            v |= std::uint64_t{b & 0x7fu} << shift;
            if ((b & 0x80) == 0) {
                // Minimal: a trailing zero group would be a second spelling
                // of the same number, and canonical means one spelling.
                if (b == 0 && shift != 0) return std::unexpected(WireError::overlong);
                return v;
            }
        }
        return std::unexpected(WireError::overlong);
    }

    /// A count of things each at least `min_size` bytes long: no larger than
    /// the bytes left could hold, so a forged count cannot allocate the moon.
    std::expected<std::size_t, WireError> count(std::size_t min_size) {
        auto n = varint();
        if (!n) return std::unexpected(n.error());
        const auto room = remaining() / (min_size == 0 ? 1 : min_size);
        if (*n > room || *n > (std::uint64_t{1} << 24)) return std::unexpected(WireError::too_large);
        return static_cast<std::size_t>(*n);
    }

    std::expected<std::string_view, WireError> bytes() {
        auto n = count(1);
        if (!n) return std::unexpected(n.error());
        const auto* p = reinterpret_cast<const char*>(in_.data() + at_);
        at_ += *n;
        return std::string_view{p, *n};
    }

    [[nodiscard]] std::size_t remaining() const { return in_.size() - at_; }

private:
    std::span<const std::byte> in_;
    std::size_t at_ = 0;
};

using Read = std::expected<void, WireError>;

template <class T>
Read get(Reader& r, T& v);

namespace detail {

template <class T>
Read get_variant(Reader& r, T& v, std::size_t index) {
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        Read out = std::unexpected(WireError::bad_variant);
        ((index == I ? (out = get(r, v.template emplace<I>()), 0) : 0), ...);
        return out;
    }(std::make_index_sequence<std::variant_size_v<T>>{});
}

}  // namespace detail

template <class T>
Read get(Reader& r, T& v) {
    using namespace detail;
    auto fail = [](WireError e) { return Read{std::unexpected(e)}; };
    if constexpr (std::is_same_v<T, bool>) {
        auto x = r.varint();
        if (!x) return fail(x.error());
        if (*x > 1) return fail(WireError::bad_bool);
        v = *x == 1;
    } else if constexpr (std::is_enum_v<T>) {
        auto x = r.varint();
        if (!x) return fail(x.error());
        if (*x >= enum_count<T>()) return fail(WireError::bad_enum);
        v = static_cast<T>(*x);
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        auto x = r.varint();
        if (!x) return fail(x.error());
        const auto s = static_cast<std::int64_t>((*x >> 1) ^ (~(*x & 1) + 1));
        if (s < std::numeric_limits<T>::min() || s > std::numeric_limits<T>::max())
            return fail(WireError::out_of_range);
        v = static_cast<T>(s);
    } else if constexpr (std::is_integral_v<T>) {
        auto x = r.varint();
        if (!x) return fail(x.error());
        if (*x > std::numeric_limits<T>::max()) return fail(WireError::out_of_range);
        v = static_cast<T>(*x);
    } else if constexpr (std::is_same_v<T, std::string>) {
        auto s = r.bytes();
        if (!s) return fail(s.error());
        v.assign(*s);
    } else if constexpr (is_vector<T>::value) {
        auto n = r.count(1);
        if (!n) return fail(n.error());
        v.clear();
        v.resize(*n);
        for (auto& x : v)
            if (auto ok = get(r, x); !ok) return ok;
    } else if constexpr (is_optional<T>::value) {
        auto flag = r.varint();
        if (!flag) return fail(flag.error());
        if (*flag > 1) return fail(WireError::bad_optional);
        v.reset();
        if (*flag == 1) return get(r, v.emplace());
    } else if constexpr (is_map<T>::value) {
        auto n = r.count(2);
        if (!n) return fail(n.error());
        v.clear();
        for (std::size_t i = 0; i < *n; ++i) {
            typename T::key_type k{};
            if (auto ok = get(r, k); !ok) return ok;
            if (!v.empty() && !(std::prev(v.end())->first < k)) return fail(WireError::not_canonical);
            if (auto ok = get(r, v.emplace_hint(v.end(), std::move(k), typename T::mapped_type{})->second); !ok)
                return ok;
        }
    } else if constexpr (is_variant<T>::value) {
        auto index = r.varint();
        if (!index) return fail(index.error());
        if (*index >= std::variant_size_v<T>) return fail(WireError::bad_variant);
        return get_variant(r, v, static_cast<std::size_t>(*index));
    } else {
        auto& [... xs] = v;
        Read out{};
        ((out ? (out = get(r, xs), 0) : 0), ...);
        if constexpr (requires { { v.valid() } -> std::same_as<bool>; })
            if (out && !v.valid()) return fail(WireError::invalid);
        return out;
    }
    return {};
}

template <Wire T>
[[nodiscard]] std::expected<T, WireError> decode(std::span<const std::byte> bytes) {
    Reader r{bytes};
    T v{};
    if (auto ok = get(r, v); !ok) return std::unexpected(ok.error());
    if (r.remaining() != 0) return std::unexpected(WireError::trailing);
    return v;
}

}  // namespace i4::link
