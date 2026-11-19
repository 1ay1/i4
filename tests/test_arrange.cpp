// tests/test_arrange.cpp — the tree as rectangles.
#include "desk.hpp"

using namespace i4::test;

TEST(siblings_share_every_pixel) {
    // 1000 / 3 does not divide: the shares must still meet exactly and end
    // at the edge, with no line of desktop between windows.
    Desk d;
    d.open("a");
    d.open("b");
    d.open("c");
    const auto a = *d.tile("a"), b = *d.tile("b"), c = *d.tile("c");
    CHECK_EQ(a.frame.left(), 0);
    CHECK_EQ(a.frame.right(), b.frame.left());
    CHECK_EQ(b.frame.right(), c.frame.left());
    CHECK_EQ(c.frame.right(), 1000);
}

TEST(gaps) {
    Desk d;
    auto st = d.s.settings;
    st.gaps_inner = 10;
    st.gaps_outer = 20;
    d.step(ev::Configure{st});
    d.open("a");
    d.open("b");
    CHECK_EQ(d.tile("a")->frame, (Rect<Desktop>{20, 20, 475, 760}));
    CHECK_EQ(d.tile("b")->frame, (Rect<Desktop>{505, 20, 475, 760}));
}

TEST(normal_border_has_a_title_bar) {
    Desk d;
    auto st = d.s.settings;
    st.border = Border::normal;
    d.step(ev::Configure{st});
    d.open("a");
    const auto a = arrange(d.s);
    CHECK_EQ(a.screens.at(0).titles.size(), 1u);
    CHECK_EQ(a.screens.at(0).titles.at(0).rect, (Rect<Desktop>{0, 0, 1000, 20}));
    CHECK_EQ(a.screens.at(0).titles.at(0).text, std::string("a title"));
    CHECK_EQ(d.tile("a")->content, (Rect<Desktop>{2, 20, 996, 778}));
}

TEST(hidden_tabs_keep_their_size) {
    Desk d;
    d.open("a");
    d.open("b");
    d.cmd("layout tabbed");
    const auto a = *d.tile("a"), b = *d.tile("b");
    CHECK_EQ(a.frame, b.frame);
    CHECK_EQ(b.frame, (Rect<Desktop>{0, 20, 1000, 780}));
    const auto arr = arrange(d.s);
    CHECK(arr.screens.at(0).titles.at(1).active);
    CHECK(!arr.screens.at(0).titles.at(0).active);
}

TEST(windows_on_hidden_workspaces_stay_sized) {
    Desk d;
    d.open("a");
    d.cmd("workspace 2");
    const auto arr = arrange(d.s);
    CHECK(arr.screens.at(0).tiles.empty());
    CHECK_EQ(arr.hidden.size(), 1u);
    CHECK_EQ(arr.hidden.at(0).frame, (Rect<Desktop>{0, 0, 1000, 800}));
}

TEST(keyboard_focus_follows_focus_down_into_containers) {
    Desk d;
    d.open("a");
    d.open("b");
    d.cmd("split v");
    d.open("c");
    d.cmd("focus parent");
    CHECK(arrange(d.s).keyboard_focus == d.ids.at("c"));
    const auto c = *d.tile("c");
    CHECK(c.focused);
    CHECK(!d.tile("a")->focused);
}

TEST(no_output_no_screens) {
    Desk d(false);
    d.open("a");
    const auto arr = arrange(d.s);
    CHECK(arr.screens.empty());
    CHECK(!arr.keyboard_focus.has_value());
}
