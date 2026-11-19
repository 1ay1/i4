// Server-side conformance: the lifecycle hooks a compositor needs.
//
// libwayland's server API is mostly bookkeeping — who connected, who left,
// what globals exist right now. A compositor built on a library without those
// hooks ends up polling, which races clients that send a request immediately
// after connecting. These are the behaviours, tested over a real socket.
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <i4/wl/proto/wayland.hpp>
#include <i4/wl/registry.hpp>
#include <i4/wl/server/display.hpp>

#include <i4/wl/server/drive.hpp>

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

namespace {

constexpr std::uint32_t kDisplaySync = 0;
constexpr std::uint32_t kDisplayGetRegistry = 1;
constexpr std::uint32_t kRegistryBind = 0;

/// Install wl_display's two requests on a client. Every compositor does this;
/// the server core stays out of it because a compositor may want to intercept.
void install_display(srv::Display& display, srv::Client& c) {
    c.add_object(
        1,
        [&display, &c](std::uint32_t op, wire::Decoder& d) {
            if (op == kDisplaySync) {
                const wire::id_t cb = d.get_u32();
                if (d.bad) return;
                {
                    auto ev = c.event(cb, 0);
                    ev.encoder().put_u32(display.next_serial());
                }
                c.send_delete_id(cb);
            } else if (op == kDisplayGetRegistry) {
                const wire::id_t reg = d.get_u32();
                if (d.bad) return;
                c.add_object(
                    reg,
                    [&display, &c](std::uint32_t o, wire::Decoder& rd) {
                        if (o != kRegistryBind) return;
                        const std::uint32_t name = rd.get_u32();
                        const std::string_view iface = rd.get_string();
                        const std::uint32_t ver = rd.get_u32();
                        const wire::id_t nid = rd.get_u32();
                        if (rd.bad) return;
                        display.handle_bind(c, name, iface, ver, nid);
                    },
                    {});
                display.send_globals_to(c, reg);
            }
        },
        {});
}

/// Connect a raw client socket to a display name, without building a full
/// Connection: these tests care about server-side bookkeeping.
int raw_connect(const std::string& name) {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime) return -1;
    const std::string path = std::string(runtime) + "/" + name;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size());

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string unique_socket(const char* tag) {
    return std::string("i4-wl-srv-") + tag + "-" + std::to_string(::getpid());
}

}  // namespace

// ===========================================================================
// wl_display_add_client_created_listener: a compositor must be told about a
// client BEFORE that client's first request arrives.
// ===========================================================================
static void test_client_created_hook() {
    std::printf("client-created fires before the first request\n");

    srv::Display display;
    const std::string sock = unique_socket("created");
    CHECK(display.add_socket(sock));
    srv::test::Drive drive{display};

    int created = 0;
    display.on_client_created([&](srv::Client& c) {
        ++created;
        install_display(display, c);
    });

    const int fd = raw_connect(sock);
    CHECK(fd >= 0);

    drive.step(100);
    CHECK(created == 1);
    CHECK(display.clients().size() == 1);

    // wl_display@1 must already be installed, or a client that sends
    // get_registry immediately would get "unknown object".
    CHECK(display.clients()[0]->has_object(1));

    if (fd >= 0) ::close(fd);
    std::printf("  hook ran for %d client(s), wl_display installed\n", created);
}

// ===========================================================================
// wl_client_add_destroy_listener: the compositor must learn when a client
// goes, so it can release the surfaces it was holding.
// ===========================================================================
static void test_client_destroy_hook() {
    std::printf("client-destroy fires on hangup\n");

    srv::Display display;
    const std::string sock = unique_socket("destroy");
    CHECK(display.add_socket(sock));
    srv::test::Drive drive{display};

    int destroyed = 0;
    display.on_client_created([&](srv::Client& c) {
        install_display(display, c);
        c.on_destroy([&](srv::Client&) { ++destroyed; });
    });

    const int fd = raw_connect(sock);
    CHECK(fd >= 0);
    drive.step(100);
    CHECK(display.clients().size() == 1);

    // The client goes away.
    ::close(fd);
    for (int i = 0; i < 20 && !destroyed; ++i) drive.step(50);

    CHECK(destroyed == 1);
    CHECK(display.clients().empty());
    std::printf("  destroy hook ran, client table is empty\n");
}

