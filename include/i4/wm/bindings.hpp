#pragma once
// i4/wm/bindings.hpp — what keys do, as data.
//
//   bindsym $mod+Shift+q kill
//     -> Binding{ Chord{super|shift, "q"}, Script{cmd::Kill{}} }
//
// A Chord names a key by i3's keysym name ("q", "Return", "1", "Left"),
// not by a platform keycode, so this layer stays portable: each head maps
// its own platform's keys to these names (head/keys.hpp for mayag), and a
// head on macOS and one on Linux run the same table.
//
// The table is plain data, which is the point: the config file (vibe, next)
// produces it, the defaults are it, a head predicting a keypress looks it up,
// and "what does $mod+Shift+h do" is a test with an answer.

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "i4/base/flags.hpp"
#include "i4/wm/command.hpp"

namespace i4::wm {

enum class Mod : std::uint8_t { shift, ctrl, alt, super };
using Mods = Flags<Mod>;

struct Chord {
    Mods mods;
    /// i3's keysym name, normalised by normal_key(): letters lower case,
    /// everything else as i3 spells it ("Return", "Left", "space").
    std::string key;
    friend bool operator==(const Chord&, const Chord&) = default;
};

struct Binding {
    Chord chord;
    Script script;
    friend bool operator==(const Binding&, const Binding&) = default;
};

using Bindings = std::vector<Binding>;

/// Lower-case a letter key; keep every other name as i3 spells it, but
/// accept the common lower-case spellings ("return" -> "Return").
[[nodiscard]] std::string normal_key(std::string_view key);

/// "$mod+Shift+h", with $mod meaning `mod`.
[[nodiscard]] std::expected<Chord, ParseError> parse_chord(std::string_view text, Mod mod);

/// "$mod+Shift+q" and "kill", together.
[[nodiscard]] std::expected<Binding, ParseError> parse_binding(std::string_view chord, std::string_view commands,
                                                               Mod mod);

/// The last binding for this chord wins, as when a config overrides a
/// default.
[[nodiscard]] const Script* lookup(const Bindings& bindings, const Chord& chord);

/// i3's default config (i3-config-wizard's, with $mod), with `terminal` for
/// $mod+Return. Arrow keys and the vim keys h/j/k/l both focus and move;
/// split is $mod+b (horizontal) and $mod+v (vertical) so h stays focus left.
[[nodiscard]] Bindings default_bindings(Mod mod, std::string_view terminal);

}  // namespace i4::wm
