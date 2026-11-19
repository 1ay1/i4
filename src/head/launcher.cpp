// src/head/launcher.cpp — fuzzy ranking.
#include "launcher.hpp"

#include <algorithm>

namespace i4::head {

namespace {

char low(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool word_start(std::string_view t, std::size_t i) {
    if (i == 0) return true;
    const char p = t[i - 1];
    return p == ' ' || p == '-' || p == '_' || p == '.' || (p >= 'a' && p <= 'z' && t[i] >= 'A' && t[i] <= 'Z');
}

}  // namespace

std::optional<int> score(std::string_view q, std::string_view t) {
    if (q.empty()) return 0;
    int total = 0;
    std::size_t at = 0;
    std::optional<std::size_t> last;
    for (char qc : q) {
        if (qc == ' ') continue;  // spaces in a query only separate words
        const char want = low(qc);
        while (at < t.size() && low(t[at]) != want) ++at;
        if (at == t.size()) return std::nullopt;
        int s = 1;
        if (at == 0) s += 10;  // the very start
        else if (word_start(t, at)) s += 6;
        if (last && *last + 1 == at) s += 4;  // a contiguous run
        total += s;
        last = at++;
    }
    // Shorter names win a tie: "Files" before "Files (root)".
    return total * 64 - static_cast<int>(std::min<std::size_t>(t.size(), 63));
}

std::vector<std::size_t> rank(const std::vector<link::App>& apps, std::string_view query) {
    struct Hit {
        int score;
        std::size_t index;
    };
    std::vector<Hit> hits;
    for (std::size_t i = 0; i < apps.size(); ++i) {
        // The name, or failing that the comment ("browser" finds Firefox).
        auto s = score(query, apps[i].name);
        if (!s)
            if (auto c = score(query, apps[i].comment)) s = *c / 4;
        if (s) hits.push_back({*s, i});
    }
    if (!query.empty())
        std::ranges::stable_sort(hits, [](const Hit& a, const Hit& b) { return a.score > b.score; });
    std::vector<std::size_t> out;
    for (const auto& h : hits) out.push_back(h.index);
    return out;
}

std::string command(const link::App& app, std::string_view terminal) {
    if (!app.terminal) return app.exec;
    return std::string(terminal) + " -e " + app.exec;
}

}  // namespace i4::head
