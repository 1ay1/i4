// tests/test_determinism.cpp — random sessions, every invariant, every step.
//
// Thousands of random events (windows coming and going, outputs plugged and
// unplugged, every command) from fixed seeds. After every single step the
// tree must pass check() and arrange() must produce sane geometry. At the
// end, replaying the same events into a fresh State must give an identical
// State: that is the property a head relies on to predict.
//
// std::mt19937's sequence is fixed by the standard, so a failing seed fails
// the same way on every machine. I4_FUZZ_SEEDS=1000 for a deep run.
#include <cstdlib>

#include "check.hpp"
#include "session.hpp"

using namespace i4;
using namespace i4::wm;
using i4::test::Generator;

namespace {

bool sane(const Arrangement& a, std::size_t step) {
    for (const auto& sc : a.screens)
        for (const auto& t : sc.tiles) {
            if (t.frame.size.width < 0 || t.frame.size.height < 0 || t.content.size.width < 0 ||
                t.content.size.height < 0) {
                i4::test::fail("negative size at step " + std::to_string(step));
                return false;
            }
            if (t.layer == Layer::tiling && !t.frame.empty() && t.frame.intersected(sc.rect) != t.frame) {
                i4::test::fail("tiled window outside its output at step " + std::to_string(step));
                return false;
            }
        }
    return true;
}

}  // namespace

TEST(random_sessions_keep_every_invariant_and_replay_exactly) {
    const unsigned seeds = i4::test::seeds_from_env(12);
    for (unsigned seed = 1; seed <= seeds; ++seed) {
        Generator gen(seed);
        State state;
        std::vector<Event> log{ev::OutputAdded{OutputInfo{"A", {0, 0, 1000, 800}}}};
        std::vector<std::string> notes{"output added A"};
        (void)apply(state, log.front());

        auto push = [&](Event e, std::string note) -> bool {
            log.push_back(e);
            notes.push_back(std::move(note));
            const auto out = apply(state, e);
            if (auto c = check(state); !c) {
                i4::test::fail("seed " + std::to_string(seed) + " step " + std::to_string(log.size()) + ": " +
                               c.error() + "\n       shape: " + shape(state));
                for (std::size_t i = log.size() > 6 ? log.size() - 6 : 0; i < log.size(); ++i)
                    std::printf("       event %zu: %s\n", i + 1, notes[i].c_str());
                return false;
            }
            if (!sane(arrange(state), log.size())) return false;
            // The server's half of a kill: the window goes.
            for (const auto& fx : out.effects)
                if (const auto* close = std::get_if<fx::Close>(&fx)) {
                    log.push_back(ev::WindowUnmapped{close->window});
                    notes.push_back("(closed)");
                    (void)apply(state, log.back());
                }
            return true;
        };

        bool ok = true;
        for (int i = 0; i < 2000 && ok; ++i) {
            auto [e, note] = gen.next(state);
            ok = push(std::move(e), std::move(note));
        }
        if (!ok) return;

        State replay;
        for (const auto& e : log) (void)apply(replay, e);
        if (!(replay == state)) {
            i4::test::fail("seed " + std::to_string(seed) + ": replay diverged");
            return;
        }
    }
}

TEST(every_command_parses) {
    for (auto c : i4::test::commands)
        if (!parse(c)) i4::test::fail("does not parse: " + std::string(c));
}
