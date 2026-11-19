# i4

An i3-style tiling Wayland compositor with a client/server split.

- **Locally**, it is your compositor: i3's tree, i3's commands, i3's config
  and IPC, at native speed.
- **Remotely**, the same session can be attached from another machine
  (Linux, Windows, macOS, Android, a browser). Window-management actions run
  on your side, so they stay instant.

```
apps ──wayland──▶ i4d ◀──link──▶ i4-head ──▶ screen, keyboard, GPU
```

Status:

- **Window manager core (`wm`)**: complete. i3's tree, commands, layouts,
  workspaces, outputs, floating, fullscreen, resize and default bindings.
  It's deterministic, integer-only and fuzzed.
- **Link**: complete. A wire format derived from the types, a fingerprinted
  protocol, and a replica that predicts keypresses and converges exactly.
- **Head**: started. It draws on mayag.
- **Wayland server (i4d, on loom)**: next.

Try it with `./build/debug/i4-preview`. That's the server, link and head
in one window. Press F1 to add 400 ms of network latency: window management
still responds in the same frame.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

```sh
git submodule update --init --recursive
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```
