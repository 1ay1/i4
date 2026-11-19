#pragma once
// src/server/apps.hpp — what can be launched: the session machine's
// .desktop files (freedesktop Desktop Entry spec).
//
// parse() is pure (text in, App out) and tested; scan() is the I/O and is
// called only by the host, which hands the result to the Session as an
// event like anything else it learns from the outside world.

#include <optional>
#include <string_view>
#include <vector>

#include "i4/link/protocol.hpp"

namespace i4::server {

/// One .desktop file. nullopt when it is not something to launch: not an
/// Application, hidden, NoDisplay, only for other desktops, no Exec.
[[nodiscard]] std::optional<link::App> parse_desktop(std::string_view id, std::string_view text);

/// Exec with its field codes (%f %U ...) removed and %% made %: a launcher
/// opens an app with no files, so the codes expand to nothing.
[[nodiscard]] std::string strip_field_codes(std::string_view exec);

/// $XDG_DATA_HOME, then $XDG_DATA_DIRS, each /applications, recursively.
/// The first file for a desktop id wins (a user's override beats the
/// system's), and the result is sorted by name.
[[nodiscard]] std::vector<link::App> scan_apps();

}  // namespace i4::server
