// src/app/i4d.cpp — the session server: Session (the program) on Host (the shell).
//
//   i4d [--socket wayland-i4] [--link PATH] [--remote tcp:HOST:PORT [--key FILE]] [--output WxH] [--layout us]
//
// --remote also accepts heads over the network, directly (tcp::7400 = every
// address, port 7400). Every such link is encrypted and authenticated with a
// pairing key: i4d creates one on first use (~/.config/i4/remote.key, 0600)
// and prints it; give it to each head (i4-head --key ...). A head without it
// cannot read a frame, send a key, or even get past the handshake.
//
// Until heads report their outputs, the session's one output is given here.
// Everything the session launches runs with WAYLAND_DISPLAY=<socket> and no
// DISPLAY (see Host::handle(fx::Spawn)).

#include <jaal/jaal.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../server/host.hpp"
#include "../server/session.hpp"

int main(int argc, char** argv) {
    using namespace i4;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr) {
        std::fprintf(stderr, "i4d: XDG_RUNTIME_DIR is not set\n");
        return 1;
    }
    std::string socket = "wayland-i4", link = std::string(runtime) + "/i4-link", layout = "us", remote,
                key_path = transport::default_key_path();
    int width = 1920, height = 1080;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--socket") socket = argv[++i];
        else if (a == "--link") link = argv[++i];
        else if (a == "--remote") remote = argv[++i];
        else if (a == "--key") key_path = argv[++i];
        else if (a == "--layout") layout = argv[++i];
        else if (a == "--output") std::sscanf(argv[++i], "%dx%d", &width, &height);
    }

    transport::Key key{};
    if (!remote.empty()) {
        auto k = transport::load_key(key_path, true);
        if (!k) {
            std::fprintf(stderr, "i4d: cannot read or create the pairing key %s\n", key_path.c_str());
            return 1;
        }
        key = *k;
    }

    auto listener = transport::Channel::listen(link);
    if (!listener) {
        std::fprintf(stderr, "i4d: cannot listen on %s\n", link.c_str());
        return 1;
    }
    server::Host host({.keyboard_layout = layout,
                       .output = wm::OutputInfo{"i4-0", {0, 0, width, height}},
                       .wayland_display = socket,
                       .app_log = std::string(runtime) + "/i4-apps.log",
                       .remote = remote,
                       .remote_key = key},
                      std::move(*listener));
    if (!host.display().add_socket(socket)) {
        std::fprintf(stderr, "i4d: cannot listen on %s\n", socket.c_str());
        return 1;
    }
    std::printf("i4d: WAYLAND_DISPLAY=%s  link %s%s%s  output %dx%d\n     apps log to %s/i4-apps.log\n", socket.c_str(),
                link.c_str(), remote.empty() ? "" : "  remote ", remote.c_str(), width, height, runtime);
    if (!remote.empty())
        std::printf("     remote heads need the pairing key (%s):\n       i4-head --link tcp:THIS-HOST:PORT --key %s\n",
                    key_path.c_str(), transport::key_hex(key).c_str());
    std::fflush(stdout);
    return jaal::run<server::Session>(host);
}
