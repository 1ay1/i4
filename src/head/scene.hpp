#pragma once
// src/head/scene.hpp — what a head draws, between the replica and pixels.
//
//   replica.shown() --arrange--> Arrangement --Motion--> Scene --view--> Node
//
// The Arrangement says where everything IS. Motion makes everything GET
// there: each tile chases its target rectangle on a spring (mayag's
// Animated, fixed-step, so a head at 60 Hz and one at 144 Hz move alike),
// which is what turns a focus change predicted in 0 ms into motion rather
// than a jump. The springs live only here: wm::State never sees a float.
//
// The draw is a pure function of (Scene, Hud): no state of its own, so the
// same frame renders in the window, in a PNG and in a headless test.

#include <mayag/mayag.hpp>

#include <map>
#include <string>
#include <vector>

#include "i4/wm/arrange.hpp"

namespace i4::head {

/// A tile's rectangle mid-flight: x, y, width, height in desktop pixels.
using Moving = mayag::Animated<mayag::Vec4>;

class Motion {
public:
    /// New targets. A window seen for the first time grows out of its own
    /// centre; one that is gone is dropped.
    void retarget(const wm::Arrangement& a);

    /// Advance every spring by `dt` seconds.
    void step(double dt);

    [[nodiscard]] bool animating() const;

    /// Where a window is now (its frame), if it is shown.
    [[nodiscard]] std::optional<mayag::Vec4> at(wm::WindowId w) const;

private:
    std::map<wm::WindowId, Moving> frames_;
};

/// The head's own status: what is not window management, shown in the bar.
struct Hud {
    std::vector<std::string> workspaces;  // on the focused output, in order
    std::string focused_workspace;
    int latency_ms = 0;
    std::size_t in_flight = 0;    // requests the server has not answered
    std::uint64_t corrections = 0;
    std::string mod_name;         // "Alt", "Super"
};

inline constexpr float bar_height = 28.0f;

/// The whole frame: the bar on top, the output's tiles and titles under it.
[[nodiscard]] mayag::Node view(const wm::Arrangement& a, const Motion& motion, const Hud& hud);

}  // namespace i4::head
