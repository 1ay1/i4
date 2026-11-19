// typestate.hpp — protocol STATE in the type, not in a comment.
//
// The algebra in <wl/algebra.hpp> types a protocol's DATA: what arguments a
// message carries, what object a request creates. That is a Π/Σ structure and
// it is fully checked.
//
// It says nothing about TIME. Wayland's XML does not either: "you must
// ack_configure before attaching a buffer" lives in an English <description>,
// so no generator can derive it. It has to be written by hand, once, per
// protocol that has a handshake.
//
// What this header provides is the vocabulary to write it in: a session type,
// where a request is only callable in the states the protocol permits and
// returns the state it leaves you in. Illegal orderings then fail to compile
// rather than being answered with a `xdg_surface.error.unconfigured_buffer`
// that kills the connection.
//
//   Configuring s = ...;             // fresh xdg_surface
//   auto ready = s.ack(serial);      // Configured
//   ready.attach(buf).commit();      // only Configured has attach()
//   s.attach(buf);                   // COMPILE ERROR: not yet configured
#pragma once

#include "affine.hpp"

namespace i4::wl::session {

// ---------------------------------------------------------------------------
// A transition is a function on states. Writing them as types means the
// compiler checks the state machine, and means the machine is READABLE: the
// legal moves are the member functions that exist.
// ---------------------------------------------------------------------------

/// Marker: this object has been created but the compositor has not yet sent
/// its first configure. Per xdg-shell, the only legal moves are to commit an
/// empty surface (to provoke the configure) and to ack one when it arrives.
struct Unconfigured {};

/// Marker: a configure has been acked, so the surface may carry content.
struct Configured {};

// ---------------------------------------------------------------------------
// XdgSurface<State> — the xdg-shell handshake as a session type.
//
// This is hand-written because the constraint it encodes is hand-written in
// the protocol's prose. It is ~60 lines for the one protocol in common use
// that has a real handshake, which is the honest cost of typing TIME rather
// than pretending the data algebra already did it.
// ---------------------------------------------------------------------------
template <class Proto, class State = Unconfigured>
class XdgSurface {
    using xdg_surface = typename Proto::xdg_surface;
    using wl_surface = typename Proto::wl_surface;

public:
    XdgSurface(Proxy<xdg_surface> xdg, Proxy<wl_surface> surf)
        : xdg_(xdg), surface_(surf) {}

    /// Acknowledge a configure. Legal in any state; the result is Configured,
    /// so the buffer-attaching API only becomes reachable after this call.
    [[nodiscard]] XdgSurface<Proto, Configured> ack(std::uint32_t serial) const {
        xdg_.template send<typename xdg_surface::requests::ack_configure>(serial);
        return XdgSurface<Proto, Configured>{xdg_, surface_};
    }

    /// Attach a buffer. Only exists once Configured: this is the whole point.
    template <class Buffer>
    const XdgSurface& attach(Buffer&& buf, std::int32_t x = 0, std::int32_t y = 0) const
        requires std::is_same_v<State, Configured>
    {
        surface_.template send<typename wl_surface::requests::attach>(
            std::forward<Buffer>(buf), x, y);
        return *this;
    }

    const XdgSurface& damage(std::int32_t x, std::int32_t y, std::int32_t w,
                             std::int32_t h) const
        requires std::is_same_v<State, Configured>
    {
        surface_.template send<typename wl_surface::requests::damage>(x, y, w, h);
        return *this;
    }

    /// Commit. Legal in both states: an Unconfigured commit is exactly how you
    /// ask the compositor for the first configure.
    const XdgSurface& commit() const {
        surface_.template send<typename wl_surface::requests::commit>();
        return *this;
    }

    Proxy<xdg_surface> xdg() const { return xdg_; }
    Proxy<wl_surface> surface() const { return surface_; }

private:
    Proxy<xdg_surface> xdg_;
    Proxy<wl_surface> surface_;

    template <class, class>
    friend class XdgSurface;
};

}  // namespace i4::wl::session
