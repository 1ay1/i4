// src/server/apps.cpp — .desktop files.
#include "apps.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace i4::server {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool truthy(std::string_view v) { return v == "true" || v == "1"; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

}  // namespace

std::string strip_field_codes(std::string_view exec) {
    std::string out;
    for (std::size_t i = 0; i < exec.size(); ++i) {
        if (exec[i] != '%' || i + 1 >= exec.size()) {
            out += exec[i];
            continue;
        }
        const char code = exec[++i];
        if (code == '%') out += '%';
        // Everything else (%f %F %u %U %i %c %k and the deprecated ones)
        // expands to nothing when nothing is being opened.
    }
    // Field codes leave doubled or trailing spaces behind.
    std::string tidy;
    for (char c : out)
        if (!(c == ' ' && (tidy.empty() || tidy.back() == ' '))) tidy += c;
    while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
    return tidy;
}

std::optional<link::App> parse_desktop(std::string_view id, std::string_view text) {
    std::map<std::string, std::string, std::less<>> keys;
    bool in_entry = false;
    std::size_t at = 0;
    while (at <= text.size()) {
        const auto nl = text.find('\n', at);
        const auto line = trim(text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
        at = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[') {
            in_entry = line == "[Desktop Entry]";
            continue;
        }
        if (!in_entry) continue;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        // Localised keys (Name[de]) are skipped: the unlocalised value is
        // what every entry is required to have.
        const auto key = trim(line.substr(0, eq));
        if (key.find('[') != std::string_view::npos) continue;
        keys.emplace(std::string(key), std::string(trim(line.substr(eq + 1))));
    }

    auto get = [&](std::string_view k) -> std::string {
        auto it = keys.find(k);
        return it == keys.end() ? std::string{} : it->second;
    };
    if (get("Type") != "Application") return std::nullopt;
    if (truthy(get("NoDisplay")) || truthy(get("Hidden"))) return std::nullopt;
    // Meant for some other desktop only (GNOME's settings panels, KDE's):
    // i4 is none of them.
    if (!get("OnlyShowIn").empty()) return std::nullopt;
    const auto exec = strip_field_codes(get("Exec"));
    const auto name = get("Name");
    if (exec.empty() || name.empty()) return std::nullopt;

    link::App app;
    app.id = std::string(id);
    app.name = name;
    app.comment = get("Comment").empty() ? get("GenericName") : get("Comment");
    app.exec = exec;
    app.terminal = truthy(get("Terminal"));
    return app;
}

std::vector<link::App> scan_apps() {
    namespace fs = std::filesystem;
    std::vector<fs::path> roots;
    const char* home = std::getenv("HOME");
    if (const char* d = std::getenv("XDG_DATA_HOME"); d && *d)
        roots.emplace_back(d);
    else if (home)
        roots.emplace_back(fs::path(home) / ".local/share");
    std::string dirs = std::getenv("XDG_DATA_DIRS") ? std::getenv("XDG_DATA_DIRS") : "";
    if (dirs.empty()) dirs = "/usr/local/share:/usr/share";
    for (std::size_t at = 0; at <= dirs.size();) {
        const auto c = dirs.find(':', at);
        const auto part = dirs.substr(at, c == std::string::npos ? std::string::npos : c - at);
        if (!part.empty()) roots.emplace_back(part);
        at = c == std::string::npos ? dirs.size() + 1 : c + 1;
    }

    std::map<std::string, link::App> by_id;
    std::map<std::string, bool> seen;  // an id claimed, even by an entry that is hidden
    for (const auto& root : roots) {
        const auto dir = root / "applications";
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec) || it->path().extension() != ".desktop") continue;
            // The desktop-file id: the path under applications/, / -> -.
            auto id = fs::relative(it->path(), dir, ec).replace_extension().string();
            std::ranges::replace(id, '/', '-');
            if (seen.contains(id)) continue;  // a higher-priority dir already said what this id is
            seen[id] = true;
            std::ifstream in(it->path());
            std::stringstream text;
            text << in.rdbuf();
            if (auto app = parse_desktop(id, text.str())) by_id.emplace(id, std::move(*app));
        }
    }
    std::vector<link::App> out;
    for (auto& [id, app] : by_id) out.push_back(std::move(app));
    std::ranges::sort(out, [](const auto& a, const auto& b) { return lower(a.name) < lower(b.name); });
    return out;
}

}  // namespace i4::server
