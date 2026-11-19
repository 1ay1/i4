// src/app/head.cpp — i4-head: attach a mayag window to a session.
//
//   i4-head [--link PATH] [--super] [--terminal CMD]
//
// $mod+d opens the launcher: the session's apps, fuzzy-searched.
//
// $mod is Alt by default, so a head running inside another compositor does
// not fight its Super bindings; --super for a head that owns the screen.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../head/head.hpp"
#include "../head/host.hpp"

#include <unistd.h>

namespace {

/// $mod+Return's terminal, and what terminal apps from the launcher run in:
/// $TERMINAL, else the first of the usual ones on $PATH. (Both run on the
/// session's machine; a remote head will ask the session instead.)
std::string find_terminal() {
    if (const char* t = std::getenv("TERMINAL"); t && *t) return t;
    const std::string dirs = std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin";
    for (const char* name : {"foot", "kitty", "alacritty", "wezterm", "ghostty", "konsole", "gnome-terminal"})
        for (std::size_t at = 0; at <= dirs.size();) {
            const auto c = dirs.find(':', at);
            const auto dir = dirs.substr(at, c == std::string::npos ? std::string::npos : c - at);
            if (::access((dir + "/" + name).c_str(), X_OK) == 0) return name;
            at = c == std::string::npos ? dirs.size() + 1 : c + 1;
        }
    return "foot";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace i4;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string path = std::string(runtime ? runtime : "/tmp") + "/i4-link";
    head::options.terminal = find_terminal();
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--super") head::options.mod = wm::Mod::super;
        else if (a == "--link" && i + 1 < argc) path = argv[++i];
        else if (a == "--terminal" && i + 1 < argc) head::options.terminal = argv[++i];
    }

    auto link = transport::Channel::connect(path);
    if (!link) {
        std::fprintf(stderr, "i4-head: no session at %s (start i4d first)\n", path.c_str());
        return 1;
    }

    mayag::AppConfig cfg;
    cfg.title = "i4";
    cfg.size = {1280, 800};
    auto opened = mayag::platform::Native::open(mayag::detail::window_config(cfg));
    if (!opened) {
        std::fprintf(stderr, "i4-head: could not open a window\n");
        return 1;
    }
    auto window = std::move(*opened);
    if (cfg.fonts == nullptr) cfg.fonts = mayag::typo::system::default_stack();

    jaal::run_options opt;
    opt.kernel = mayag::detail::kernel_options(cfg);
    head::Host<head::Head, mayag::platform::Native> host{window, std::move(cfg), std::move(*link)};
    // I4_NO_DMABUF=1 forces the shm path (for comparison).
    head::options.zero_copy = std::getenv("I4_NO_DMABUF") == nullptr;
    return jaal::run<head::Head>(host, std::move(opt));
}
