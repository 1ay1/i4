#pragma once
// i4/wm/arrange.hpp — where everything goes. The server -> head contract.
//
// arrange() turns the tree into rectangles: one Screen per output with its
// visible windows bottom-to-top and the title bars to draw, plus every
// hidden window with the size it would have (i3 keeps hidden tabs and
// windows on other workspaces sized, so switching to them is instant).
//
// It is a pure function of State. The server diffs consecutive
// Arrangements to know which windows to configure; the head draws one. Over
// the network an Arrangement (or its diff) is the whole layout message.

#include <optional>
#include <string>
#include <vector>

#include "i4/base/geom.hpp"
#include "i4/wm/state.hpp"

namespace i4::wm {

enum class Layer : std::uint8_t { tiling, floating, fullscreen };

struct Tile {
    WindowId window;
    ConId con;
    Rect<Desktop> frame;    // including border and title bar
    Rect<Desktop> content;  // the client's surface
    Layer layer;
    bool focused;  // inside the focused con: draw the focus colour

    friend bool operator==(const Tile&, const Tile&) = default;
};

/// A title bar: a tab, a stacked row, or a window's own (border normal).
struct Title {
    ConId con;
    Rect<Desktop> rect;
    std::string text;
    bool focused;  // inside the focused con
    bool active;   // the visible child of a tabbed/stacking container

    friend bool operator==(const Title&, const Title&) = default;
};

struct Screen {
    OutputId output;
    std::string name;
    Rect<Desktop> rect;
    WorkspaceId workspace;
    std::string workspace_name;
    std::vector<Tile> tiles;  // visible, bottom to top
    std::vector<Title> titles;

    friend bool operator==(const Screen&, const Screen&) = default;
};

struct Arrangement {
    std::vector<Screen> screens;
    /// Not shown, but sized: background tabs, other workspaces.
    std::vector<Tile> hidden;
    /// Where keyboard input goes.
    std::optional<WindowId> keyboard_focus;

    friend bool operator==(const Arrangement&, const Arrangement&) = default;
};

[[nodiscard]] Arrangement arrange(const State& state);

/// One con's frame, as arrange() would place it (shown or not).
[[nodiscard]] std::optional<Rect<Desktop>> frame_of(const State& state, ConId con);

/// i3's tree notation, for tests, logs and `i4-msg -t get_tree --short`:
/// "H[a V[b* c]] | f" — layouts H V T S, window app_ids, `*` on the
/// focused con, `!` on the fullscreen one, floating cons after a `|`.
[[nodiscard]] std::string shape(const State& state, WorkspaceId workspace);
[[nodiscard]] std::string shape(const State& state);  // the focused workspace

}  // namespace i4::wm
