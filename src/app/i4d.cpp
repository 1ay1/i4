// src/app/i4d.cpp — the session server: Session (the program) on Host (the shell).
//
//   i4d [--socket wayland-i4] [--link PATH] [--output WxH] [--layout us]
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
    std::string socket = "wayland-i4", link = std::string(runtime) + "/i4-link", layout = "us";
    int width = 1920, height = 1080;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--socket") socket = argv[++i];
        else if (a == "--link") link = argv[++i];
        else if (a == "--layout") layout = argv[++i];
        else if (a == "--output") std::sscanf(argv[++i], "%dx%d", &width, &height);
    }

    auto listener = transport::Channel::listen(link);
    if (!listener) {
        std::fprintf(stderr, "i4d: cannot listen on %s\n", link.c_str());
        return 1;
    }
    server::Host host({.keyboard_layout = layout,
                       .output = wm::OutputInfo{"i4-0", {0, 0, width, height}},
                       .wayland_display = socket},
                      std::move(*listener));
    if (!host.display().add_socket(socket)) {
        std::fprintf(stderr, "i4d: cannot listen on %s\n", socket.c_str());
        return 1;
    }
    std::printf("i4d: WAYLAND_DISPLAY=%s  link %s  output %dx%d\n", socket.c_str(), link.c_str(), width, height);
    return jaal::run<server::Session>(host);
}
