// loom/server/drive.hpp — step a Display by hand, for TESTS.
//
// loom has no event loop: whoever runs the program waits, and tells the
// Display what is ready (display.hpp). A program gets that from jaal
// (loom/jaal.hpp). A test that needs to say "now let the server handle what
// the client just sent" gets it from this.
//
// It is for tests. A program that steps a Display with this is running a
// second loop beside the runtime, which is the thing loom is built to rule
// out; tapestry's build refuses it outside tests/.
//
// But the WAITING here is still jaal's. This used to call ::poll directly —
// a second, hand-written implementation of the one thing the runtime exists
// to do, with its own idea of what a hangup is and its own timeout clamp.
// Now it drives jaal's reactor, the same epoll the real loop uses, so a test
// that passes here is exercising the readiness path a program actually gets
// rather than a lookalike that can drift from it.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include <jaal/platform/concepts.hpp>
#include <jaal/platform/select.hpp>

#include "i4/wl/server/display.hpp"

namespace i4::wl::server::test {

class Drive {
public:
    using reactor = ::jaal::platform::native_reactor;

    explicit Drive(Display& d) : display_(d) {
        if (auto r = reactor::create()) reactor_.emplace(std::move(*r));
        display_.on_watch([this](Display::Watch w) { watch(w.key, w.fd); });
        display_.on_unwatch([this](std::uint64_t key) { regs_.erase(key); });
    }

    /// Wait up to `ms` for anything watched, and hand each ready key to the
    /// display. Returns how many were ready, 0 on timeout, -1 if nothing is
    /// watched at all (the display has closed everything).
    int step(int ms) {
        if (!reactor_ || regs_.empty()) return -1;

        auto res = reactor_->wait(std::chrono::milliseconds{ms});
        if (!res) return -1;
        if (res->timeout || res->count == 0) return 0;

        for (std::uint8_t i = 0; i < res->count; ++i) {
            const auto& r = res->ready[i];
            // The same translation loom/jaal.hpp does for a real program:
            // readable, plus hangup and error as their own facts rather than
            // as a failed read discovered later.
            Ready what = Ready::readable;
            if (r.hangup) what = what | Ready::hangup;
            if (r.error) what = what | Ready::error;
            display_.ready(r.token, what);
        }
        display_.flush_clients();
        return res->count;
    }

    std::size_t watched() const { return regs_.size(); }

private:
    void watch(std::uint64_t key, int fd) {
        if (!reactor_) return;
        // The Display's key IS the token: no offset to get wrong, because a
        // test drives exactly one Display and nothing else.
        if (auto r = reactor_->watch(fd, ::jaal::platform::interest::read, key))
            regs_.insert_or_assign(key, std::move(*r));
    }

    Display& display_;
    std::optional<reactor> reactor_;
    std::map<std::uint64_t, reactor::registration> regs_;
};

}  // namespace i4::wl::server::test
