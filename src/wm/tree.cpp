// src/wm/tree.cpp — tree surgery, and check(): every invariant the types
// cannot carry.
#include "tree.hpp"

#include <algorithm>
#include <climits>
#include <set>
#include <utility>

#include "i4/base/expect.hpp"
#include "i4/base/match.hpp"

namespace i4::wm {

namespace {

template <class T>
void erase_value(std::vector<T>& v, const T& x) {
    std::erase(v, x);
}

template <class T>
void move_to_front(std::vector<T>& v, const T& x) {
    auto it = std::ranges::find(v, x);
    expect(it != v.end(), "MRU entry present");
    std::rotate(v.begin(), it, it + 1);
}

/// Make `w` sum to exactly `whole`, every share positive, keeping the
/// proportions as closely as integers allow. The rounding remainder goes to
/// the largest share (the first, on a tie), which is where one part is least
/// visible and which every replica picks the same way.
void rebalance(std::vector<Share>& w) {
    if (w.empty()) return;
    std::uint64_t sum = 0;
    for (Share& x : w) {
        x = std::max<Share>(x, 1);
        sum += x;
    }
    std::uint64_t total = 0;
    for (Share& x : w) {
        x = std::max<Share>(1, static_cast<Share>(std::uint64_t{x} * whole / sum));
        total += x;
    }
    auto& big = *std::ranges::max_element(w);
    if (total < whole)
        big += static_cast<Share>(whole - total);
    else
        big -= static_cast<Share>(total - whole);
}

std::pair<int, std::string> order_key(const Workspace& w) {
    return {number_of(w.name).value_or(INT_MAX), w.name};
}

}  // namespace

std::optional<int> number_of(const std::string& name) {
    int n = 0;
    std::size_t i = 0;
    // Nine digits cannot overflow an int.
    while (i < name.size() && i < 9 && name[i] >= '0' && name[i] <= '9') n = n * 10 + (name[i++] - '0');
    if (i == 0) return std::nullopt;
    return n;
}

namespace tree {

// -- lookup ----------------------------------------------------------------

Con& con(State& s, ConId id) {
    auto it = s.cons.find(id);
    expect(it != s.cons.end(), "con exists");
    return it->second;
}

const Con& con(const State& s, ConId id) {
    auto it = s.cons.find(id);
    expect(it != s.cons.end(), "con exists");
    return it->second;
}

Split& split(State& s, ConId id) {
    auto* sp = std::get_if<Split>(&con(s, id).body);
    expect(sp != nullptr, "con is a split");
    return *sp;
}

const Split& split(const State& s, ConId id) {
    const auto* sp = std::get_if<Split>(&con(s, id).body);
    expect(sp != nullptr, "con is a split");
    return *sp;
}

Workspace& workspace(State& s, WorkspaceId id) {
    auto it = s.workspaces.find(id);
    expect(it != s.workspaces.end(), "workspace exists");
    return it->second;
}

const Workspace& workspace(const State& s, WorkspaceId id) {
    auto it = s.workspaces.find(id);
    expect(it != s.workspaces.end(), "workspace exists");
    return it->second;
}

Output& output(State& s, OutputId id) {
    auto it = s.outputs.find(id);
    expect(it != s.outputs.end(), "output exists");
    return it->second;
}

const Output& output(const State& s, OutputId id) {
    auto it = s.outputs.find(id);
    expect(it != s.outputs.end(), "output exists");
    return it->second;
}

bool is_split(const State& s, ConId id) { return is<Split>(con(s, id).body); }
bool is_root(const State& s, ConId id) { return is<TilingRoot>(con(s, id).place); }
bool is_floating(const State& s, ConId id) { return is<Floating>(con(s, id).place); }

std::optional<ConId> parent(const State& s, ConId id) {
    if (const auto* c = std::get_if<Child>(&con(s, id).place)) return c->parent;
    return std::nullopt;
}

ConId top(const State& s, ConId id) {
    while (auto p = parent(s, id)) id = *p;
    return id;
}

bool inside(const State& s, ConId id, ConId ancestor) {
    for (std::optional<ConId> c = id; c; c = parent(s, *c))
        if (*c == ancestor) return true;
    return false;
}

std::size_t index_in(const Split& sp, ConId child) {
    auto it = std::ranges::find(sp.children, child);
    expect(it != sp.children.end(), "child listed by its parent");
    return static_cast<std::size_t>(it - sp.children.begin());
}

std::optional<WorkspaceId> focused_workspace(const State& s) {
    if (!s.focus) return std::nullopt;
    return con(s, *s.focus).workspace;
}

std::optional<OutputId> focused_output(const State& s) {
    auto w = focused_workspace(s);
    if (!w) return std::nullopt;
    return workspace(s, *w).output;
}

ConId descend(const State& s, ConId id) {
    for (;;) {
        const auto* sp = std::get_if<Split>(&con(s, id).body);
        if (sp == nullptr || sp->mru.empty()) return id;
        id = sp->mru.front();
    }
}

ConId workspace_focus(const State& s, WorkspaceId ws) {
    const auto& w = workspace(s, ws);
    return descend(s, w.fullscreen ? *w.fullscreen : w.mru.front());
}

ConId tiling_focus(const State& s, WorkspaceId ws) { return descend(s, workspace(s, ws).tiling); }

std::vector<WindowId> windows_in(const State& s, ConId id) {
    std::vector<WindowId> out;
    auto walk = [&](auto&& self, ConId c) -> void {
        match(con(s, c).body, [&](const Leaf& l) { out.push_back(l.window); },
              [&](const Split& sp) {
                  for (ConId ch : sp.children) self(self, ch);
              });
    };
    walk(walk, id);
    return out;
}

bool is_empty(const State& s, WorkspaceId ws) {
    const auto& w = workspace(s, ws);
    return w.floating.empty() && split(s, w.tiling).children.empty();
}

// -- building ----------------------------------------------------------------

ConId create_con(State& s, WorkspaceId ws, Place place, Body body) {
    const auto id = fresh<ConId>(s);
    s.cons.emplace(id, Con{ws, std::move(place), std::move(body)});
    return id;
}

WorkspaceId create_workspace(State& s, OutputId out, std::string name) {
    const auto id = fresh<WorkspaceId>(s);
    const auto root = create_con(s, id, TilingRoot{}, Split{});
    Workspace w;
    w.name = std::move(name);
    w.output = out;
    w.tiling = root;
    w.mru = {root};
    s.workspaces.emplace(id, std::move(w));
    output(s, out).workspaces.push_back(id);
    sort_workspaces(s, out);
    return id;
}

void sort_workspaces(State& s, OutputId out) {
    std::ranges::sort(output(s, out).workspaces, [&](WorkspaceId a, WorkspaceId b) {
        return order_key(workspace(s, a)) < order_key(workspace(s, b));
    });
}

std::string first_free_number(const State& s) {
    std::set<int> used;
    for (const auto& [id, w] : s.workspaces)
        if (auto n = number_of(w.name)) used.insert(*n);
    int n = 1;
    while (used.contains(n)) ++n;
    return std::to_string(n);
}

namespace {

void set_workspace(State& s, ConId c, WorkspaceId ws) {
    auto& x = con(s, c);
    x.workspace = ws;
    if (const auto* sp = std::get_if<Split>(&x.body))
        for (ConId ch : sp->children) set_workspace(s, ch, ws);
}

}  // namespace

void insert(State& s, ConId parent_id, std::size_t index, ConId child) {
    set_workspace(s, child, con(s, parent_id).workspace);
    con(s, child).place = Child{parent_id};

    auto& sp = split(s, parent_id);
    expect(index <= sp.children.size(), "insert position in range");
    // An equal share for the newcomer; the others give it up in proportion.
    const auto share = static_cast<Share>(whole / (sp.children.size() + 1));
    for (Share& w : sp.weights) w = static_cast<Share>(std::uint64_t{w} * (whole - share) / whole);
    sp.children.insert(sp.children.begin() + static_cast<std::ptrdiff_t>(index), child);
    sp.weights.insert(sp.weights.begin() + static_cast<std::ptrdiff_t>(index), share);
    rebalance(sp.weights);
    sp.mru.push_back(child);
}

void float_in(State& s, WorkspaceId ws, ConId c, Rect<Desktop> rect) {
    expect(!is_split(s, c), "only windows float");
    set_workspace(s, c, ws);
    con(s, c).place = Floating{rect};
    auto& w = workspace(s, ws);
    w.floating.push_back(c);
    w.mru.push_back(c);
}

std::optional<ConId> unlink(State& s, ConId c) {
    auto& x = con(s, c);
    auto& w = workspace(s, x.workspace);
    if (w.fullscreen && inside(s, *w.fullscreen, c)) w.fullscreen.reset();

    return match(
        x.place,
        [&](const TilingRoot&) -> std::optional<ConId> {
            expect(false, "a tiling root is never unlinked");
            return std::nullopt;
        },
        [&](const Floating&) -> std::optional<ConId> {
            erase_value(w.floating, c);
            erase_value(w.mru, c);
            return std::nullopt;
        },
        [&](const Child& ch) -> std::optional<ConId> {
            auto& sp = split(s, ch.parent);
            const auto i = index_in(sp, c);
            sp.children.erase(sp.children.begin() + static_cast<std::ptrdiff_t>(i));
            sp.weights.erase(sp.weights.begin() + static_cast<std::ptrdiff_t>(i));
            erase_value(sp.mru, c);
            rebalance(sp.weights);
            return ch.parent;
        });
}

namespace {

/// `k` has just replaced a collapsed split inside `g`. If it is a split
/// along the same axis as `g`, it says nothing `g` does not: H[a H[b c]] is
/// H[a b c]. So its children move up into `g`, in its place, sharing its
/// weight.
void splice(State& s, ConId g, ConId k) {
    if (!is_split(s, k) || s.focus == k) return;
    const auto& w = workspace(s, con(s, k).workspace);
    if (w.fullscreen == k) return;

    const Split inner = split(s, k);  // a copy: k is about to go
    auto& gs = split(s, g);
    if (inner.layout != gs.layout || shows_one_child(inner.layout)) return;

    const auto i = index_in(gs, k);
    const Share share = gs.weights[i];
    gs.children.erase(gs.children.begin() + static_cast<std::ptrdiff_t>(i));
    gs.weights.erase(gs.weights.begin() + static_cast<std::ptrdiff_t>(i));
    gs.children.insert(gs.children.begin() + static_cast<std::ptrdiff_t>(i), inner.children.begin(),
                       inner.children.end());
    for (std::size_t j = 0; j < inner.weights.size(); ++j)
        gs.weights.insert(gs.weights.begin() + static_cast<std::ptrdiff_t>(i + j),
                          static_cast<Share>(std::uint64_t{inner.weights[j]} * share / whole));
    rebalance(gs.weights);

    auto at = std::ranges::find(gs.mru, k);
    const auto m = at - gs.mru.begin();
    gs.mru.erase(at);
    gs.mru.insert(gs.mru.begin() + m, inner.mru.begin(), inner.mru.end());

    for (ConId ch : inner.children) con(s, ch).place = Child{g};
    s.cons.erase(k);
}

/// `p` has one child left: the child takes p's place, weight and MRU slot.
void collapse(State& s, ConId p) {
    const ConId k = split(s, p).children.front();
    const auto g = parent(s, p);
    expect(g.has_value(), "a collapsing split has a parent");

    auto& gs = split(s, *g);
    gs.children[index_in(gs, p)] = k;
    *std::ranges::find(gs.mru, p) = k;
    con(s, k).place = Child{*g};

    auto& w = workspace(s, con(s, p).workspace);
    if (w.fullscreen == p) w.fullscreen = k;
    if (s.focus == p) s.focus = k;
    s.cons.erase(p);

    splice(s, *g, k);
}

}  // namespace

void normalise(State& s, ConId p) {
    if (!s.cons.contains(p) || is_root(s, p)) return;
    const auto& sp = split(s, p);
    if (sp.children.empty()) {
        const auto g = unlink(s, p);
        s.cons.erase(p);
        if (g) normalise(s, *g);
    } else if (sp.children.size() == 1) {
        collapse(s, p);
    }
}

void erase(State& s, ConId c) {
    match(con(s, c).body, [&](const Leaf& l) { s.windows.erase(l.window); },
          [&](const Split& sp) {
              for (ConId ch : std::vector(sp.children)) erase(s, ch);
          });
    s.cons.erase(c);
}

// -- focus --------------------------------------------------------------------

void raise(State& s, ConId c) {
    ConId cur = c;
    while (auto p = parent(s, cur)) {
        move_to_front(split(s, *p).mru, cur);
        cur = *p;
    }
    auto& w = workspace(s, con(s, cur).workspace);
    move_to_front(w.mru, cur);
    if (is_floating(s, cur)) {
        erase_value(w.floating, cur);
        w.floating.push_back(cur);
    }
}

void focus(State& s, ConId c) {
    const auto before = focused_workspace(s);
    std::optional<std::string> before_name;
    if (before) before_name = workspace(s, *before).name;

    raise(s, c);
    const auto ws = con(s, c).workspace;
    const auto out = workspace(s, ws).output;
    auto& o = output(s, out);
    const auto hidden = o.shown;
    o.shown = ws;
    move_to_front(s.output_mru, out);
    s.focus = c;

    if (before && *before != ws) s.previous_workspace = before_name;
    if (hidden != ws) gc(s, hidden);
    if (before && *before != ws) gc(s, *before);
}

void gc(State& s, WorkspaceId id) {
    auto it = s.workspaces.find(id);
    if (it == s.workspaces.end()) return;
    const auto& w = it->second;
    if (output(s, w.output).shown == id || !is_empty(s, id)) return;
    erase(s, w.tiling);
    erase_value(output(s, w.output).workspaces, id);
    s.workspaces.erase(it);
}

void refocus_if_lost(State& s, WorkspaceId ws) {
    if (!s.focus || s.cons.contains(*s.focus)) return;
    s.focus.reset();
    focus(s, workspace_focus(s, ws));
}

}  // namespace tree

// -- check ----------------------------------------------------------------------

namespace {

template <class T>
bool same_set(std::vector<T> a, std::vector<T> b) {
    std::ranges::sort(a);
    std::ranges::sort(b);
    return a == b;
}

}  // namespace

std::expected<void, std::string> check(const State& s) {
    using std::to_string;
#define I4_REQUIRE(cond, msg)                                     \
    do {                                                          \
        if (!(cond)) return std::unexpected(std::string(msg));    \
    } while (0)

    // -- outputs
    std::vector<OutputId> output_ids;
    for (const auto& [id, o] : s.outputs) output_ids.push_back(id);
    I4_REQUIRE(same_set(output_ids, s.output_mru), "output_mru is not a permutation of the outputs");
    I4_REQUIRE(s.outputs.empty() == !s.focus, "focus must exist exactly when an output does");
    I4_REQUIRE(s.unplaced.empty() || s.outputs.empty(), "windows wait unplaced while there is an output");
    if (s.outputs.empty())
        I4_REQUIRE(s.workspaces.empty() && s.cons.empty() && s.windows.empty(), "a tree with no output");

    for (const auto& [id, o] : s.outputs) {
        I4_REQUIRE(!o.workspaces.empty(), "output " + o.info.name + " has no workspace");
        I4_REQUIRE(std::ranges::count(o.workspaces, o.shown) == 1, "output " + o.info.name + " shows a workspace it does not list");
        for (WorkspaceId w : o.workspaces) {
            I4_REQUIRE(s.workspaces.contains(w), "output lists a missing workspace");
            I4_REQUIRE(s.workspaces.at(w).output == id, "workspace's output does not list it back");
        }
    }

    // -- workspaces, and the trees under them
    std::set<std::string> names;
    std::set<ConId> seen;
    std::size_t leaves = 0;

    for (const auto& [wid, w] : s.workspaces) {
        const auto ctx = "workspace " + w.name + ": ";
        I4_REQUIRE(names.insert(w.name).second, ctx + "name not unique");
        I4_REQUIRE(s.outputs.contains(w.output), ctx + "on a missing output");
        I4_REQUIRE(std::ranges::count(s.outputs.at(w.output).workspaces, wid) == 1, ctx + "not listed by its output");

        std::vector<ConId> tops = w.floating;
        tops.push_back(w.tiling);
        I4_REQUIRE(same_set(tops, w.mru), ctx + "mru is not a permutation of tiling + floating");
        const bool shown = s.outputs.at(w.output).shown == wid;

        // Walk every con below a top, checking each edge both ways.
        auto walk = [&](auto&& self, ConId c, bool root) -> std::expected<void, std::string> {
            I4_REQUIRE(s.cons.contains(c), ctx + "missing con " + to_string(c.value));
            I4_REQUIRE(seen.insert(c).second, ctx + "con " + to_string(c.value) + " reached twice");
            const auto& x = s.cons.at(c);
            const auto here = ctx + "con " + to_string(c.value) + ": ";
            I4_REQUIRE(x.workspace == wid, here + "wrong workspace");
            if (const auto* l = std::get_if<Leaf>(&x.body)) {
                I4_REQUIRE(s.windows.contains(l->window), here + "window missing");
                I4_REQUIRE(s.windows.at(l->window).con == c, here + "window points elsewhere");
                ++leaves;
                return {};
            }
            const auto& sp = std::get<Split>(x.body);
            I4_REQUIRE(root || !sp.children.empty(), here + "empty split survived");
            I4_REQUIRE(sp.weights.size() == sp.children.size(), here + "weights and children differ in length");
            I4_REQUIRE(same_set(sp.children, sp.mru), here + "mru is not a permutation of children");
            std::uint64_t sum = 0;
            for (Share v : sp.weights) {
                I4_REQUIRE(v > 0, here + "a child has no share");
                sum += v;
            }
            I4_REQUIRE(sp.children.empty() || sum == whole, here + "shares do not sum to whole");
            for (ConId ch : sp.children) {
                I4_REQUIRE(s.cons.contains(ch), here + "missing child");
                const auto* place = std::get_if<Child>(&s.cons.at(ch).place);
                I4_REQUIRE(place && place->parent == c, here + "child does not point back");
                if (auto r = self(self, ch, false); !r) return r;
            }
            return {};
        };

        I4_REQUIRE(s.cons.contains(w.tiling), ctx + "missing tiling root");
        I4_REQUIRE(is<TilingRoot>(s.cons.at(w.tiling).place), ctx + "tiling root is not placed as one");
        I4_REQUIRE(is<Split>(s.cons.at(w.tiling).body), ctx + "tiling root is not a split");
        if (auto r = walk(walk, w.tiling, true); !r) return r;
        for (ConId f : w.floating) {
            I4_REQUIRE(s.cons.contains(f), ctx + "missing floating con");
            I4_REQUIRE(is<Floating>(s.cons.at(f).place), ctx + "floating con not placed as floating");
            I4_REQUIRE(is<Leaf>(s.cons.at(f).body), ctx + "floating con is not a window");
            if (auto r = walk(walk, f, false); !r) return r;
        }
        if (w.fullscreen) {
            I4_REQUIRE(s.cons.contains(*w.fullscreen) && s.cons.at(*w.fullscreen).workspace == wid,
                       ctx + "fullscreen con is not in this workspace");
            I4_REQUIRE(*w.fullscreen != w.tiling, ctx + "the tiling root is fullscreen");
        }
        I4_REQUIRE(shown || !w.floating.empty() || !std::get<Split>(s.cons.at(w.tiling).body).children.empty(),
                   ctx + "empty and hidden, so it should have been collected");
    }
    I4_REQUIRE(seen.size() == s.cons.size(), "a con is not reachable from any workspace");
    I4_REQUIRE(leaves == s.windows.size(), "a window has no con");

    // -- focus agrees with every MRU list on the way up
    if (s.focus) {
        I4_REQUIRE(s.cons.contains(*s.focus), "focus on a missing con");
        const auto& fx = s.cons.at(*s.focus);
        const auto& w = s.workspaces.at(fx.workspace);
        I4_REQUIRE(s.outputs.at(w.output).shown == fx.workspace, "focus on a hidden workspace");
        I4_REQUIRE(s.output_mru.front() == w.output, "focused output is not first in output_mru");
        ConId cur = *s.focus;
        for (;;) {
            const auto* ch = std::get_if<Child>(&s.cons.at(cur).place);
            if (!ch) break;
            I4_REQUIRE(std::get<Split>(s.cons.at(ch->parent).body).mru.front() == cur,
                       "focus path disagrees with an MRU list");
            cur = ch->parent;
        }
        I4_REQUIRE(w.mru.front() == cur, "focus path disagrees with the workspace MRU");
        if (w.fullscreen) {
            ConId c = *s.focus;
            bool in = false;
            for (std::optional<ConId> p = c; p; p = tree::parent(s, *p))
                if (*p == *w.fullscreen) in = true;
            I4_REQUIRE(in, "focus escaped a fullscreen con");
        }
    }

    std::uint32_t highest = 0;
    for (const auto& [id, o] : s.outputs) highest = std::max(highest, id.value);
    for (const auto& [id, w] : s.workspaces) highest = std::max(highest, id.value);
    for (const auto& [id, c] : s.cons) highest = std::max(highest, id.value);
    I4_REQUIRE(highest < s.next_id, "an id at or above next_id");

#undef I4_REQUIRE
    return {};
}

}  // namespace i4::wm
