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
#include "launcher.hpp"

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
    /// Simulated link delay; 0 = local, negative = a real link (no delay to
    /// show, only what is in flight).
    int latency_ms = 0;
    std::size_t in_flight = 0;    // requests the server has not answered
    std::uint64_t corrections = 0;
    std::string mod_name;         // "Alt", "Super"
};

inline constexpr float bar_height = 28.0f;

/// A window's latest pixels, as a mayag texture id.
using Textures = std::map<wm::WindowId, std::uint32_t>;

/// The whole frame: the bar on top, the output's tiles and titles under it.
/// A window with a texture shows its pixels; one without, its name.
[[nodiscard]] mayag::Node view(const wm::Arrangement& a, const Motion& motion, const Hud& hud,
                               const Textures& textures = {});

/// The node id of the launcher's i-th row, for click hit-testing.
[[nodiscard]] std::uint64_t launcher_row(std::size_t i);

/// The launcher panel, centred near the top of a `screen`-sized window.
[[nodiscard]] mayag::Node launcher_view(const Launcher& l, const std::vector<link::App>& apps,
                                        Size<space::Desktop> screen);

/// A session notice, as a toast along the bottom of the screen.
[[nodiscard]] mayag::Node notice_view(const std::string& text, Size<space::Desktop> screen);

}  // namespace i4::head