// ===========================================================================
// wl_display_destroy_clients: shutting down must disconnect everyone, running
// their destroy listeners.
// ===========================================================================
static void test_destroy_clients() {
    std::printf("destroy_clients disconnects everyone\n");

    srv::Display display;
    const std::string sock = unique_socket("destroyall");
    CHECK(display.add_socket(sock));
    srv::test::Drive drive{display};

    int destroyed = 0;
    display.on_client_created([&](srv::Client& c) {
        install_display(display, c);
        c.on_destroy([&](srv::Client&) { ++destroyed; });
    });

    std::vector<int> fds;
    for (int i = 0; i < 3; ++i) {
        const int fd = raw_connect(sock);
        CHECK(fd >= 0);
        fds.push_back(fd);
    }
    drive.step(100);
    CHECK(display.clients().size() == 3);

    display.destroy_clients();
    CHECK(destroyed == 3);
    CHECK(display.clients().empty());

    for (int fd : fds)
        if (fd >= 0) ::close(fd);
    std::printf("  3 clients disconnected, 3 hooks ran\n");
}

// ===========================================================================
// wl_global_destroy: removing a global must send wl_registry.global_remove to
// every client that is watching, not just drop it server-side.
// ===========================================================================
static void test_global_remove_is_announced() {
    std::printf("removing a global announces global_remove\n");

    srv::Display display;
    const std::string sock = unique_socket("globalrm");
    CHECK(display.add_socket(sock));
    srv::test::Drive drive{display};

    display.on_client_created([&](srv::Client& c) { install_display(display, c); });

    const std::uint32_t name = display.add_global<wl_compositor>(
        4, [](srv::Client&, std::uint32_t, wire::id_t) {});
    CHECK(display.globals().size() == 1);

    const int fd = raw_connect(sock);
    CHECK(fd >= 0);
    drive.step(100);

    // Ask for the registry by hand: get_registry(new_id 2).
    std::uint32_t req[3];
    req[0] = 1;                       // wl_display
    req[1] = (12u << 16) | kDisplayGetRegistry;
    req[2] = 2;                       // the registry's id
    CHECK(::send(fd, req, sizeof(req), 0) == sizeof(req));
    drive.step(100);
    display.flush_clients();

    // Drain the advertisement so the next read sees only global_remove.
    std::uint32_t buf[64];
    [[maybe_unused]] ssize_t got = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);

    display.remove_global(name);
    display.flush_clients();
    CHECK(display.globals().empty());

    // wl_registry.global_remove is opcode 1 on the registry object.
    std::uint32_t ev[4]{};
    const ssize_t n = ::recv(fd, ev, sizeof(ev), MSG_DONTWAIT);
    CHECK(n >= 12);
    if (n >= 12) {
        CHECK(ev[0] == 2);                  // the registry
        CHECK((ev[1] & 0xffff) == 1);       // global_remove
        CHECK(ev[2] == name);
        std::printf("  client told: global %u removed\n", ev[2]);
    }

    if (fd >= 0) ::close(fd);
}

// ===========================================================================
// Credentials: a compositor gates privileged protocols on who is asking.
// ===========================================================================
static void test_client_credentials() {
    std::printf("client credentials are readable\n");

    srv::Display display;
    const std::string sock = unique_socket("creds");
    CHECK(display.add_socket(sock));
    srv::test::Drive drive{display};
    display.on_client_created([&](srv::Client& c) { install_display(display, c); });

    const int fd = raw_connect(sock);
    CHECK(fd >= 0);
    drive.step(100);
    CHECK(display.clients().size() == 1);

    if (!display.clients().empty()) {
        const auto creds = display.clients()[0]->credentials();
        CHECK(creds.pid == ::getpid());  // we are our own client here
        CHECK(creds.uid == ::getuid());
        std::printf("  pid=%d uid=%d\n", creds.pid, creds.uid);
    }

    if (fd >= 0) ::close(fd);
}

