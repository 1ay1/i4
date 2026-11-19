# i4 — architecture

i4 is an i3-style tiling Wayland compositor that is split down the middle:

```
   apps ──wayland──▶ i4d (session server)  ◀──link──▶  i4-head (presenter)
                     protocol, windows,                 outputs, input,
                     the i3 tree, policy                rendering, effects
```

- **i4d** runs where the apps run. It speaks Wayland and owns the window
  manager. It never draws anything.
- **i4-head** runs where the screen is. It owns the display, the input
  devices and the GPU, and it keeps a *replica* of the window manager.
- **link** is the protocol between the two. Locally it runs in one process
  with zero copies. Remotely it runs over the network, from Linux, Windows,
  macOS, Android or a browser.

Used locally, i4 is just a compositor (the Hyprland/sway use case) and the
split costs nothing. The split is what makes remote access and
crash-survival possible without redesigning anything later.

## Layers

Bottom to top. A layer may only include layers below it.
`cmake/layering.cmake` enforces this as a test.

| Layer       | What it is                                    | Portable | Status  |
|-------------|-----------------------------------------------|----------|---------|
| `base`      | ids, spaces, match, enum ranges, flags        | yes      | done    |
| `wm`        | the i3 brain: tree, commands, bindings        | yes      | done    |
| `link`      | wire codec, protocol, replica + prediction    | yes      | done    |
| `wl`        | Wayland, both halves, no libwayland (loom)    | POSIX    | done    |
| `transport` | link messages + fds between processes         | POSIX    | done    |
| `server`    | i4d: Session (program) + Host (shell)         | Linux    | working |
| `head`      | keys, motion, view, on mayag                  | per-OS   | started |
| `app`       | `i4d`, `i4-watch`, `i4-preview`               | —        | working |

`server` and `head` share a rank, so neither can include the other. They
meet only through `link`.

"Portable" means the layer builds on every head platform, so it may not
include an OS header. The lint checks that as well.

## The window manager is one pure function

```cpp
Outcome apply(State&, const Event&);    // i4/wm/apply.hpp
Arrangement arrange(const State&);      // i4/wm/arrange.hpp
```

- `State` is the entire i3 tree as a plain value: ordered maps, no pointers,
  no I/O.
- `Event` is everything that can happen to it: an output was plugged in, a
  window was mapped, a command was typed.
- `Outcome` carries only what the outside world has to do (spawn a process,
  close a window).
- Geometry is never an effect. `arrange()` derives it from `State`, and the
  server diffs consecutive arrangements to decide what to configure.

`apply` is **deterministic**: no clocks, no randomness, no addresses, only
ordered containers. `tests/test_determinism.cpp` checks this. It runs
thousands of random events per seed, checks every invariant after every
step, and then requires an exact replay. Three features rest on that
property:

1. **Prediction.** The head runs the same `apply` on a keypress and draws
   the result in the same frame. Remotely, focus changes, tiling moves and
   workspace switches therefore feel local, and the server confirms or
   corrects afterwards.
2. **Replay.** A bug report is an event log, and replaying it reproduces the
   exact state.
3. **Undo.** A `State` is cheap to copy, so undo is a stack of copies.

## The link replicates events, not pixels or state

```
head   -> Hello{protocol fingerprint}
server -> Welcome{seq, State}           the replica, once
server -> Applied{seq, Event, by?}      then every event, in order
head   -> Request{nonce, Event}         a keypress
```

- **The wire format is derived from the types** (`i4/link/wire.hpp`). C++26
  structured-binding packs (`auto& [...m] = v;`) take any struct apart, so
  there is no IDL and there are no hand-written serialisers. Integers are
  varints, and variants, optionals, vectors and maps are length- or
  index-prefixed.
- **The type universe is checked at compile time.** `Wire<T>` refuses
  floating point (a replica must compute bit-identical results on another
  CPU), pointers and unscoped enums.
- **Decoding is total and encoding canonical.** Every byte string either
  decodes to a valid value or is refused with a reason:
  - an enum past its last enumerator is refused (its count is read off the
    compiler, `i4/base/enum.hpp`);
  - so is a variant index past its last alternative;
  - so is any value a refinement type's `valid()` rejects;
  - a count that couldn't fit in the remaining bytes is refused before any
    allocation.

  Whatever decodes re-encodes to the same bytes. A 200k-input byte fuzzer
  checks this.
