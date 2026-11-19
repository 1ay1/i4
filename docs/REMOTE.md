# Remote heads: i4 from another machine

i4d runs where the apps run (Linux). i4-head runs where you sit: another
Linux box, or a Mac. Keys and the pointer go up, frames come down.

The link is **direct** (no ssh, no VPN needed) and always **encrypted and
authenticated**: X25519 key exchange mixed with a 32-byte pairing key, then
ChaCha20-Poly1305 on every packet (forward secret; a head without the key
cannot get past the handshake, read a frame or send a key). Cost: under 1 ms.

## On the Linux box (the session)

```sh
i4d --remote tcp::7400
```

The first time, i4d creates the pairing key (`~/.config/i4/remote.key`, mode
0600) and prints it on start:

```
     remote heads need the pairing key (~/.config/i4/remote.key):
       i4-head --link tcp:THIS-HOST:PORT --key fa64...2ed6
```

Open TCP and UDP 7400 in the firewall if there is one (`sudo ufw allow 7400`).
From outside your LAN, forward port 7400 (TCP and UDP) on the router to this box.

With an NVIDIA GPU, apps keep rendering on the GPU and every frame is
encoded there (NVENC, HEVC 4:4:4, **lossless**, no reordering): the pixels
never touch a CPU. Without NVENC, apps render to shared memory and frames go
as compressed changed tiles (also lossless).

## On the Mac (the head)

```sh
brew install gcc@16 cmake ninja pkg-config ffmpeg zstd libsodium   # Apple clang lacks C++26 pack bindings
git clone --recursive https://github.com/1ay1/i4.git && cd i4
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-16 -DI4_BUILD_TESTS=OFF   # head only
cmake --build build -j --target i4-head

./build/i4-head --link tcp:192.168.0.104:7400 --key fa64...2ed6   # the box's address, the printed key
```

Or save the key once (`mkdir -p ~/.config/i4 && echo fa64...2ed6 >
~/.config/i4/remote.key`) and drop `--key`. `I4_KEY=...` works too.

On a Mac the head forwards real key presses and releases (macOS keycodes
mapped to evdev, modifiers left/right apart), shows the app's cursor
(shape, its own image, or hidden) on the Mac's own cursor, draws windows
as Metal textures updated in place, and goes fullscreen with
`--fullscreen`. CI builds it and compiles its Metal shaders on macOS on
every push; nobody has run it against a session on a real Mac yet.

Video is decoded by VideoToolbox (the Mac's hardware decoder). `I4_HEAD_TRACE=1`
prints each frame (codec, decoder, bytes, decode time) and key->photon
latency.

## Frames by datagram

Input, the window manager and the cursor stay on the TCP link (reliable,
ordered). Frames go by UDP on the same port, encrypted with keys from the
same handshake: a lost packet never holds up the next frame. Each frame is
fragmented with XOR parity (one loss per 8 fragments rebuilt on arrival;
a keystroke-sized frame is sent three times), and a frame lost beyond
repair makes the head ask for a refresh: that window's next frame is a
keyframe. Open UDP 7400 too; `I4_TCP_ONLY=1` on the head keeps frames on
TCP. Measured with 30% of datagrams dropped on purpose (`I4_UDP_DROP=0.3`
on i4d): key->photon p50 4.8 ms, p90 5.2 ms.

## What it costs (measured, release build, kitty, 40 keys, head fullscreen)

| path | key->photon p50 | p90 |
|---|---|---|
| local head (zero copy) | 3.8 ms | 4.1 ms |
| network head (direct, encrypted) | 4.8 ms | 5.5 ms |

How: i4d compares each frame with the last one ON THE GPU (64 px tiles). A
few changed tiles (typing, a cursor, a hover) are read off the GPU and sent
losslessly, ~570 bytes per keystroke; the head pastes them into its texture
in place (0.4 ms). Big changes (scrolling, video) go as NVENC HEVC 4:4:4
lossless. Add your network's one-way latency on top.

For the lowest latency run the head fullscreen: `i4-head --fullscreen` (or
`I4_FULLSCREEN=1`). The host compositor then scans the head's buffer out
directly instead of compositing it, which is most of the difference between
~18 ms windowed and ~4 ms fullscreen. Frames are presented without waiting
for vblank by default (`--vsync` / `I4_VSYNC=1` for tear-free).
