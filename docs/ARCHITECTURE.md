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

| Layer    | What it is                                  | Portable | Status    |
|----------|---------------------------------------------|----------|-----------|
| `base`   | ids, typed coordinate spaces, match, expect | yes      | done      |
| `wm`     | the i3 brain: tree, commands, arrangement   | yes      | done      |
| `link`   | server ↔ head messages and transports       | yes      | next      |
| `server` | i4d: Wayland protocol + policy, on jaal     | Linux    | planned   |
| `head`   | i4-head: output, input, rendering, on mayag | per-OS   | planned   |
| `app`    | the binaries                                | —        | planned   |

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
  latest upstream commit.
- **mayag** (renderer). `third_party/mayag`, latest upstream. It becomes the
  head's renderer once it can import foreign buffers; see
  `third_party/mayag/docs/compositor-substrate.md`.
- **weft** → `i4/base/geom.hpp`: `Point`/`Size`/`Rect` over phantom spaces.
  weft's integer-only `Transform` was not taken, because fractional scaling
  needs something better.
- **tapestry** → `divide()` in `src/wm/arrange.cpp`: weights become pixels
  with a single rounding pass, so no gap ever appears between windows.
  tapestry's tree, which uses mutable nodes and per-node focus flags, was
  *not* taken. i4's `State` was redesigned around determinism and
  replication.
- **loom** (Wayland in C++ with no libwayland, protocol versions checked at
  compile time). This is the candidate for the `server` protocol layer, to
  be ported onto the current jaal host API. Its jaal glue targets an older
  jaal (`router`, an older `host_context`).

## Building

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan
I4_FUZZ_SEEDS=500 ./build/release/tests/test_determinism   # deep fuzz
```

Requirements: GCC 16 or another C++26 compiler, and CMake 3.29 or newer.
The `wm` layer has no other dependencies.
