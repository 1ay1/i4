#pragma once
// i4/wm/state.hpp — the i3 tree, as data.
//
// This is the whole state of the window manager. It is a plain value:
// copyable, comparable, no pointers, no I/O. Three things follow from that,
// and they are the reason i4 is shaped the way it is:
//
//   1. apply() (apply.hpp) is deterministic. The same events from the same
//      State give the same State, on any machine.
//   2. So a head can hold a replica and PREDICT: run the same apply() on a
//      keypress and show the result immediately, while the server confirms.
//      Window-management actions feel local even over a network.
//   3. Undo, replay and bug reports are copies and event logs.
//
// The grammar of the tree
// -----------------------
//
//   State      -> Output*
//   Output     -> Workspace+            (exactly one shown)
//   Workspace  -> tiling: Con(Split)    (always present, may be empty)
//                 floating: Leaf*       (floating containers: later)
//   Con        -> Split(Con+) | Leaf(Window)
//
// What the types enforce:
//   * a Leaf has no children and a Split has no window: Body is a sum type.
//   * where a Con sits decides what it can be: Place is a sum type, and a
//     floating Con's rectangle exists only in the Floating alternative.
//   * Output, Workspace, Con and Window ids are distinct types.
//
// What the types cannot enforce (parent/child agreement, focus stacks being
// permutations of children, shares summing to whole) is checked by check(),
// which every test runs after every single step.

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "i4/base/geom.hpp"
#include "i4/base/id.hpp"

namespace i4::wm {

/// The window manager thinks in one space: logical desktop pixels.
using i4::space::Desktop;

using OutputId = Id<struct OutputTag>;
using WorkspaceId = Id<struct WorkspaceTag>;
using ConId = Id<struct ConTag>;
/// Chosen by the server (one per toplevel), never by the window manager.
using WindowId = Id<struct WindowTag>;

// -- vocabulary ----------------------------------------------------------------

/// A child's share of its split's axis, in parts of `whole`.
///
/// Integers, not doubles, and that is a correctness decision rather than a
/// performance one: a head predicts by running apply() itself, possibly on
/// another CPU built by another compiler. Floating point is allowed to differ
/// there (an fma contracted on ARM, not on x86) and a replica that differs by
/// one ulp differs forever. Integer arithmetic is the same everywhere.
using Share = std::uint32_t;
inline constexpr Share whole = Share{1} << 20;

/// How a Split arranges its children. i3's four layouts.
enum class Layout : std::uint8_t { splith, splitv, tabbed, stacking };

enum class Axis : std::uint8_t { horizontal, vertical };

enum class Direction : std::uint8_t { left, right, up, down };

/// tabbed reads left-to-right like splith; stacking top-to-bottom like splitv.
/// This is what makes `focus right` move between tabs.
[[nodiscard]] constexpr Axis axis_of(Layout l) noexcept {
    switch (l) {
        case Layout::splith:
        case Layout::tabbed: return Axis::horizontal;
        case Layout::splitv:
        case Layout::stacking: return Axis::vertical;
    }
    return Axis::horizontal;
}

[[nodiscard]] constexpr Axis axis_of(Direction d) noexcept {
    return d == Direction::left || d == Direction::right ? Axis::horizontal : Axis::vertical;
}

/// left and up point toward the start of a child list.
[[nodiscard]] constexpr bool backward(Direction d) noexcept {
    return d == Direction::left || d == Direction::up;
}

[[nodiscard]] constexpr Layout split_along(Axis a) noexcept {
    return a == Axis::horizontal ? Layout::splith : Layout::splitv;
}

[[nodiscard]] constexpr bool shows_one_child(Layout l) noexcept {
    return l == Layout::tabbed || l == Layout::stacking;
}

// -- windows ---------------------------------------------------------------------

/// What the server tells the window manager about a toplevel.
struct WindowInfo {
    std::string app_id;
    std::string title;
    /// The size the client asked for, if any. Used when it floats.
    std::optional<Size<Desktop>> size;
    /// Dialogs, fixed-size windows, transients: the server's judgement.
    bool floating = false;

    friend bool operator==(const WindowInfo&, const WindowInfo&) = default;
};

struct Window {
    WindowInfo info;
    ConId con;  // the Leaf holding it