// ===========================================================================
// A server with no loop of its own.
//
// Whoever runs the program owns waiting. The display only says what it needs
// watched (on_watch / on_unwatch) and is told what is ready (ready()). That
// is the whole contract a runtime like jaal needs; these tests drive it with
// the smallest runtime that satisfies it (support/drive.hpp).
// ===========================================================================

static void test_runtime_drives_the_display() {
    std::printf("a runtime drives the display with no loop inside it\n");

    srv::Display display;
    srv::test::Drive rt{display};
    const std::string sock = unique_socket("loopfree");
    CHECK(display.add_socket(sock));
    CHECK(rt.watched() == 1);  // the listen socket, announced

    int created = 0;
    display.on_client_created([&](srv::Client& c) {
        ++created;
        install_display(display, c);
    });

    const int fd = raw_connect(sock);
    CHECK(fd >= 0);
    rt.step(100);  // listen socket ready -> accept
    CHECK(created == 1);
    CHECK(rt.watched() == 2);  // and the client, announced on accept

    if (fd >= 0) ::close(fd);
    std::printf("  accepted through ready(), client watched separately\n");
}

// The bug this design fixes: teardown was an idle callback on the display's
// own loop, run at the start of its NEXT dispatch. A runtime that dispatches
// only when something is readable never gets a next dispatch after the last
// client quits, so the client (and every window it had) lived forever.
static void test_disconnect_is_torn_down_in_the_same_ready() {
    std::printf("a client that quits is gone when ready() returns\n");

    srv::Display display;
    srv::test::Drive rt{display};
    const std::string sock = unique_socket("hangup");
    CHECK(display.add_socket(sock));

    int destroyed = 0;
    display.on_client_created([&](srv::Client& c) {
        install_display(display, c);
        c.on_destroy([&](srv::Client&) { ++destroyed; });
    });

    const int fd = raw_connect(sock);
    rt.step(100);
    CHECK(display.clients().size() == 1);

    ::close(fd);   // and then NOTHING else happens
    rt.step(100);  // exactly one readiness: the hangup

    CHECK(destroyed == 1);
    CHECK(display.clients().empty());
    CHECK(rt.watched() == 1);  // unwatched, only the listen socket left
    std::printf("  destroyed and unwatched within one step\n");
}

// post_error ends the client, per the protocol. It used to only mark it, so
// a client that got an error stayed connected until IT hung up.
static void test_protocol_error_drops_the_client() {
    std::printf("a protocol error drops the client at once\n");

    srv::Display display;
    srv::test::Drive rt{display};
    const std::string sock = unique_socket("perror");
    CHECK(display.add_socket(sock));

    int destroyed = 0;
    display.on_client_created([&](srv::Client& c) {
        install_display(display, c);
        c.on_destroy([&](srv::Client&) { ++destroyed; });
    });

    const int fd = raw_connect(sock);
    rt.step(100);
    CHECK(display.clients().size() == 1);

    // A request to an object that doesn't exist: the server answers with
    // wl_display.error and must let the client go.
    std::uint32_t req[2] = {77, (8u << 16) | 0};
    CHECK(::send(fd, req, sizeof(req), 0) == sizeof(req));
    rt.step(100);

    CHECK(destroyed == 1);
    CHECK(display.clients().empty());

    // And the client did get told why before it was cut off.
    std::uint32_t ev[16]{};
    const ssize_t n = ::recv(fd, ev, sizeof(ev), MSG_DONTWAIT);
    CHECK(n >= 12 && ev[0] == 1 && (ev[1] & 0xffff) == 0);   // wl_display.error

    if (fd >= 0) ::close(fd);
    std::printf("  error sent, client destroyed, no hangup needed\n");
}

// ===========================================================================
// Versions: a client that bound an old version never sees a newer event.
// ===========================================================================

