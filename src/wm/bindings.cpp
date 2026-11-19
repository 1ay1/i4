// src/wm/bindings.cpp — chords and the default table.
#include "i4/wm/bindings.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "i4/base/expect.hpp"

namespace i4::wm {

namespace {

bool iequal(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        auto low = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        return low(x) == low(y);
    });
}

constexpr std::array<std::string_view, 16> canonical = {
    "Return", "space", "Escape", "Tab", "BackSpace", "Delete", "Left", "Right",
    "Up", "Down", "Home", "End", "Prior", "Next", "minus", "semicolon",
};

}  // namespace

std::string normal_key(std::string_view key) {
    if (key.size() == 1) {
        const char c = key[0];
        return std::string(1, c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    for (auto name : canonical)
        if (iequal(key, name)) return std::string(name);
    if (iequal(key, "enter")) return "Return";
    return std::string(key);
}

std::expected<Chord, ParseError> parse_chord(std::string_view text, Mod mod) {
    Chord c;
    std::size_t start = 0;
    while (true) {
        const auto plus = text.find('+', start);
        const auto part = text.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start);
        if (part.empty()) return std::unexpected(ParseError{start, "empty part in key chord"});
        if (plus == std::string_view::npos) {
            c.key = normal_key(part);
            return c;
        }
        if (part == "$mod")
            c.mods |= mod;
        else if (iequal(part, "Shift"))
            c.mods |= Mod::shift;
        else if (iequal(part, "Ctrl") || iequal(part, "Control"))
            c.mods |= Mod::ctrl;
        else if (iequal(part, "Mod1") || iequal(part, "Alt"))
            c.mods |= Mod::alt;
        else if (iequal(part, "Mod4") || iequal(part, "Super"))
            c.mods |= Mod::super;
        else
            return std::unexpected(ParseError{start, "unknown modifier '" + std::string(part) + "'"});
        start = plus + 1;
    }
}

std::expected<Binding, ParseError> parse_binding(std::string_view chord, std::string_view commands, Mod mod) {
    auto c = parse_chord(chord, mod);
    if (!c) return std::unexpected(c.error());
    auto s = parse(commands);
    if (!s) return std::unexpected(s.error());
    return Binding{std::move(*c), std::move(*s)};
}

const Script* lookup(const Bindings& bindings, const Chord& chord) {
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it)
        if (it->chord == chord) return &it->script;
    return nullptr;
}

Bindings default_bindings(Mod mod, std::string_view terminal) {
    Bindings out;
    auto bind = [&](std::string_view chord, std::string_view commands) {
        auto b = parse_binding(chord, commands, mod);
        expect(b.has_value(), "the default bindings parse");
        out.push_back(std::move(*b));
    };

    bind("$mod+Return", "exec " + std::string(terminal));
    bind("$mod+Shift+q", "kill");

    constexpr std::array<std::pair<std::string_view, std::string_view>, 8> ways = {{
        {"h", "left"}, {"j", "down"}, {"k", "up"}, {"l", "right"},
        {"Left", "left"}, {"Down", "down"}, {"Up", "up"}, {"Right", "right"},
    }};
    for (auto [key, dir] : ways) {
        bind("$mod+" + std::string(key), "focus " + std::string(dir));
        bind("$mod+Shift+" + std::string(key), "move " + std::string(dir));
    }

    bind("$mod+b", "split h");
    bind("$mod+v", "split v");
    bind("$mod+f", "fullscreen toggle");
    bind("$mod+s", "layout stacking");
    bind("$mod+w", "layout tabbed");
    bind("$mod+e", "layout toggle split");
    bind("$mod+Shift+space", "floating toggle");
    bind("$mod+space", "focus mode_toggle");
    bind("$mod+a", "focus parent");
    bind("$mod+Shift+a", "focus child");
    bind("$mod+Tab", "workspace back_and_forth");

    for (int n = 1; n <= 10; ++n) {
        const auto key = std::to_string(n % 10);
        const auto ws = std::to_string(n);
        bind("$mod+" + key, "workspace number " + ws);
        bind("$mod+Shift+" + key, "move container to workspace number " + ws);
    }
    return out;
}

}  // namespace i4::wm
