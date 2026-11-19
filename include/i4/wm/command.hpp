#pragma once
// i4/wm/command.hpp — i3's command language as a typed AST.
//
//   parse("focus left; move container to workspace 3")
//     -> Script{ FocusDirection{left}, MoveToWorkspace{Named{"3"}} }
//
// Parsing and meaning are separate: parse() only builds the tree, and
// apply() (apply.hpp) gives it meaning. So a binding, the IPC socket and a
// config file all produce the same Command values, and a head predicting a
// keypress runs exactly what the server will.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "i4/wm/state.hpp"

namespace i4::wm {

enum class Toggle : std::uint8_t { enable, disable, toggle };

// -- references -------------------------------------------------------------

namespace ws {
struct Named {
    std::string name;
    friend bool operator==(const Named&, const Named&) = default;
};
/// `workspace number 3` matches "3", "3: web", ... and creates "3".
struct Number {
    int number;
    friend bool operator==(Number, Number) = default;
};
struct Next { friend bool operator==(Next, Next) = default; };
struct Prev { friend bool operator==(Prev, Prev) = default; };
struct NextOnOutput { friend bool operator==(NextOnOutput, NextOnOutput) = default; };
struct PrevOnOutput { friend bool operator==(PrevOnOutput, PrevOnOutput) = default; };
struct BackAndForth { friend bool operator==(BackAndForth, BackAndForth) = default; };
}  // namespace ws

using WorkspaceRef = std::variant<ws::Named, ws::Number, ws::Next, ws::Prev, ws::NextOnOutput,
                                  ws::PrevOnOutput, ws::BackAndForth>;

/// An output by direction from the focused one, or by name.
using OutputRef = std::variant<Direction, std::string>;

// -- commands -----------------------------------------------------------------

namespace cmd {
struct FocusDirection { Direction direction; friend bool operator==(FocusDirection, FocusDirection) = default; };
struct FocusParent { friend bool operator==(FocusParent, FocusParent) = default; };
struct FocusChild { friend bool operator==(FocusChild, FocusChild) = default; };
/// Between the tiling and floating layers (`focus mode_toggle`).
struct FocusModeToggle { friend bool operator==(FocusModeToggle, FocusModeToggle) = default; };
struct FocusOutput { OutputRef output; friend bool operator==(const FocusOutput&, const FocusOutput&) = default; };

/// Tiling: i3's move (swap, enter a neighbour, or leave the container).
/// Floating: shift by `pixels`.
struct Move {
    Direction direction;
    std::int32_t pixels = 10;
    friend bool operator==(Move, Move) = default;
};
struct MoveToWorkspace { WorkspaceRef workspace; friend bool operator==(const MoveToWorkspace&, const MoveToWorkspace&) = default; };
struct MoveToOutput { OutputRef output; friend bool operator==(const MoveToOutput&, const MoveToOutput&) = default; };
struct MoveWorkspaceToOutput { OutputRef output; friend bool operator==(const MoveWorkspaceToOutput&, const MoveWorkspaceToOutput&) = default; };

struct SwitchWorkspace { WorkspaceRef workspace; friend bool operator==(const SwitchWorkspace&, const SwitchWorkspace&) = default; };

enum class SplitKind : std::uint8_t { horizontal, vertical, toggle };
struct SplitCon { SplitKind kind; friend bool operator==(SplitCon, SplitCon) = default; };

enum class LayoutKind : std::uint8_t { splith, splitv, tabbed, stacking, toggle, toggle_split, toggle_all };
struct SetLayout { LayoutKind kind; friend bool operator==(SetLayout, SetLayout) = default; };

struct SetFloating { Toggle mode; friend bool operator==(SetFloating, SetFloating) = default; };
struct SetFullscreen { Toggle mode; friend bool operator==(SetFullscreen, SetFullscreen) = default; };
struct Kill { friend bool operator==(Kill, Kill) = default; };

/// `resize grow width 10 px or 10 ppt`. Tiling cons use ppt (percentage
/// points of the parent), floating ones px — as in i3.
struct Resize {
    bool grow;
    Axis axis;  // width = horizontal
    std::int32_t pixels = 10;
    std::int32_t points = 10;
    friend bool operator==(Resize, Resize) = default;
};

struct Exec { std::string command; friend bool operator==(const Exec&, const Exec&) = default; };
struct Nop { friend bool operator==(Nop, Nop) = default; };
}  // namespace cmd

using Command = std::variant<cmd::FocusDirection, cmd::FocusParent, cmd::FocusChild,
                             cmd::FocusModeToggle, cmd::FocusOutput, cmd::Move,
                             cmd::MoveToWorkspace, cmd::MoveToOutput, cmd::MoveWorkspaceToOutput,
                             cmd::SwitchWorkspace, cmd::SplitCon, cmd::SetLayout, cmd::SetFloating,
                             cmd::SetFullscreen, cmd::Kill, cmd::Resize, cmd::Exec, cmd::Nop>;

/// Commands separated by `;` or `,`, run in order.
using Script = std::vector<Command>;

struct ParseError {
    std::size_t offset;  // into the source text
    std::string message;
    friend bool operator==(const ParseError&, const ParseError&) = default;
};

[[nodiscard]] std::expected<Script, ParseError> parse(std::string_view source);

}  // namespace i4::wm