- **A fingerprint of the protocol's shape** (`link::protocol`) changes when
  any field or enumerator anywhere in it changes. A mismatched build is
  refused at Hello instead of misreading messages.

`link::Replica` keeps two states:

- `confirmed`: the server's state at the last applied event.
- `shown`: confirmed, plus this head's requests the server hasn't answered
  yet.

A keypress is applied to `shown` in the same frame. When the server's
events arrive, `shown` is rebuilt from `confirmed`. If nothing interleaved,
the rebuild equals the prediction and nothing moves. If something did, the
rebuild is the correction. This is why `wm::State` holds no floats: window
weights are 20-bit fixed-point `Share`s, so `apply()` agrees bit for bit on
every machine.

`tests/test_replica.cpp` checks convergence. It runs three heads and one
server with random delays in both directions, server-side events
interleaving, and every message encoded and decoded along the way. After
the network drains, every head equals the server exactly.

## The head draws on mayag

`src/head/` turns the replica into frames:

- `keys.hpp` maps mayag's physical keys to i3 keysym names with an
  exhaustive switch. Bindings are therefore portable data, and a head on
  macOS reads the same table as one on Linux.
- `scene.cpp` gives every tile a spring (`mayag::Animated`, fixed-step) that
  chases its arranged rectangle, and renders the frame with mayag's DSL.
  Springs exist only in the head; the window manager never sees a float.

`i4-preview` (`src/app/preview.cpp`) is the whole architecture in one window:
server, link and head as a single jaal program. Every message is encoded,
delayed by a simulated network (F1 cycles 0/50/150/400 ms) and decoded. At
400 ms, focus and moves still land in the frame you pressed the key, while
the bar counts requests in flight. `i4-preview --headless` runs the same
program under ctest. There are no Wayland clients yet, so `exec` opens a
stand-in tile.

## Every process is a jaal program on a jaal host

Each i4 binary has the same split:

- **The program** is a jaal `Program`: a `Model`, a `Msg` variant, one
  `update` per message, and effects returned as values. It is pure, with no
  fd, protocol object or syscall in it.
- **The host** is the imperative shell. It owns every fd, turns what
  happens on them into events, and carries out the program's effects in
  `handle()`. It decides nothing.

For i4d (`src/server/`):

| File | Role |
|------|------|
| `vocab.hpp` | the whole conversation: `server::Event` in, `fx::*` effects out |
| `session.*` | the program: the replicated window manager, who is configured, focus, frame pacing, which head hears what |
| `host*.cpp` | the shell: the Wayland display (`i4::wl`), each client's objects, the link sockets |

The Session never sees a protocol object. It says
`fx::Configure{window, size, activated}`, and the host turns that into
`xdg_toplevel.configure`.

All of i4d's decisions are therefore tested under `jaal::headless` with no
socket at all (`tests/test_session.cpp`): events in, recorded effects out.
Examples: a frame callback waits until a head has shown that frame, and a
head from another build is refused and dropped.

Typed protocol state in the host:

- A surface's double-buffered state is two types, `Pending` and `Current`,
  and `commit()` is the only path between them.
- A toplevel's life is `Unmapped | Mapped{WindowId}`.
- Every object is a `wl::Resource<I, V>`, so an event newer than the bound
  version doesn't compile.

## Wayland: `i4::wl`, from loom

loom's code lives in the tree as the `wl` layer (`include/i4/wl`, `src/wl`,
`tools/wlgen.py`, `protocols/`). Only the namespace changed. Its old jaal
glue was left out, because i4d's host does that job now.

- **Generation.** 103 protocols are generated from XML into types: the
  system's, plus the vendored wlroots, KDE and Hyprland ones.
- **Tests.** loom's own suite runs in i4's ctest: algebra laws, resources,
  completeness, a forked client/server round trip, server conformance, a
  decoder fuzzer, and 20 compile-fail cases (a request newer than the bound
  version, a server sending a request, copying an fd, ...).

## Pixels never touch the log

`down::Content` says only that a window has a new frame, its size, offset
and stride. The pixels ride beside it. Locally that is the client's own
shm pool fd, passed by `SCM_RIGHTS`, so nothing is copied. The head answers
`up::Shown`, and only then does the client's frame callback fire, so an
app draws at the rate the screen, local or remote, actually takes frames.
With no head attached, frames are released at once and no client hangs.

## The launcher ($mod+d)

i4's dmenu lives in the head (`src/head/launcher.*`), but its list comes
from the session:

