#pragma once
// src/head/launcher.hpp — dmenu, inside the head: $mod+d.
//
// The list is the SESSION's apps (down::Apps), because that is where they
// run; a head on a laptop attached to a desktop lists the desktop's apps.
// Picking one sends `exec` as a predicted Request, the same path a key
// binding takes, so the window opens where i3 would put it.
//
// State and ranking are pure values (tests/test_launcher.cpp). The head
// program owns one Launcher and routes keys to it while it is open.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "i4/link/protocol.hpp"

namespace i4::head {

/// How well `query` matches `text`: nullopt when it does not (its letters
/// are not a subsequence of the text, case-insensitively). Higher is better:
/// a prefix beats a word start beats anywhere, and contiguous runs beat
/// scattered letters, so "fi" ranks Firefox above "Profile Manager".
[[nodiscard]] std::optional<int> score(std::string_view query, std::string_view text);

/// Indices into `apps` that match, best first; ties keep the list's order
/// (alphabetical, from the session). An empty query matches everything.
[[nodiscard]] std::vector<std::size_t> rank(const std::vector<link::App>& apps, std::string_view query);

struct Launcher {
    bool open = false;
    std::string query;
    std::size_t selected = 0;  // into the ranked list
    std::vector<std::size_t> shown;

    void show(const std::vector<link::App>& apps) {
        open = true;
        query.clear();
        selected = 0;
        shown = rank(apps, query);
    }
    void hide() { open = false; }
    void type(const std::vector<link::App>& apps, std::string_view text) {
        query += text;
        refilter(apps);
    }
    void erase(const std::vector<link::App>& apps) {
        // One UTF-8 code point.
        while (!query.empty() && (static_cast<unsigned char>(query.back()) & 0xC0) == 0x80) query.pop_back();
        if (!query.empty()) query.pop_back();
        refilter(apps);
    }
    void move(int by) {
        if (shown.empty()) return;
        const auto n = static_cast<long>(shown.size());
        selected = static_cast<std::size_t>(((static_cast<long>(selected) + by) % n + n) % n);
    }
    void refilter(const std::vector<link::App>& apps) {
        shown = rank(apps, query);
        selected = 0;
    }
    /// The app to launch: the selection, if there is one.
    [[nodiscard]] std::optional<std::size_t> choice() const {
        if (shown.empty()) return std::nullopt;
        return shown[selected];
    }
};

/// The shell command that launches `app`, wrapping terminal apps.
[[nodiscard]] std::string command(const link::App& app, std::string_view terminal);

}  // namespace i4::head
