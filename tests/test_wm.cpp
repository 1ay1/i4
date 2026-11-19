// tests/test_wm.cpp — the window manager against i3's behaviour.
//
// Each case is something a user of i3 would notice missing, written as the
// commands they would type and the tree they would expect, in i3's own
// notation: "H[a V[b* c]]" — * is focus, ! fullscreen, | floating.
#include "desk.hpp"

using namespace i4::test;

// -- opening and closing ------------------------------------------------------

TEST(first_window_fills_the_workspace) {
    Desk d;
    CHECK_EQ(d.shape(), "H[]*");  // focus is on the empty workspace
    d.open("a");
    CHECK_EQ(d.shape(), "H[a*]");
    CHECK_EQ(d.tile("a")->frame, (Rect<Desktop>{0, 0, 1000, 800}));
}

TEST(new_windows_open_after_the_focused_one) {
    Desk d;
    d.open("a");
    d.open("b");
    CHECK_EQ(d.shape(), "H[a b*]");
    CHECK(d.cmd("focus left"));
    d.open("c");
    CHECK_EQ(d.shape(), "H[a c* b]");
}

TEST(closing_focuses_the_most_recent_not_the_neighbour) {
    Desk d;
    d.open("a");
    d.open("b");
    d.open("c");
    d.cmd("focus left; focus left");
    d.activate("c");
    d.close("c");
    CHECK_EQ(d.shape(), "H[a* b]");
}

TEST(a_container_left_with_one_child_dissolves) {
    Desk d;
    d.open("a");
    d.open("b");
    d.cmd("split v");
    d.open("c");
    CHECK_EQ(d.shape(), "H[a V[b c*]]");
    d.close("c");
    CHECK_EQ(d.shape(), "H[a b*]");
}

TEST(windows_wait_for_an_output) {
    Desk d(false);
    d.open("a");
    CHECK_EQ(d.s.unplaced.size(), 1u);
    d.add_output("A", {0, 0, 1000, 800});
    CHECK_EQ(d.shape(), "H[a*]");
    CHECK(d.s.unplaced.empty());
}

// -- splitting and layouts -------------------------------------------------------

TEST(split_of_a_lone_window_turns_its_container) {
    Desk d;
    d.open("a");
    CHECK(d.cmd("split v"));
    CHECK_EQ(d.shape(), "V[a*]");
    d.open("b");
    CHECK_EQ(d.shape(), "V[a b*]");
}

TEST(split_wraps_and_new_windows_go_inside) {
    Desk d;
    d.open("a");
    d.open("b");
    CHECK(d.cmd("split v"));
    CHECK_EQ(d.shape(), "H[a V[b*]]");
    d.open("c");
    CHECK_EQ(d.shape(), "H[a V[b c*]]");
}

TEST(tabbed_shows_one_and_navigates_sideways) {
    Desk d;
    d.open("a");
    d.open("b");
    CHECK(d.cmd("layout tabbed"));
    CHECK_EQ(d.shape(), "T[a b*]");
    CHECK(d.cmd("focus left"));
    CHECK_EQ(d.shape(), "T[a* b]");

    const auto a = arrange(d.s);
    CHECK_EQ(a.screens.at(0).tiles.size(), 1u);
    CHECK(a.screens.at(0).tiles.at(0).window == d.ids.at("a"));
    CHECK_EQ(a.screens.at(0).titles.size(), 2u);
    CHECK_EQ(a.hidden.size(), 1u);
}

TEST(layout_toggle_split_returns_to_the_last_split) {
    Desk d;
    d.open("a");
    d.open("b");
    d.cmd("layout toggle split");
    CHECK_EQ(d.shape(), "V[a b*]");
    d.cmd("layout tabbed; layout toggle split");
    CHECK_EQ(d.shape(), "V[a b*]");
    d.cmd("layout toggle split");
    CHECK_EQ(d.shape(), "H[a b*]");
}

TEST(focus_parent_selects_a_container) {
    Desk d;
    d.open("a");
    d.open("b");
    d.cmd("split v");
    d.open("c");
    CHECK(d.cmd("focus parent"));
    CHECK_EQ(d.shape(), "H[a V[b c]*]");
    CHECK(d.cmd("layout tabbed"));
    CHECK_EQ(d.shape(), "H[a T[b c]*]");
    CHECK(d.cmd("focus child"));
    CHECK_EQ(d.shape(), "H[a T[b c*]]");
}

// -- focus ---------------------------------------------------------------------

