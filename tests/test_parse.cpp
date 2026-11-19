// tests/test_parse.cpp — i3's command language into the AST.
#include "check.hpp"
#include "i4/wm/command.hpp"

using namespace i4::wm;

namespace {

bool parses_to(std::string_view src, const Script& want) {
    auto got = parse(src);
    return got && *got == want;
}

std::size_t error_at(std::string_view src) {
    auto got = parse(src);
    return got ? std::string_view::npos : got.error().offset;
}

}  // namespace

TEST(single_commands) {
    CHECK(parses_to("focus left", {cmd::FocusDirection{Direction::left}}));
    CHECK(parses_to("focus parent", {cmd::FocusParent{}}));
    CHECK(parses_to("kill", {cmd::Kill{}}));
    CHECK(parses_to("fullscreen", {cmd::SetFullscreen{Toggle::toggle}}));
    CHECK(parses_to("floating enable", {cmd::SetFloating{Toggle::enable}}));
    CHECK(parses_to("split toggle", {cmd::SplitCon{cmd::SplitKind::toggle}}));
    CHECK(parses_to("layout stacked", {cmd::SetLayout{cmd::LayoutKind::stacking}}));
    CHECK(parses_to("layout toggle split", {cmd::SetLayout{cmd::LayoutKind::toggle_split}}));
}

TEST(sequences) {
    CHECK(parses_to("focus left; move right 20 px, kill",
                    {cmd::FocusDirection{Direction::left}, cmd::Move{Direction::right, 20}, cmd::Kill{}}));
    CHECK(parses_to("", {}));
    CHECK(parses_to("kill;", {cmd::Kill{}}));
}

TEST(workspaces) {
    CHECK(parses_to("workspace 1: www", {cmd::SwitchWorkspace{ws::Named{"1: www"}}}));
    CHECK(parses_to("workspace \"my ws\"", {cmd::SwitchWorkspace{ws::Named{"my ws"}}}));
    CHECK(parses_to("workspace next", {cmd::SwitchWorkspace{ws::Next{}}}));
    CHECK(parses_to("workspace next steps", {cmd::SwitchWorkspace{ws::Named{"next steps"}}}));
    CHECK(parses_to("workspace number 3: web", {cmd::SwitchWorkspace{ws::Number{3}}}));
    CHECK(parses_to("move container to workspace number 3", {cmd::MoveToWorkspace{ws::Number{3}}}));
    CHECK(parses_to("move workspace 4", {cmd::MoveToWorkspace{ws::Named{"4"}}}));
    CHECK(parses_to("move workspace to output left", {cmd::MoveWorkspaceToOutput{Direction::left}}));
    CHECK(parses_to("move window to output DP-1", {cmd::MoveToOutput{std::string("DP-1")}}));
}

TEST(resize) {
    CHECK(parses_to("resize shrink height 5 px or 15 ppt", {cmd::Resize{false, Axis::vertical, 5, 15}}));
    CHECK(parses_to("resize grow width", {cmd::Resize{true, Axis::horizontal, 10, 10}}));
    CHECK(parses_to("resize grow left 30 ppt", {cmd::Resize{true, Axis::horizontal, 10, 30}}));
}

TEST(exec_keeps_its_arguments_as_written) {
    CHECK(parses_to("exec --no-startup-id foot -e htop", {cmd::Exec{"foot -e htop"}}));
    CHECK(parses_to("exec sh -c \"echo hi\"", {cmd::Exec{"sh -c \"echo hi\""}}));
    CHECK(parses_to("exec \"firefox\"", {cmd::Exec{"firefox"}}));
}

TEST(errors_point_at_the_word) {
    CHECK_EQ(error_at("focus sideways"), 6u);
    CHECK_EQ(error_at("frobnicate"), 0u);
    CHECK_EQ(error_at("kill now"), 5u);
    CHECK_EQ(error_at("focus left; move"), 16u);
    CHECK_EQ(error_at("workspace \"open"), 10u);
    CHECK_EQ(error_at("[class=\"x\"] kill"), 0u);
}
