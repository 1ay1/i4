#pragma once
// src/head/keys.hpp — a mayag key press, as an i3 chord.
//
// mayag reports physical keys (Key::h, Key::n1, Key::enter) on every
// platform it runs on: evdev on Linux, kVK on macOS, scancodes on Windows,
// KeyboardEvent.code in a browser. This maps them to the keysym names the
// wm layer's bindings use, once, so a binding written in a config means the
// same key on every head.
//
// The switch is exhaustive on purpose: a key mayag learns to report is a
// compile error here until it is named (or deliberately left unbound).

#include <mayag/mayag.hpp>

#include <optional>
#include <string_view>

#include "i4/wm/bindings.hpp"

namespace i4::head {

/// i3's name for a physical key; nullopt for keys that are only modifiers
/// or that i3 has no name for.
[[nodiscard]] constexpr std::optional<std::string_view> keysym(mayag::Key k) noexcept {
    using K = mayag::Key;
    switch (k) {
        case K::unknown: return std::nullopt;
        case K::enter: return "Return";
        case K::tab: return "Tab";
        case K::backspace: return "BackSpace";
        case K::del: return "Delete";
        case K::insert: return "Insert";
        case K::escape: return "Escape";
        case K::space: return "space";
        case K::left: return "Left";
        case K::right: return "Right";
        case K::up: return "Up";
        case K::down: return "Down";
        case K::home: return "Home";
        case K::end: return "End";
        case K::page_up: return "Prior";
        case K::page_down: return "Next";
        case K::shift:
        case K::ctrl:
        case K::alt:
        case K::super:
        case K::caps_lock: return std::nullopt;
        case K::f1: return "F1";
        case K::f2: return "F2";
        case K::f3: return "F3";
        case K::f4: return "F4";
        case K::f5: return "F5";
        case K::f6: return "F6";
        case K::f7: return "F7";
        case K::f8: return "F8";
        case K::f9: return "F9";
        case K::f10: return "F10";
        case K::f11: return "F11";
        case K::f12: return "F12";
        case K::a: return "a";
        case K::b: return "b";
        case K::c: return "c";
        case K::d: return "d";
        case K::e: return "e";
        case K::f: return "f";
        case K::g: return "g";
        case K::h: return "h";
        case K::i: return "i";
        case K::j: return "j";
        case K::k: return "k";
        case K::l: return "l";
        case K::m: return "m";
        case K::n: return "n";
        case K::o: return "o";
        case K::p: return "p";
        case K::q: return "q";
        case K::r: return "r";
        case K::s: return "s";
        case K::t: return "t";
        case K::u: return "u";
        case K::v: return "v";
        case K::w: return "w";
        case K::x: return "x";
        case K::y: return "y";
        case K::z: return "z";
        case K::n0: return "0";
        case K::n1: return "1";
        case K::n2: return "2";
        case K::n3: return "3";
        case K::n4: return "4";
        case K::n5: return "5";
        case K::n6: return "6";
        case K::n7: return "7";
        case K::n8: return "8";
        case K::n9: return "9";
        case K::minus: return "minus";
        case K::equal: return "equal";
        case K::bracket_left: return "bracketleft";
        case K::bracket_right: return "bracketright";
        case K::backslash: return "backslash";
        case K::semicolon: return "semicolon";
        case K::quote: return "apostrophe";
        case K::comma: return "comma";
        case K::period: return "period";
        case K::slash: return "slash";
        case K::grave: return "grave";
    }
    return std::nullopt;
}

[[nodiscard]] constexpr wm::Mods mods(mayag::Mods m) noexcept {
    wm::Mods out;
    if (m.shift) out |= wm::Mod::shift;
    if (m.ctrl) out |= wm::Mod::ctrl;
    if (m.alt) out |= wm::Mod::alt;
    if (m.super) out |= wm::Mod::super;
    return out;
}

[[nodiscard]] inline std::optional<wm::Chord> chord(const mayag::KeyEvent& k) {
    const auto name = keysym(k.key);
    if (!name) return std::nullopt;
    return wm::Chord{mods(k.mods), std::string(*name)};
}

}  // namespace i4::head
