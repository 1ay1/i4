// End-to-end: our compositor and our client, no libwayland on either side.
//
// The server half is only believable if a client actually gets served, so this
// forks the compositor, connects to it, and drives a real session: registry,
// bind, surface, shm pool, buffer, commit, teardown.
//
// Both halves are the same algebra read in opposite directions. If the role
// duality in wl/role.hpp is right, this works; if it is a pretty type-level
// story that does not correspond to the wire, this fails.
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include <i4/wl/affine.hpp>
#include <i4/wl/proto/wayland.hpp>
#include <i4/wl/registry.hpp>
#include <i4/wl/server/display.hpp>

#include <i4/wl/server/drive.hpp>
#include <i4/wl/shm.hpp>

using namespace i4::wl;
using namespace i4::wl::proto;
namespace srv = i4::wl::server;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

struct Core {
    using wl_display = proto::wl_display;
    using wl_registry = proto::wl_registry;
};

namespace {

constexpr std::uint32_t kDisplaySync = 0;
constexpr std::uint32_t kDisplayGetRegistry = 1;
constexpr std::uint32_t kRegistryBind = 0;

/// The compositor half. Runs in the child process.
[[noreturn]] void run_server(const std::string& socket_name) {
    srv::Display display;
    if (!display.add_socket(socket_name)) std::_Exit(2);

    int surfaces = 0;

    display.add_global<wl_compositor>(
        4, [&](srv::Client& client, std::uint32_t, wire::id_t id) {
            auto l = std::make_shared<srv::RequestListener<wl_compositor>>();
            l->on<wl_compositor::requests::create_surface>([&](
                                                               srv::NewObject<wl_surface> s) {
                ++surfaces;
                auto sl = std::make_shared<srv::RequestListener<wl_surface>>();
                sl->on<wl_surface::requests::commit>([] {});
                sl->on<wl_surface::requests::attach>(
                    [](Ref<wl_buffer>, std::int32_t, std::int32_t) {});
                sl->on<wl_surface::requests::damage>(
                    [](std::int32_t, std::int32_t, std::int32_t, std::int32_t) {});
                sl->on<wl_surface::requests::destroy>([] {});
                s.bind<4>(sl);
            });
            l->on<wl_compositor::requests::create_region>([&client](srv::NewObject<wl_region> r) {
                auto rl = std::make_shared<srv::RequestListener<wl_region>>();
                rl->on<wl_region::requests::destroy>([] {});
                rl->on<wl_region::requests::add>(
                    [](std::int32_t, std::int32_t, std::int32_t, std::int32_t) {});
                rl->on<wl_region::requests::subtract>(
                    [](std::int32_t, std::int32_t, std::int32_t, std::int32_t) {});
                r.bind<4>(rl);
            });
            client.bind_resource<wl_compositor, 4>(id, l);
        });

    display.add_global<wl_shm>(1, [&](srv::Client& client, std::uint32_t,
                                      wire::id_t id) {
        auto l = std::make_shared<srv::RequestListener<wl_shm>>();
        l->on<wl_shm::requests::create_pool>(
            [&client](srv::NewObject<wl_shm_pool> pool, BorrowedFd fd, std::int32_t) {
                if (fd.get() >= 0) ::close(fd.get());
                auto pl = std::make_shared<srv::RequestListener<wl_shm_pool>>();
                pl->on<wl_shm_pool::requests::create_buffer>(
                    [&client](srv::NewObject<wl_buffer> b, std::int32_t, std::int32_t,
                              std::int32_t, std::int32_t, wl_shm::format) {
                        auto bl = std::make_shared<srv::RequestListener<wl_buffer>>();
                        bl->on<wl_buffer::requests::destroy>([] {});
                        b.bind<1>(bl);
                    });
                pl->on<wl_shm_pool::requests::destroy>([] {});
                pl->on<wl_shm_pool::requests::resize>([](std::int32_t) {});
                pool.bind<1>(pl);
            });

        auto shm = client.bind_resource<wl_shm, 1>(id, l);
        // Events, sent from the server. The compiler checks the direction.
        shm.send<wl_shm::events::format>(wl_shm::format::argb8888);
        shm.send<wl_shm::events::format>(wl_shm::format::xrgb8888);
    });

    // wl_display and wl_registry, installed the moment a client connects:
    // before it can send anything, so a get_registry that arrives at once is
    // never met with "unknown object".
    display.on_client_created([&](srv::Client& client) {
        auto* c = &client;
        c->add_object(
            1,
            [&display, c](std::uint32_t opcode, wire::Decoder& d) {
                if (opcode == kDisplaySync) {
                    const wire::id_t cb = d.get_u32();
                    if (d.bad) return;
                    {
                        auto ev = c->event(cb, 0);  // wl_callback.done
                        ev.encoder().put_u32(display.next_serial());
                    }
                    c->send_delete_id(cb);
                } else if (opcode == kDisplayGetRegistry) {
                    const wire::id_t reg = d.get_u32();
                    if (d.bad) return;
                    c->add_object(
                        reg,
                        [&display, c](std::uint32_t op, wire::Decoder& rd) {
                            if (op != kRegistryBind) return;
                            const std::uint32_t name = rd.get_u32();
                            const std::string_view iface = rd.get_string();
                            const std::uint32_t ver = rd.get_u32();
                            const wire::id_t nid = rd.get_u32();
                            if (rd.bad) return;
                            display.handle_bind(*c, name, iface, ver, nid);
                        },
                        {});
                    display.send_globals_to(*c, reg);
                }
            },
            {});
    });

    // Serve until the test kills us.
    srv::test::Drive drive{display};
    for (;;) {
        if (drive.step(50) < 0) break;
        display.flush_clients();
    }
    std::_Exit(0);
}

}  // namespace

