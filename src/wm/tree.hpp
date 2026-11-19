#pragma once
// src/wm/tree.hpp — tree surgery, shared by apply, arrange and shape.
// Internal to the wm layer: nothing outside src/wm includes this.
//
// The rules every function here keeps
// -----------------------------------
//
//   * unlink() takes a con out of its parent and does nothing else.
//     normalise() prunes and collapses, and callers run it only AFTER the
//     con is somewhere new. Pruning first would erase the very containers
//     the destination was computed against.
//
//   * Nothing is left pointing at a con that is gone. A Split that collapses
//     hands its place in every MRU list, focus and fullscreen to the child
//     that replaces it.
//
//   * Collapse happens only when a child LEAVES. `split v` on a lone window
//     makes V[a] on purpose (the next window opens inside it), so a
//     one-child Split is only redundant once something has left it.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "i4/wm/state.hpp"

namespace i4::wm::tree {

// -- lookup ----------------------------------------------------------------

[[nodiscard]] Con& con(State& s, ConId id);
[[nodiscard]] const Con& con(const State& s, ConId id);
[[nodiscard]] Split& split(State& s, ConId id);
[[nodiscard]] const Split& split(const State& s, ConId id);
[[nodiscard]] Workspace& workspace(State& s, WorkspaceId id);
[[nodiscard]] const Workspace& workspace(const State& s, WorkspaceId id);
[[nodiscard]] Output& output(State& s, OutputId id);
[[nodiscard]] const Output& output(const State& s, OutputId id);

[[nodiscard]] bool is_split(const State& s, ConId id);
[[nodiscard]] bool is_root(const State& s, ConId id);
[[nodiscard]] bool is_floating(const State& s, ConId id);
[[nodiscard]] std::optional<ConId> parent(const State& s, ConId id);
/// The tiling root or floating con this one is in.
[[nodiscard]] ConId top(const State& s, ConId id);
/// `id` is `ancestor` or somewhere below it.
[[nodiscard]] bool inside(const State& s, ConId id, ConId ancestor);
[[nodiscard]] std::size_t index_in(const Split& split, ConId child);

[[nodiscard]] std::optional<WorkspaceId> focused_workspace(const State& s);
[[nodiscard]] std::optional<OutputId> focused_output(const State& s);

/// Follow MRU down to a leaf (or an empty root).
[[nodiscard]] ConId descend(const State& s, ConId id);
/// Where focus lands on entering a workspace: inside its fullscreen con if
/// it has one, else down its MRU.
[[nodiscard]] ConId workspace_focus(const State& s, WorkspaceId ws);
[[nodiscard]] ConId tiling_focus(const State& s, WorkspaceId ws);

[[nodiscard]] std::vector<WindowId> windows_in(const State& s, ConId id);
[[nodiscard]] bool is_empty(const State& s, WorkspaceId ws);

// -- building ----------------------------------------------------------------

template <class I>
[[nodiscard]] I fresh(State& s) {
    return I{s.next_id++};
}

ConId create_con(State& s, WorkspaceId ws, Place place, Body body);
WorkspaceId create_workspace(State& s, OutputId out, std::string name);
/// i3's order: numbered workspaces by number, then the rest by name.
void sort_workspaces(State& s, OutputId out);
[[nodiscard]] std::string first_free_number(const State& s);

/// Into `parent` at `index`. The child takes an equal share of the axis;
/// the others give it up in proportion. It goes to the BACK of the MRU:
/// insertion is not focus.
void insert(State& s, ConId parent, std::size_t index, ConId child);
void float_in(State& s, WorkspaceId ws, ConId c, Rect<Desktop> rect);
/// Out of its parent (or the floating list). Returns the old parent when
/// there was one, for the caller to normalise once the con has landed.
std::optional<ConId> unlink(State& s, ConId c);
/// Prune `split` if it is empty, collapse it if one child is left.
void normalise(State& s, ConId split);
/// A subtree that is already unlinked, and the windows in it.
void erase(State& s, ConId c);

// -- focus --------------------------------------------------------------------

/// Bring `c` to the front of every MRU list up to its workspace (and to
/// the top of the floating stack). Does not move focus.
void raise(State& s, ConId c);
/// Focus `c`: raise it, show its workspace, focus its output. Collects the
/// workspace focus left if that is now empty and hidden.
void focus(State& s, ConId c);
/// Destroy `ws` if it is empty and not shown.
void gc(State& s, WorkspaceId ws);
/// After surgery: if focus pointed at a con that is gone, put it back
/// inside `ws`.
void refocus_if_lost(State& s, WorkspaceId ws);

// -- decorations (arrange.cpp) --------------------------------------------------

/// The frame a window needs around content of this size.
[[nodiscard]] Size<Desktop> decorated(const Settings& st, Size<Desktop> content);
/// The content inside a frame. `titled`: the window has its own title bar
/// (not when a tab or stack row stands in for it).
[[nodiscard]] Rect<Desktop> undecorated(const Settings& st, Rect<Desktop> frame, bool titled);

/// "T[a b]": a con in shape() notation, without marks (shape.cpp).
[[nodiscard]] std::string describe(const State& s, ConId c);

}  // namespace i4::wm::tree