    friend bool operator==(const Window&, const Window&) = default;
};

// -- containers --------------------------------------------------------------------

/// The workspace's tiling area. Never pruned, even when empty.
struct TilingRoot {
    friend bool operator==(TilingRoot, TilingRoot) = default;
};

/// Floating above the workspace, at this rectangle (the frame, in Desktop).
struct Floating {
    Rect<Desktop> rect;
    friend bool operator==(Floating, Floating) = default;
};

/// Inside a Split.
struct Child {
    ConId parent;
    friend bool operator==(Child, Child) = default;
};

using Place = std::variant<TilingRoot, Floating, Child>;

struct Split {
    Layout layout = Layout::splith;
    /// Where `layout toggle split` returns to from tabbed/stacking.
    Layout last_split = Layout::splith;
    /// In order, left-to-right or top-to-bottom.
    std::vector<ConId> children;
    /// Each child's share of the split axis, parallel to children. Every one
    /// is positive and they sum to exactly `whole`.
    std::vector<Share> weights;
    /// The same children, most recently focused first. Descending through
    /// mru heads from the workspace is where focus is.
    std::vector<ConId> mru;

    friend bool operator==(const Split&, const Split&) = default;
};

struct Leaf {
    WindowId window;
    friend bool operator==(Leaf, Leaf) = default;
};

using Body = std::variant<Split, Leaf>;

struct Con {
    WorkspaceId workspace;
    Place place;
    Body body;

    friend bool operator==(const Con&, const Con&) = default;
};

// -- workspaces and outputs ------------------------------------------------------------

struct Workspace {
    std::string name;
    OutputId output;
    ConId tiling;
    /// Bottom to top.
    std::vector<ConId> floating;
    /// tiling and every floating con, most recently focused first.
    std::vector<ConId> mru;
    std::optional<ConId> fullscreen;

    friend bool operator==(const Workspace&, const Workspace&) = default;
};

/// i3 orders and addresses workspaces by the number a name starts with:
/// "3: web" is workspace number 3.
[[nodiscard]] std::optional<int> number_of(const std::string& workspace_name);

struct OutputInfo {
    std::string name;
    Rect<Desktop> rect;
    friend bool operator==(const OutputInfo&, const OutputInfo&) = default;
};

struct Output {
    OutputInfo info;
    std::vector<WorkspaceId> workspaces;
    WorkspaceId shown;

    friend bool operator==(const Output&, const Output&) = default;
};

// -- settings ---------------------------------------------------------------------------

enum class Border : std::uint8_t { none, pixel, normal };

struct Settings {
    Border border = Border::pixel;
    std::int32_t border_width = 2;
    std::int32_t title_height = 20;
    std::int32_t gaps_inner = 0;
    std::int32_t gaps_outer = 0;
    /// At the edge of a workspace with no output beyond it, wrap around.
    bool focus_wrapping = true;

    friend bool operator==(const Settings&, const Settings&) = default;
};

// -- the state ------------------------------------------------------------------------------

/// A window mapped while there was no output to put it on.
struct Unplaced {
    WindowId id;
    WindowInfo info;
    friend bool operator==(const Unplaced&, const Unplaced&) = default;
};

struct State {
    Settings settings;

    // Ordered, so iteration order is part of determinism; node-based, so a
    // reference to one entry survives inserting another (tree surgery
    // creates containers while holding references to their neighbours).
    std::map<OutputId, Output> outputs;
    std::map<WorkspaceId, Workspace> workspaces;
    std::map<ConId, Con> cons;
    std::map<WindowId, Window> windows;
    std::vector<Unplaced> unplaced;

    /// Outputs, most recently focused first.
    std::vector<OutputId> output_mru;
    /// Present iff there is an output. Usually a Leaf; a Split after
    /// `focus parent`; an empty workspace's tiling root.
    std::optional<ConId> focus;
    /// For `workspace back_and_forth`.
    std::optional<std::string> previous_workspace;

    std::uint32_t next_id = 1;

    friend bool operator==(const State&, const State&) = default;
};

/// Every invariant the types cannot carry. Returns the first one broken.
[[nodiscard]] std::expected<void, std::string> check(const State& s);

}  // namespace i4::wm