TEST(focus_moves_through_nesting_and_remembers) {
    Desk d;
    d.open("a");
    d.open("b");
    d.cmd("split v");
    d.open("c");
    d.cmd("focus up");
    CHECK_EQ(d.shape(), "H[a V[b* c]]");
    d.cmd("focus left");
    CHECK_EQ(d.shape(), "H[a* V[b c]]");
    d.cmd("focus right");  // back into V, to the one focused last there
    CHECK_EQ(d.shape(), "H[a V[b* c]]");
}

TEST(focus_wraps_at_the_edge_unless_told_not_to) {
    Desk d;
    d.open("a");
    d.open("b");
    d.open("c");
    d.cmd("focus right");
    CHECK_EQ(d.shape(), "H[a* b c]");

    auto st = d.s.settings;
    st.focus_wrapping = false;
    d.step(ev::Configure{st});
    d.cmd("focus left");
    CHECK_EQ(d.shape(), "H[a* b c]");
}

// -- moving --------------------------------------------------------------------------

TEST(move_swaps_with_a_window) {
    Desk d;
    d.open("a");
    d.open("b");
    CHECK(d.cmd("move left"));
    CHECK_EQ(d.shape(), "H[b* a]");
}

TEST(move_enters_a_neighbouring_container) {
    Desk d;
    d.open("a");
    d.open("b");
    d.cmd("split v");
    d.open("c");
    d.cmd("focus left; move right");
    CHECK_EQ(d.shape(), "H[V[b c a*]]");
}

TEST(move_leaves_a_container_at_its_edge) {
    Desk d;
    d.open("a");
    d.open("b");
    d.cmd("split v");
    d.open("c");
    d.cmd("move left");
    CHECK_EQ(d.shape(), "H[a c* b]");
}

TEST(move_across_the_workspace_axis_turns_it) {
    Desk d;
    d.open("a");
    d.open("b");
    d.cmd("move up");
    CHECK_EQ(d.shape(), "V[b* a]");
}

// -- floating and fullscreen ------------------------------------------------------

TEST(floating_round_trip) {
    Desk d;
    d.open("a");
    d.open("b", false, Size<Desktop>{400, 300});
    CHECK(d.cmd("floating toggle"));
    CHECK_EQ(d.shape(), "H[a] | b*");
    // Centred, at the size it asked for plus its 2px border.
    CHECK_EQ(d.tile("b")->frame, (Rect<Desktop>{298, 248, 404, 304}));
    CHECK_EQ(d.tile("b")->content, (Rect<Desktop>{300, 250, 400, 300}));
    CHECK(d.cmd("floating toggle"));
    CHECK_EQ(d.shape(), "H[a b*]");
}

TEST(focus_mode_toggle_crosses_layers) {
    Desk d;
    d.open("a");
    d.open("f", true);
    CHECK_EQ(d.shape(), "H[a] | f*");
    CHECK(d.cmd("focus mode_toggle"));
    CHECK_EQ(d.shape(), "H[a*] | f");
    CHECK(d.cmd("focus mode_toggle"));
    CHECK_EQ(d.shape(), "H[a] | f*");
}

TEST(fullscreen_covers_the_output_and_keeps_focus) {
    Desk d;
    d.open("a");
    d.open("b");
    CHECK(d.cmd("fullscreen"));
    CHECK_EQ(d.shape(), "H[a b*!]");

    const auto a = arrange(d.s);
    CHECK_EQ(a.screens.at(0).tiles.size(), 1u);
    CHECK_EQ(a.screens.at(0).tiles.at(0).frame, (Rect<Desktop>{0, 0, 1000, 800}));
    CHECK(a.screens.at(0).tiles.at(0).layer == Layer::fullscreen);

    d.open("c");  // opens behind, does not steal focus
    CHECK_EQ(d.shape(), "H[a b*! c]");
    CHECK(!d.cmd("move left"));
    d.cmd("fullscreen toggle");
    CHECK_EQ(d.shape(), "H[a b* c]");
}

TEST(nothing_floats_out_of_a_fullscreen_container) {
    // Found by the fuzzer (seed 95).
    Desk d;
    d.open("a");
    d.open("b");
    d.cmd("split v");
    d.open("c");
    d.cmd("focus parent; fullscreen enable; focus child");
    CHECK(!d.cmd("floating toggle"));
    CHECK_EQ(d.shape(), "H[a V[b c*]!]");
}

// -- workspaces ------------------------------------------------------------------------

