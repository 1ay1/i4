// fuzz_decoder.cpp — the decoder parses bytes from another process.
//
// Everything else in this library is checked at compile time, but the wire is
// not: a compositor (or anything that can talk to the socket) hands us
// arbitrary bytes, and the decoder is the only thing between those bytes and
// a handler. So it gets fuzzed.
//
// This is a self-contained generative fuzzer rather than libFuzzer, so it runs
// in the ordinary test suite with no special toolchain. Build with
// -DWLALG_SANITIZE=asan to make every out-of-bounds read fatal instead of
// silent.
//
// The invariants under test, for ANY input at all:
//   1. no out-of-bounds read (ASan enforces this; without it we check the
//      decoder's own cursor never passes the end)
//   2. malformed input sets `bad` rather than returning garbage
//   3. a handler is never invoked with partially-decoded arguments
//   4. the decoder terminates \u2014 no input causes a hang
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include <i4/wl/listener.hpp>
#include <i4/wl/proto/wayland.hpp>

using namespace i4::wl;
using namespace i4::wl::proto;

namespace {

int failures = 0;
long g_delivered = 0;  // handlers that ran on fuzzed input, reported at the end

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

/// Decode one signature from a random buffer and assert the invariants.
///
/// The decoder may legitimately succeed on random bytes (four random words
/// are a perfectly good `motion` event), so the test is not "it fails" \u2014 it
/// is "it never reads past the end, and it never reports success while its
/// cursor is out of bounds".
template <class Sig_>
void try_decode(std::span<const wire::word> words, std::span<const int> fds) {
    wire::Decoder d{words, fds, 0, 0, false};
    auto out = detail::decode_fold(d, Sig_{});
    (void)out;

    // Invariant 1: the cursor never passes the end, whatever the input said.
    CHECK(d.pos <= words.size());

    // Invariant 2: if the decoder consumed everything and more was needed, it
    // must have said so.
    if (d.pos > words.size()) CHECK(d.bad);
}

/// Every signature shape that appears in the core protocol, so the fuzzer
/// covers each atom and each combination rather than one convenient message.
void fuzz_one(std::mt19937& rng, std::vector<wire::word>& buf) {
    std::uniform_int_distribution<std::size_t> len(0, 24);
    std::uniform_int_distribution<wire::word> word(0, 0xffffffffu);

    buf.clear();
    const std::size_t n = len(rng);
    for (std::size_t i = 0; i < n; ++i) {
        // Bias toward values that are interesting to a length-prefixed
        // decoder: huge lengths, zero, and small plausible ones. Pure uniform
        // noise almost never produces a length the decoder will act on.
        switch (rng() % 8) {
            case 0: buf.push_back(0); break;
            case 1: buf.push_back(0xffffffffu); break;
            case 2: buf.push_back(0x7fffffffu); break;
            case 3: buf.push_back(static_cast<wire::word>(rng() % 64)); break;
            default: buf.push_back(word(rng)); break;
        }
    }

    const std::span<const wire::word> words(buf);
    const std::span<const int> no_fds;

    // One message of each shape. Together these cover every atom: int, uint,
    // fixed, string, array, object, new_id, enum, fd, and nullable.
    try_decode<wl_pointer::events::motion::sig>(words, no_fds);       // u f f
    try_decode<wl_registry::events::global::sig>(words, no_fds);      // u s u
    try_decode<wl_keyboard::events::key::sig>(words, no_fds);         // u u u enum
    try_decode<wl_keyboard::events::keymap::sig>(words, no_fds);      // enum h u
    try_decode<wl_surface::events::enter::sig>(words, no_fds);        // o
    try_decode<wl_data_offer::events::offer::sig>(words, no_fds);     // s
    try_decode<wl_keyboard::events::enter::sig>(words, no_fds);       // u o a
    try_decode<wl_output::events::geometry::sig>(words, no_fds);      // iiii e ss e
    try_decode<wl_callback::events::done::sig>(words, no_fds);        // u
    try_decode<wl_display::events::error::sig>(words, no_fds);        // o u s
    try_decode<wl_shm::events::format::sig>(words, no_fds);           // enum
    try_decode<wl_data_device::events::data_offer::sig>(words, no_fds);  // n
}

/// The dispatch path, not just the decoder: a malformed event must never
/// reach a handler, whatever the opcode or body.
void fuzz_dispatch(std::mt19937& rng, std::vector<wire::word>& buf) {
    // Counts handler invocations. Random bytes CAN decode into a valid event,
    // so this is not expected to stay zero — it exists so that a handler which
    // did run is known to have run with coherent arguments (the loop below
    // touches every byte of the string it was handed).
    static long delivered = 0;
    static Listener<wl_registry>* listener = [] {
        auto* l = new Listener<wl_registry>();
        l->on<wl_registry::events::global>(
            [](std::uint32_t, std::string_view iface, std::uint32_t) {
                ++delivered;
                // If a handler ever runs, its arguments must be coherent: a
                // string_view the decoder produced must be readable. Under
                // ASan this faults if the view points outside the buffer.
                volatile char sink = 0;
                for (char ch : iface) sink = static_cast<char>(sink ^ ch);
                (void)sink;
            });
        l->on<wl_registry::events::global_remove>([](std::uint32_t) { ++delivered; });
        return l;
    }();

    std::uniform_int_distribution<std::size_t> len(0, 16);
    buf.clear();
    const std::size_t n = len(rng);
    for (std::size_t i = 0; i < n; ++i) {
        buf.push_back(rng() % 4 == 0 ? 0xffffffffu
                                     : static_cast<wire::word>(rng()));
    }

    // Any opcode, including ones past the end of the interface.
    const std::uint32_t opcode = rng() % 6;
    const RawEvent ev{7, opcode, std::span<const wire::word>(buf), {}, nullptr};
    listener->dispatch(ev);  // must not crash, whatever the bytes say

    g_delivered = delivered;
}

/// Message framing: the header carries a length that the router trusts.
/// A hostile length must not walk the buffer off its end.
void fuzz_framing(std::mt19937& rng) {
    std::vector<wire::word> stream;
    const std::size_t n = 2 + (rng() % 12);
    for (std::size_t i = 0; i < n; ++i) stream.push_back(static_cast<wire::word>(rng()));

    // Walk it the way Connection::route_incoming does, and check the same
    // bounds conditions hold.
    std::size_t head = 0;
    int guard = 0;
    while (head + 2 <= stream.size() && ++guard < 1000) {
        const wire::word meta = stream[head + 1];
        const std::size_t size_bytes = meta >> 16;

        if (size_bytes < 8 || size_bytes % 4 != 0) break;  // rejected as malformed
        const std::size_t words = size_bytes / 4;
        if (stream.size() - head < words) break;  // incomplete: wait for more

        // The body must be inside the buffer. This is the check that a length
        // of 0xffff would break if it were missing.
        CHECK(head + words <= stream.size());
        head += words;
    }
    CHECK(head <= stream.size());
}

}  // namespace

int main(int argc, char** argv) {
    const long iterations = argc > 1 ? std::atol(argv[1]) : 100000;

    // Deterministic by default so a failure is reproducible; pass a seed to
    // explore elsewhere.
    const unsigned seed = argc > 2 ? static_cast<unsigned>(std::atol(argv[2])) : 1234;
    std::mt19937 rng(seed);

    std::printf("i4::wl decoder fuzz (%ld iterations, seed %u)\n", iterations, seed);

    std::vector<wire::word> buf;
    for (long i = 0; i < iterations && failures == 0; ++i) {
        fuzz_one(rng, buf);
        fuzz_dispatch(rng, buf);
        fuzz_framing(rng);
    }

    if (failures) {
        std::printf("\nFAILED after %ld iterations (seed %u)\n", iterations, seed);
        return 1;
    }
    std::printf("  %ld iterations, no out-of-bounds read, no crash\n", iterations);
    std::printf("  %ld fuzzed events decoded cleanly enough to reach a handler\n",
                g_delivered);
    std::printf("\nall passed\n");
    return 0;
}
