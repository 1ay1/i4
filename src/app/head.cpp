// src/app/head.cpp — i4-head: attach a mayag window to a session.
//
//   i4-head [--link PATH | --link tcp:HOST:PORT] [--super] [--terminal CMD]
//
// --link tcp:HOST:PORT attaches to a session on another machine (i4d
// --remote tcp::PORT); frames arrive as compressed changed tiles.
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
    std::string key_arg;
    // Present each frame the moment it is drawn (no wait for vblank; the
    // host compositor may tear it in): the lowest latency there is. On by
    // default; --vsync or I4_VSYNC=1 for tear-free frames.
    bool vsync = false;
    // Fullscreen + no vsync: the host compositor scans the head's buffer out
    // directly and may flip it mid-frame (Hyprland: an `immediate` window
    // rule); its compositing pass and a whole refresh drop out of the path.
    bool fullscreen = std::getenv("I4_FULLSCREEN") != nullptr;
    if (const char* v = std::getenv("I4_VSYNC"); v && *v) vsync = *v != '0';
    head::options.terminal = find_terminal();
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--super") head::options.mod = wm::Mod::super;
        else if (a == "--link" && i + 1 < argc) path = argv[++i];
        else if (a == "--key" && i + 1 < argc) key_arg = argv[++i];
        else if (a == "--terminal" && i + 1 < argc) head::options.terminal = argv[++i];
        else if (a == "--vsync") vsync = true;
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--immediate") vsync = false;
    }

    // Across a network the link is encrypted, with the session's pairing
    // key: --key HEX, or --key FILE, or $I4_KEY, or ~/.config/i4/remote.key.
    std::optional<transport::Key> key;
    if (transport::Channel::is_tcp(path)) {
        if (key_arg.empty())
            if (const char* e = std::getenv("I4_KEY"); e && *e) key_arg = e;
        if (!key_arg.empty()) key = transport::parse_key(key_arg);
        if (!key) key = transport::load_key(key_arg.empty() ? transport::default_key_path() : key_arg, false);
        if (!key) {
            std::fprintf(stderr,
                         "i4-head: a network session needs its pairing key: --key HEX (i4d prints it when started "
                         "with --remote), or put it in %s\n",
                         transport::default_key_path().c_str());
            return 1;
        }
    }
    auto link = transport::Channel::connect(path, key ? &*key : nullptr);
    if (!link) {
        if (key && link.error() == transport::ChannelError::io)
            std::fprintf(stderr, "i4-head: the session at %s refused this key (wrong pairing key?)\n", path.c_str());
        else
            std::fprintf(stderr, "i4-head: no session at %s (start i4d first)\n", path.c_str());
        return 1;
    }

    mayag::AppConfig cfg;
    cfg.title = "i4";
    cfg.size = {1280, 800};
    cfg.vsync = vsync;
    auto opened = mayag::platform::Native::open(mayag::detail::window_config(cfg));
    if (!opened) {
        std::fprintf(stderr, "i4-head: could not open a window\n");
        return 1;
    }
    auto window = std::move(*opened);
    if (fullscreen) {
        // Through a template, so the check is on the type (not every
        // platform's window can go fullscreen).
        const bool ok = [](auto& w) {
            if constexpr (requires { w.set_fullscreen(true); }) {
                w.set_fullscreen(true);
                return true;
            } else {
                return false;
            }
        }(window);
        if (!ok) std::fprintf(stderr, "i4-head: this platform's window cannot go fullscreen yet\n");
    }
    if (cfg.fonts == nullptr) cfg.fonts = mayag::typo::system::default_stack();

    jaal::run_options opt;
    opt.kernel = mayag::detail::kernel_options(cfg);
    head::Host<head::Head, mayag::platform::Native> host{window, std::move(cfg), std::move(*link)};
    // Across a network, frames come by datagram (UDP, the same port): a lost
    // packet never stalls the next frame. I4_TCP_ONLY=1 keeps them on TCP.
    if (key && std::getenv("I4_TCP_ONLY") == nullptr)
        if (auto u = transport::DatagramSocket::connect(path)) host.use_datagrams(std::move(*u));
    if (transport::Channel::is_tcp(path)) remote::warm_up();
    // I4_NO_DMABUF=1 forces the shm path (for comparison). Across a network
    // nothing can be imported: clients render to shm, i4d sends tiles.
    head::options.zero_copy = std::getenv("I4_NO_DMABUF") == nullptr && !transport::Channel::is_tcp(path);
    head::options.remote = transport::Channel::is_tcp(path);
    return jaal::run<head::Head>(host, std::move(opt));
}
