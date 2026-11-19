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

Status: the window manager core (`wm`) is complete and tested. It covers
i3's tree, commands, layouts, workspaces, outputs, floating, fullscreen and
resize, and it is deterministic and fuzzed. The protocol server and the
presenter come next.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

```sh
git submodule update --init --recursive
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```