int main() {
    std::printf("i4::wl client/server round-trip\n\n");

    // A unique socket so concurrent test runs do not collide.
    const std::string sock = "i4-wl-test-" + std::to_string(::getpid());

    const pid_t pid = ::fork();
    if (pid < 0) {
        std::printf("SKIP: fork failed\n");
        return 0;
    }
    if (pid == 0) run_server(sock);

    // Give the child a moment to bind its socket.
    ::usleep(200 * 1000);

    struct Reaper {
        pid_t pid;
        ~Reaper() {
            ::kill(pid, SIGTERM);
            int st = 0;
            ::waitpid(pid, &st, 0);
        }
    } reaper{pid};

    // ---- the client half --------------------------------------------------
    ::setenv("WAYLAND_DISPLAY", sock.c_str(), 1);

    Error err;
    auto conn = Connection::connect(err);
    if (!conn) {
        std::printf("  FAIL cannot connect to our own compositor: %s\n",
                    err.what.c_str());
        return 1;
    }
    std::printf("connected to the i4::wl compositor\n");

    Proxy<wl_display> display{conn.get(), 1};
    auto registry = Registry<Core>::discover(*conn, display);

    std::printf("globals advertised: %zu\n", registry.globals().size());
    for (const auto& [name, g] : registry.globals())
        std::printf("  %s v%u\n", g.interface.c_str(), g.version);

    CHECK(registry.globals().size() == 2);
    CHECK(registry.find<wl_compositor>().has_value());
    CHECK(registry.find<wl_shm>().has_value());

    // ---- bind and drive ---------------------------------------------------
    auto compositor = registry.bind<wl_compositor, 4>();
    CHECK(compositor.has_value());
    if (!compositor) return 1;

    auto shm_proxy = registry.bind<wl_shm, 1>();
    CHECK(shm_proxy.has_value());
    if (!shm_proxy) return 1;

    // The server sends wl_shm.format events on bind: proof that events flow
    // server -> client through the dual of the client's own send path.
    int formats = 0;
    auto shm = own(*shm_proxy);
    shm.on<wl_shm::events::format>([&](wl_shm::format) { ++formats; });
    conn->roundtrip();
    std::printf("received %d shm format events from the server\n", formats);
    CHECK(formats == 2);

    // A surface, a pool, a buffer: the full client lifecycle.
    auto surface = own(compositor->send<wl_compositor::requests::create_surface>());
    CHECK(surface.id() != 0);

    auto mem = ShmPool::create(64 * 64 * 4);
    CHECK(static_cast<bool>(mem));

    auto pool = own(shm_proxy->send<wl_shm::requests::create_pool>(
        mem.fd(), static_cast<std::int32_t>(mem.size())));
    auto buffer = own(pool.send<wl_shm_pool::requests::create_buffer>(
        0, 64, 64, 64 * 4, wl_shm::format::xrgb8888));

    surface.send<wl_surface::requests::attach>(buffer.ref(), 0, 0);
    surface.send<wl_surface::requests::damage>(0, 0, 64, 64);
    surface.send<wl_surface::requests::commit>();
    conn->roundtrip();

    std::printf("surface + shm pool + buffer round-tripped\n");
    CHECK(!conn->error());
    if (conn->error()) std::printf("  error: %s\n", conn->error().what.c_str());

    // Teardown, which exercises delete_id in both directions.
    surface.send<wl_surface::requests::destroy>();
    conn->roundtrip();
    CHECK(!conn->error());

    // ---- a client driven only by readiness --------------------------------
    //
    // What a client inside a runtime (tapestry's nested backend, on jaal)
    // does: never roundtrip, never block. It asks, returns, and handles each
    // reply when told the socket is readable. The "runtime" here is a poll()
    // on the one fd, standing in for jaal.
    {
        Error e2;
        auto c2 = Connection::connect(e2);
        CHECK(static_cast<bool>(c2));
        if (!c2) return 1;
        Proxy<wl_display> d2{c2.get(), 1};

        auto reg = Registry<Core>::listen(*c2, d2);     // returns at once
        CHECK(reg.globals().empty());                   // nothing yet: no wait
        bool synced = false;
        auto sync = own(d2.send<wl_display::requests::sync>());
        sync.on<wl_callback::events::done>([&](std::uint32_t) { synced = true; });
        c2->flush();

        int turns = 0;
        while (!synced && turns++ < 50) {
            pollfd p{c2->fd(), POLLIN, 0};
            if (::poll(&p, 1, 1000) <= 0) break;
            CHECK(c2->ready() >= 0);
        }
        CHECK(synced);
        CHECK(reg.globals().size() == 2);   // the handlers wrote into the moved Registry
        std::printf("a readiness-driven client got %zu globals in %d turn(s), no roundtrip\n",
                    reg.globals().size(), turns);

        // A spurious readiness (nothing there) returns, it does not wait.
        CHECK(c2->ready() == 0);
        CHECK(!c2->error());
    }

    std::printf("\n%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
