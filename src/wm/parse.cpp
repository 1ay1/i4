// src/wm/parse.cpp — i3's command language into the typed AST.
//
// Two passes. The source is cut into commands at `;` and `,` outside quotes,
// each command into words (a "quoted string" is one word). Then each command
// is read by a small recursive-descent parser over its words. Errors carry
// the byte offset of the word that was wrong.
//
// i4 does not support criteria (`[class="x"] focus`) yet; they are an error
// rather than silently ignored.
#include <charconv>
#include <optional>

#include "i4/wm/command.hpp"

namespace i4::wm {

namespace {

struct Word {
    std::string text;
    std::size_t begin;
    std::size_t end;
    bool quoted;
};

struct Segment {
    std::size_t end;  // of the source text the command came from
    std::vector<Word> words;
};

using Fail = std::unexpected<ParseError>;

bool blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::expected<std::vector<Segment>, ParseError> cut(std::string_view src) {
    std::vector<Segment> out;
    Segment cur{};
    auto finish = [&](std::size_t at) {
        cur.end = at;
        if (!cur.words.empty()) out.push_back(std::move(cur));
        cur = Segment{};
    };
    std::size_t i = 0;
    while (i < src.size()) {
        const char c = src[i];
        if (blank(c)) {
            ++i;
            continue;
        }
        if (c == ';' || c == ',') {
            finish(i++);
            continue;
        }
        Word w{{}, i, i, c == '"'};
        if (w.quoted) {
            for (++i; i < src.size() && src[i] != '"'; ++i) {
                if (src[i] == '\\' && i + 1 < src.size()) ++i;
                w.text += src[i];
            }
            if (i >= src.size()) return Fail(ParseError{w.begin, "unterminated quote"});
            ++i;
        } else {
            while (i < src.size() && !blank(src[i]) && src[i] != ';' && src[i] != ',') w.text += src[i++];
        }
        w.end = i;
        cur.words.push_back(std::move(w));
    }
    finish(src.size());
    return out;
}

class Parser {
public:
    Parser(std::string_view src, const Segment& seg) : src_(src), seg_(seg) {}

    std::expected<Command, ParseError> command() {
        const auto* w = peek();
        if (w->text.starts_with('[')) return error("criteria are not supported yet");
        if (eat("focus")) return focus();
        if (eat("move")) return move();
        if (eat("workspace")) return workspace();
        if (eat("split")) return split();
        if (eat("layout")) return layout();
        if (eat("floating")) return floating();
        if (eat("fullscreen")) return fullscreen();
        if (eat("resize")) return resize();
        if (eat("exec")) return exec();
        if (eat("kill")) return done(cmd::Kill{});
        if (eat("nop")) {
            rest();
            return cmd::Nop{};
        }
        return error("unknown command '" + w->text + "'");
    }

private:
    using R = std::expected<Command, ParseError>;

    [[nodiscard]] const Word* peek() const { return i_ < seg_.words.size() ? &seg_.words[i_] : nullptr; }
    [[nodiscard]] bool last() const { return i_ + 1 == seg_.words.size(); }
    [[nodiscard]] std::size_t here() const { return peek() ? peek()->begin : seg_.end; }

    [[nodiscard]] Fail error(std::string why) const { return Fail(ParseError{here(), std::move(why)}); }

    bool eat(std::string_view keyword) {
        if (peek() && !peek()->quoted && peek()->text == keyword) {
            ++i_;
            return true;
        }
        return false;
    }

    /// A keyword only when it is the final word: `workspace next` is the
    /// next workspace, `workspace next steps` is a workspace by that name.
    bool eat_last(std::string_view keyword) { return last() && eat(keyword); }

    R done(Command c) const {
        if (peek()) return error("unexpected '" + peek()->text + "'");
        return c;
    }

    std::optional<Direction> direction() {
        if (eat("left")) return Direction::left;
        if (eat("right")) return Direction::right;
        if (eat("up")) return Direction::up;
        if (eat("down")) return Direction::down;
        return std::nullopt;
    }

    std::optional<std::int32_t> number() {
        const auto* w = peek();
        if (!w || w->quoted) return std::nullopt;
        std::int32_t n = 0;
        const auto* end = w->text.data() + w->text.size();
        auto [p, ec] = std::from_chars(w->text.data(), end, n);
        if (ec != std::errc{} || p != end) return std::nullopt;
        ++i_;
        return n;
    }

    std::optional<Toggle> toggle() {
        if (eat("enable")) return Toggle::enable;
        if (eat("disable")) return Toggle::disable;
        if (eat("toggle")) return Toggle::toggle;
        return std::nullopt;
    }

    /// Everything left, as written: `workspace 1: www` names "1: www", and
    /// exec hands the shell its arguments untouched. A single quoted word
    /// is unquoted.
    std::string rest() {
        if (!peek()) return {};
        if (last() && peek()->quoted) return seg_.words[i_++].text;
        const auto b = peek()->begin;
        const auto e = seg_.words.back().end;
        i_ = seg_.words.size();
        return std::string(src_.substr(b, e - b));
    }