- **Apps are scanned where they run.** i4d reads the `.desktop` files
  (`src/server/apps.cpp`) with a `Cmd::perform` off the loop, and sends the
  list as `down::Apps`. A laptop head attached to a desktop session
  therefore lists the desktop's apps.
- **Launching goes through the normal path.** The head sends a predicted
  `exec` request, the same one a key binding produces. It enters the
  replicated log and the window opens where i3 would put it.
- **Ranking is fuzzy and pure.** A prefix beats a word start, which beats
  scattered letters; contiguous runs score extra; and the comment is
  searched as a fallback ("browser" finds Firefox). Terminal apps run in
  `$TERMINAL`.

## Types do the checking

- **Coordinate spaces are types** (`Point<space::Desktop>`, `Rect<space::Surface>`, …).
  Mixing two spaces is a compile error, which `tests/fail/spaces.cpp` proves.
- **Ids are distinct types.** `ConId` is not a `WindowId`
  (`tests/fail/ids.cpp`).
- **The tree's grammar lives in sum types.** A container is a `Split` or a
  `Leaf`, never both. Where a container sits is `TilingRoot | Floating | Child`,
  and only `Floating` has a rectangle.
- **`match()` is exhaustive.** A new alternative breaks every match that
  doesn't handle it (`tests/fail/match.cpp`). `-Werror=switch` does the same
  for enums.
- **Whatever the types cannot express is checked by `wm::check()`.** That
  covers parent/child agreement in both directions, MRU lists being
  permutations, weights summing to 1, and focus agreeing with every MRU list
  on its path. Every test runs it after every step.

## i3 semantics

`src/wm/apply.cpp` follows i3's behaviour for focus, move, split and layout,
workspaces, outputs, floating, fullscreen and resize. `tests/test_wm.cpp`
states each behaviour in i3's tree notation:

```
d.cmd("focus left; move right");
CHECK_EQ(d.shape(), "H[V[b c a*]]");      // * focus, ! fullscreen, | floating
```

Commands are parsed into a typed AST (`i4/wm/command.hpp`) and then applied.
So a keybinding, the IPC socket, a config file and a head predicting a
keypress all run exactly the same values.

Known gaps compared with i3: criteria (`[class="x"]`), marks, scratchpad,
modes, sticky windows, floating containers, `move` of a whole workspace's
contents.

## Where the pieces come from

i4 is designed from scratch. Code is lifted only where it is the best
version available, and only after review:

- **jaal** (runtime: event loop, typed Elm programs, effects). Pulled in as
  `third_party/mayag/third_party/jaal`, since mayag requires it, at the
  latest upstream commit. `i4-preview` is a jaal program.
- **mayag** (renderer). `third_party/mayag`, latest upstream, built in-tree
  as a SYSTEM dependency. The head draws with its DSL and animates with its
  springs. Compositing client buffers still needs mayag to import foreign
  memory (dmabuf); see `third_party/mayag/docs/compositor-substrate.md`.
- **weft** → `i4/base/geom.hpp`: `Point`/`Size`/`Rect` over phantom spaces.
  weft's integer-only `Transform` was not taken, because fractional scaling
  needs something better.
- **tapestry** → `divide()` in `src/wm/arrange.cpp`: weights become pixels
  with a single rounding pass, so no gap ever appears between windows.
  tapestry's tree, which uses mutable nodes and per-node focus flags, was
  *not* taken. i4's `State` was redesigned around determinism and
  replication.
- **loom** (Wayland in C++ with no libwayland). Its code is copied in as
  the `wl` layer, described above.

## Building

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan
I4_FUZZ_SEEDS=500 ./build/release/tests/test_determinism   # deep fuzz
I4_FUZZ_SEEDS=500 ./build/release/tests/test_replica
./build/debug/i4-preview                                   # try it ($mod = Alt)

# a real session, on a nested socket:
./build/debug/i4d --output 1600x900 &                      # WAYLAND_DISPLAY=wayland-i4
WAYLAND_DISPLAY=wayland-i4 alacritty &
./build/debug/i4-watch                                     # print the tree live
./build/debug/i4-watch --run "split v; exec kitty"         # i4-msg, over the link
```

Requirements: GCC 16 or Clang 23 (C++26 structured-binding packs), and
CMake 3.29 or newer. `wm` and `link` have no other dependencies. The head
needs nothing at build time either, because mayag loads Wayland and Vulkan
with dlopen.