static void test_no_event_outruns_the_client() {
    std::printf("an event newer than the client's version is never sent\n");

    srv::Display display;
    const std::string sock = unique_socket("versions");
    CHECK(display.add_socket(sock));
    srv::test::Drive drive{display};
    display.on_client_created([&](srv::Client& c) { install_display(display, c); });

    srv::Client* who = nullptr;
    display.on_client_created([&](srv::Client& c) { who = &c; });

    const int fd = raw_connect(sock);
    CHECK(fd >= 0);
    drive.step(100);
    CHECK(who != nullptr);
    if (!who) return;

    // A wl_output the client bound at v1, with the code written for v4.
    auto l = std::make_shared<srv::RequestListener<wl_output>>();
    auto out = who->bind_resource<wl_output, 4>(7, l, 1);
    CHECK(who->version_of(7) == 1);

    // geometry is v1: sent. name is v4: must not be.
    out.send<wl_output::events::geometry>(0, 0, 0, 0, wl_output::subpixel::unknown, "m", "x",
                                          wl_output::transform::normal);
    out.send<wl_output::events::name>("DP-1");
    display.flush_clients();

    std::uint32_t buf[64]{};
    const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    // Walk the messages: only one, the geometry (opcode 0) on object 7.
    int to_output = 0;
    bool name_sent = false;
    for (ssize_t off = 0; n > 0 && off + 8 <= n;) {
        const std::uint32_t obj = buf[off / 4], meta = buf[off / 4 + 1];
        const std::uint32_t size = meta >> 16, op = meta & 0xffff;
        if (size < 8) break;
        if (obj == 7) {
            ++to_output;
            if (op == wl_output::events::name::opcode) name_sent = true;
        }
        off += size;
    }
    CHECK(to_output == 1);
    CHECK(!name_sent);

    // An object the client creates through a v1 object is v1 too: that's
    // what NewObject::bind does with the creator it carries.
    auto cl = std::make_shared<srv::RequestListener<wl_output>>();
    srv::NewObject<wl_output>{who, 8, 7}.bind<4>(cl);
    CHECK(who->version_of(8) == 1);
    std::printf("  v1 client: geometry sent, name (v4) dropped; a child inherits v1\n");
    if (fd >= 0) ::close(fd);
}

// ===========================================================================
// delete_id is how a client learns it may reuse an id it allocated. Ids from
// 0xff000000 up are the server's (wl_data_offer); a delete_id for one is an
// error in every client library, and killed firefox and kitty.
// ===========================================================================
static void test_delete_id_only_for_client_ids() {
    std::printf("delete_id is never sent for a server-allocated id\n");

    srv::Display display;
    const std::string sock = unique_socket("deleteid");
    CHECK(display.add_socket(sock));
    srv::test::Drive drive{display};
    srv::Client* who = nullptr;
    display.on_client_created([&](srv::Client& c) {
        install_display(display, c);
        who = &c;
    });

    const int fd = raw_connect(sock);
    CHECK(fd >= 0);
    drive.step(100);
    CHECK(who != nullptr);
    if (!who) return;

    who->send_delete_id(0xff000003);
    who->send_delete_id(9);
    display.flush_clients();

    std::uint32_t buf[16]{};
    const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    // Exactly one message: wl_display@1.delete_id(9).
    CHECK(n == 12);
    CHECK(buf[0] == 1 && (buf[1] & 0xffff) == 1 && buf[2] == 9);
    std::printf("  client id confirmed, server id kept quiet\n");
    if (fd >= 0) ::close(fd);
}

int main() {
    std::printf("i4::wl server conformance\n\n");
    test_client_created_hook();
    test_client_destroy_hook();
    test_destroy_clients();
    test_global_remove_is_announced();
    test_client_credentials();
    test_runtime_drives_the_display();
    test_disconnect_is_torn_down_in_the_same_ready();
    test_protocol_error_drops_the_client();
    test_no_event_outruns_the_client();
    test_delete_id_only_for_client_ids();
    std::printf("\n%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
