#pragma once
// i4/wm/apply.hpp — the window manager, as one function.
//
//   Outcome apply(State&, const Event&)
//
// Everything the window manager does happens here, and nothing here does
// I/O. Events come in from the server (a window mapped, an output plugged,
// a command typed); the State changes; anything that needs the outside
// world comes back as an Effect for the caller to carry out.
//
// It is deterministic: no clocks, no randomness, no addresses, ordered
// containers only. That is a contract, and tests/test_determinism.cpp holds
// it to it, because the head relies on it to predict.

#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "i4/wm/command.hpp"
#include "i4/wm/state.hpp"

namespace i4::wm {

// -- events: what happened ----------------------------------------------------

namespace ev {
struct OutputAdded { OutputInfo info; };
struct OutputChanged { OutputInfo info; };  // matched by name
struct OutputRemoved { std::string name; };
struct WindowMapped { WindowId id; WindowInfo info; };
struct WindowChanged { WindowId id; WindowInfo info; };
struct WindowUnmapped { WindowId id; };
/// A click, or xdg-activation: focus this window.
struct WindowActivated { WindowId id; };
struct Run { Script script; };
struct Configure { Settings settings; };
}  // namespace ev

using Event = std::variant<ev::OutputAdded, ev::OutputChanged, ev::OutputRemoved,
                           ev::WindowMapped, ev::WindowChanged, ev::WindowUnmapped,
                           ev::WindowActivated, ev::Run, ev::Configure>;

// -- effects: what the outside world must do ------------------------------------
//
// Only what cannot be derived from State. Geometry and focus are not effects:
// the server diffs arrange(State) against what it last sent (arrange.hpp).

namespace fx {
struct Spawn { std::string command; friend bool operator==(const Spawn&, const Spawn&) = default; };
struct Close { WindowId window; friend bool operator==(Close, Close) = default; };
}  // namespace fx

using Effect = std::variant<fx::Spawn, fx::Close>;

/// One result per command run, as i3's IPC reports them.
using Result = std::expected<void, std::string>;

struct Outcome {
    std::vector<Effect> effects;
    std::vector<Result> results;
};

Outcome apply(State& state, const Event& event);

/// Convenience: parse and run a command string. A parse error is one failed
/// result and changes nothing.
Outcome run(State& state, std::string_view commands);

}  // namespace i4::wm