    std::expected<WorkspaceRef, ParseError> workspace_ref() {
        if (eat_last("next")) return ws::Next{};
        if (eat_last("prev")) return ws::Prev{};
        if (eat_last("next_on_output")) return ws::NextOnOutput{};
        if (eat_last("prev_on_output")) return ws::PrevOnOutput{};
        if (eat_last("back_and_forth")) return ws::BackAndForth{};
        if (eat("number")) {
            // `workspace number 3: www` means number 3.
            const auto at = here();
            const auto name = rest();
            if (auto n = number_of(name)) return ws::Number{*n};
            return Fail(ParseError{at, "expected a workspace number"});
        }
        const auto name = rest();
        if (name.empty()) return error("expected a workspace");
        return ws::Named{name};
    }

    std::expected<OutputRef, ParseError> output_ref() {
        if (last())
            if (auto d = direction()) return OutputRef{*d};
        const auto name = rest();
        if (name.empty()) return error("expected an output");
        return OutputRef{name};
    }

    R focus() {
        if (auto d = direction()) return done(cmd::FocusDirection{*d});
        if (eat("parent")) return done(cmd::FocusParent{});
        if (eat("child")) return done(cmd::FocusChild{});
        if (eat("mode_toggle")) return done(cmd::FocusModeToggle{});
        if (eat("output")) {
            auto o = output_ref();
            if (!o) return Fail(o.error());
            return done(cmd::FocusOutput{*o});
        }
        return error("expected left, right, up, down, parent, child, mode_toggle or output");
    }

    R move() {
        const bool container = eat("container") || eat("window");
        if (auto d = direction()) {
            cmd::Move m{*d};
            if (auto n = number()) {
                m.pixels = *n;
                eat("px");
            }
            return done(m);
        }
        if (!container && eat("workspace")) {
            eat("to");
            if (eat("output")) {
                auto o = output_ref();
                if (!o) return Fail(o.error());
                return done(cmd::MoveWorkspaceToOutput{*o});
            }
            auto w = workspace_ref();
            if (!w) return Fail(w.error());
            return done(cmd::MoveToWorkspace{*w});
        }
        eat("to");
        if (eat("workspace")) {
            auto w = workspace_ref();
            if (!w) return Fail(w.error());
            return done(cmd::MoveToWorkspace{*w});
        }
        if (eat("output")) {
            auto o = output_ref();
            if (!o) return Fail(o.error());
            return done(cmd::MoveToOutput{*o});
        }
        return error("expected a direction, workspace or output");
    }

    R workspace() {
        eat("--no-auto-back-and-forth");
        auto w = workspace_ref();
        if (!w) return Fail(w.error());
        return done(cmd::SwitchWorkspace{*w});
    }

    R split() {
        if (eat("h") || eat("horizontal")) return done(cmd::SplitCon{cmd::SplitKind::horizontal});
        if (eat("v") || eat("vertical")) return done(cmd::SplitCon{cmd::SplitKind::vertical});
        if (eat("t") || eat("toggle")) return done(cmd::SplitCon{cmd::SplitKind::toggle});
        return error("expected h, v or toggle");
    }

    R layout() {
        using K = cmd::LayoutKind;
        if (eat("splith") || eat("default")) return done(cmd::SetLayout{K::splith});
        if (eat("splitv")) return done(cmd::SetLayout{K::splitv});
        if (eat("tabbed")) return done(cmd::SetLayout{K::tabbed});
        if (eat("stacking") || eat("stacked")) return done(cmd::SetLayout{K::stacking});
        if (eat("toggle")) {
            if (eat("split")) return done(cmd::SetLayout{K::toggle_split});
            if (eat("all")) return done(cmd::SetLayout{K::toggle_all});
            return done(cmd::SetLayout{K::toggle});
        }
        return error("expected splith, splitv, tabbed, stacking or toggle");
    }

    R floating() {
        if (auto t = toggle()) return done(cmd::SetFloating{*t});
        return error("expected enable, disable or toggle");
    }

    R fullscreen() { return done(cmd::SetFullscreen{toggle().value_or(Toggle::toggle)}); }

    R resize() {
        cmd::Resize r{};
        if (eat("grow"))
            r.grow = true;
        else if (eat("shrink"))
            r.grow = false;
        else
            return error("expected grow or shrink");

        if (eat("width") || eat("left") || eat("right"))
            r.axis = Axis::horizontal;
        else if (eat("height") || eat("up") || eat("down"))
            r.axis = Axis::vertical;
        else
            return error("expected width or height");

        if (auto n = number()) {
            if (eat("ppt")) {
                r.points = *n;
            } else {
                eat("px");
                r.pixels = *n;
            }
        }
        if (eat("or")) {
            const auto n = number();
            if (!n) return error("expected a number after 'or'");
            if (!eat("ppt")) return error("expected 'ppt'");
            r.points = *n;
        }
        return done(r);
    }

    R exec() {
        eat("--no-startup-id");
        auto c = rest();
        if (c.empty()) return error("exec needs a command");
        return cmd::Exec{std::move(c)};
    }

    std::string_view src_;
    const Segment& seg_;
    std::size_t i_ = 0;
};

}  // namespace

std::expected<Script, ParseError> parse(std::string_view source) {
    auto segments = cut(source);
    if (!segments) return Fail(segments.error());
    Script out;
    for (const auto& seg : *segments) {
        auto c = Parser(source, seg).command();
        if (!c) return Fail(c.error());
        out.push_back(std::move(*c));
    }
    return out;
}

}  // namespace i4::wm