TEST(empty_workspaces_vanish_when_left) {
    Desk d;
    d.open("a");
    d.cmd("workspace 2");
    CHECK_EQ(d.shape(), "H[]*");
    CHECK_EQ(d.s.workspaces.size(), 2u);
    d.cmd("workspace 3");
    CHECK_EQ(d.s.workspaces.size(), 2u);  // 2 was empty
    d.cmd("workspace 1");
    CHECK_EQ(d.shape(), "H[a*]");
    CHECK_EQ(d.s.workspaces.size(), 1u);
}

TEST(move_to_workspace_leaves_focus_behind) {
    Desk d;
    d.open("a");
    d.open("b");
    CHECK(d.cmd("move container to workspace 2"));
    CHECK_EQ(d.shape(), "H[a*]");
    CHECK_EQ(d.shape_of("2"), "H[b]");
    d.cmd("workspace 2");
    CHECK_EQ(d.shape(), "H[b*]");
}

TEST(back_and_forth) {
    Desk d;
    d.open("a");
    d.cmd("workspace 2");
    d.cmd("workspace back_and_forth");
    CHECK_EQ(d.workspace(), "1");
    d.cmd("workspace back_and_forth");
    CHECK_EQ(d.workspace(), "2");
}

TEST(workspace_number_matches_a_named_workspace) {
    Desk d;
    d.open("a");
    d.cmd("workspace 3: web");
    d.open("b");
    d.cmd("workspace 1; workspace number 3");
    CHECK_EQ(d.workspace(), "3: web");
    CHECK_EQ(d.shape(), "H[b*]");
}

// -- outputs ------------------------------------------------------------------------------

TEST(focus_and_move_cross_outputs) {
    Desk d;
    d.add_output("B", {1000, 0, 1000, 800});
    d.open("a");
    CHECK_EQ(d.output(), "A");
    d.cmd("focus right");
    CHECK_EQ(d.output(), "B");
    CHECK_EQ(d.workspace(), "2");
    d.open("b");
    d.cmd("focus left");
    CHECK_EQ(d.shape(), "H[a*]");
    d.cmd("move right");  // over the edge, and focus follows
    CHECK_EQ(d.output(), "B");
    CHECK_EQ(d.shape(), "H[b a*]");
}

TEST(moving_onto_a_fullscreen_output_does_not_pull_focus) {
    // Found by the fuzzer (seed 76): focus followed the window and escaped
    // the fullscreen con on the other output.
    Desk d;
    d.add_output("B", {1000, 0, 1000, 800});
    d.cmd("focus output B");
    d.open("f");
    d.cmd("fullscreen enable; focus output A");
    d.open("a");
    d.cmd("move right");
    CHECK_EQ(d.output(), "A");
    CHECK_EQ(d.shape_of("2"), "H[f! a]");
}

TEST(removing_an_output_keeps_its_windows) {
    Desk d;
    d.add_output("B", {1000, 0, 1000, 800});
    d.cmd("focus output B");
    d.open("b");
    d.step(ev::OutputRemoved{"B"});
    CHECK_EQ(d.output(), "A");
    CHECK_EQ(d.s.windows.size(), 1u);
    CHECK_EQ(d.shape_of("2"), "H[b]");
}

TEST(move_workspace_to_output) {
    Desk d;
    d.add_output("B", {1000, 0, 1000, 800});
    d.open("a");
    CHECK(d.cmd("move workspace to output right"));
    CHECK_EQ(d.output(), "B");
    CHECK_EQ(d.workspace(), "1");
    CHECK_EQ(d.shape(), "H[a*]");
    CHECK_EQ(d.tile("a")->frame, (Rect<Desktop>{1000, 0, 1000, 800}));
}

// -- the rest -----------------------------------------------------------------------------

TEST(resize_moves_the_shared_edge) {
    Desk d;
    d.open("a");
    d.open("b");
    d.cmd("focus left");
    CHECK(d.cmd("resize grow width 10 px or 10 ppt"));
    CHECK_EQ(d.tile("a")->frame.size.width, 600);
    CHECK_EQ(d.tile("b")->frame.size.width, 400);
}

TEST(kill_and_exec_are_effects) {
    Desk d;
    d.open("a");
    CHECK(d.cmd("kill"));
    CHECK(d.cmd("exec foot --server"));
    CHECK_EQ(d.effects.size(), 2u);
    CHECK(d.effects.at(0) == Effect{fx::Close{d.ids.at("a")}});
    CHECK(d.effects.at(1) == Effect{fx::Spawn{"foot --server"}});
    CHECK_EQ(d.shape(), "H[a*]");  // the server unmaps it, not the wm
}

TEST(a_bad_command_changes_nothing) {
    Desk d;
    d.open("a");
    const auto before = d.s;
    CHECK(!d.cmd("focus sideways"));
    CHECK(d.s == before);
}
