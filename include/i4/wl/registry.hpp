// registry.hpp — typed global discovery.
//
// `Registry` turns the untyped `wl_registry.global` event stream into a typed
// lookup: you ask for an interface *by its type*, and you get it bound at a
// version the compiler already checked, or nothing.
//
//   auto compositor = reg.bind<wl_compositor, 4>();  // Proxy<wl_compositor,4>?
//
// The interface name and maximum version come from the generated record, so
// there is no string to typo and no version constant to get out of sync.
#pragma once

#include <map>
#include <memory>

#include "listener.hpp"

namespace i4::wl {

/// One advertised global, before binding.
struct Global {
    std::uint32_t name;     ///< the compositor's handle for it
    std::string interface;  ///< owned: the event's view dies with the buffer
    std::uint32_t version;  ///< the highest version the compositor offers
};

/// A global bound within a range (Registry::bind_range): the proxy, typed
/// for the most the code handles, and the version the compositor really
/// agreed to. Code that uses a newer request checks `version` first.
template <Interface I, std::uint32_t Max>
struct Bound {
    Proxy<I, Max> proxy;
    std::uint32_t version = 1;
};

template <class Proto>
class Registry {
public:
    Registry() = default;

    /// Fetch the registry and pump one roundtrip so `globals()` is populated.
    /// Blocks: for a standalone client. A client driven by a runtime uses
    /// listen() instead.
    static Registry discover(Connection& conn, Proxy<typename Proto::wl_display> display) {
        Registry r = listen(conn, display);
        conn.roundtrip();
        return r;
    }

    /// Ask for the registry and return at once. The globals arrive as the
    /// connection is dispatched (Connection::ready() from a runtime); a
    /// wl_display.sync sent right after this answers once the first batch is
    /// in. Never waits: the one to use from a program whose runtime owns
    /// waiting.
    static Registry listen(Connection& conn, Proxy<typename Proto::wl_display> display) {
        Registry r;
        r.conn_ = &conn;
        // The handlers write through a pointer that stays put when the
        // Registry is moved. They used to capture `&r`, the local being
        // returned, so any event dispatched after the return wrote into a
        // dead object; it only worked because discover() drained them all
        // before returning.
        auto* globals = r.globals_.get();
        r.registry_ = own(display.template send<typename Proto::wl_display::requests::get_registry>());
        r.registry_.template on<typename Proto::wl_registry::events::global>(
            [globals](std::uint32_t name, std::string_view iface, std::uint32_t ver) {
                (*globals)[std::string(iface)] = Global{name, std::string(iface), ver};
            });
        r.registry_.template on<typename Proto::wl_registry::events::global_remove>(
            [globals](std::uint32_t name) {
                for (auto it = globals->begin(); it != globals->end(); ++it) {
                    if (it->second.name == name) { globals->erase(it); break; }
                }
            });
        return r;
    }

    const std::map<std::string, Global>& globals() const { return *globals_; }

    /// Is interface I advertised, and at what version?
    template <Interface I>
    std::optional<Global> find() const {
        auto it = globals_->find(std::string(I::wl_name));
        if (it == globals_->end()) return std::nullopt;
        return it->second;
    }

    /// Bind interface I at exactly version V.
    ///
    /// Returns nullopt if the compositor does not advertise I, or advertises it
    /// below V. The *upper* bound is a compile error, not a runtime one: you
    /// cannot ask for a version the protocol definition doesn't have.
    template <Interface I, std::uint32_t V = I::wl_version>
    std::optional<Proxy<I, V>> bind() const {
        static_assert(V >= 1 && V <= I::wl_version,
                      "i4::wl: requested version exceeds what this protocol "
                      "definition declares for the interface.");
        auto g = find<I>();
        if (!g || g->version < V) return std::nullopt;
        // wl_registry.bind(uint name, new_id<interface, version> id)
        return registry_.proxy().template send_bind<I, V, typename Proto::wl_registry::requests::bind>(
            g->name, BindSpec{I::wl_name, V});
    }

    /// Bind at the best version both sides support, reported at runtime.
    /// Use when you can degrade gracefully; prefer `bind<I, V>()` otherwise.
    template <Interface I>
    std::optional<Proxy<I>> bind_best() const {
        auto g = find<I>();
        if (!g) return std::nullopt;
        const std::uint32_t v = std::min(g->version, I::wl_version);
        return registry_.proxy().template send_bind<I, I::wl_version, typename Proto::wl_registry::requests::bind>(
            g->name, BindSpec{I::wl_name, v});
    }

    /// Bind between Min and Max: the newest the compositor has, up to Max,
    /// and nothing below Min. The handle is typed for Max (the most the
    /// caller's code handles); what was really bound is `version` in the
    /// result, and events newer than it simply never arrive. The shape of a
    /// client that uses new features when they're there and still works
    /// when they're not.
    template <Interface I, std::uint32_t Min, std::uint32_t Max = I::wl_version>
    std::optional<Bound<I, Max>> bind_range() const {
        static_assert(1 <= Min && Min <= Max && Max <= I::wl_version,
                      "i4::wl: bind_range needs 1 <= Min <= Max <= the protocol's version.");
        auto g = find<I>();
        if (!g || g->version < Min) return std::nullopt;
        const std::uint32_t v = std::min(g->version, Max);
        auto p = registry_.proxy().template send_bind<I, Max, typename Proto::wl_registry::requests::bind>(
            g->name, BindSpec{I::wl_name, v});
        return Bound<I, Max>{p, v};
    }

private:
    Connection* conn_ = nullptr;
    Owned<typename Proto::wl_registry> registry_{};
    // Heap-allocated so the event handlers can hold a pointer that survives
    // the Registry being moved.
    std::unique_ptr<std::map<std::string, Global>> globals_ =
        std::make_unique<std::map<std::string, Global>>();
};

}  // namespace i4::wl
