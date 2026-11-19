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

Input, the window manager and the cursor go on the TCP link (reliable,
ordered). Input also goes by UDP, numbered, twice: the session applies the
first copy of each number, in order, so a key never waits behind a TCP
retransmission. Frames go by UDP on the same port, encrypted with keys from
the same handshake: a lost packet never holds up the next frame. Each frame
is fragmented with XOR parity, interleaved (a burst of up to one lost
fragment per group, consecutive or not, is rebuilt on arrival; a
keystroke-sized frame is sent three times), and a frame lost beyond repair
makes the head ask for a refresh: that window's next frame is a keyframe.
The head sends a small datagram every 50 ms so a Wi-Fi radio never dozes
(an idle Mac lost all but 64 of a 700-datagram frame). Open UDP 7400 too;
`I4_TCP_ONLY=1` on the head keeps frames on TCP.

## The pixel protocol

- **Diff on the GPU.** Each frame is copied once into a texture; one pass
  hashes every 64 px row segment (64 bits). ~250 KB of hashes come back,
  not the frame; the CPU compares them with the last frame's. 0.6 ms.
- **Scroll as a move.** Whole-row hashes vote for a vertical shift; the
  head moves the pixels it already has (`Encoded.scroll`) and only the
  newly revealed rows travel. A terminal printing a line costs one line.
- **Changed tiles, lossless.** Read off the GPU (only those), zstd. A
  keystroke is ~200 bytes; the head pastes it into its texture in place.
- **Video only when it pays.** Most of the window changed AND the head
  decodes HEVC 4:4:4 in hardware: NVENC, lossless. A head whose decoder is
  software (an M1) says so and always gets tiles.
- **Retina.** A head reports its backing scale; clients render at it
  (`preferred_buffer_scale`, `wl_output.scale`), so text is as sharp as
  a local app's.

## What it costs (measured)

Mac head (M1, Wi-Fi, ping 3.7 ms) to i4d on Linux (RTX 4060), kitty running
`cat`, `I4_HEAD_PROBE=300` (a key every 300 ms, timed until its frame is on
the Mac):

| | key->frame p50 | p90 |
|---|---|---|
| before (CPU-side diff, TCP input) | 28.8 ms | 35.9 ms |
| GPU row hashes | 8.6 ms | 31 ms |
| + input by UDP | **6.5 ms** | **7.3 ms** |

Of the 6.5 ms: ~3.7 ms is the network round trip, ~1.5 ms kitty drawing,
<1 ms i4d (diff, read, pack, seal), <0.1 ms the Mac pasting the tile.

Earlier, Linux head (local network, fullscreen):

| path | key->photon p50 | p90 |
|---|---|---|
| local head (zero copy) | 3.8 ms | 4.1 ms |
| network head (direct, encrypted) | 4.8 ms | 5.5 ms |

Tools: `I4_HEAD_PROBE=MS` types into the focused window and logs key->frame;
`I4D_TRACE=1` stamps key in / commit / sent on i4d; `I4D_VERIFY=1` with
`I4_HEAD_VERIFY=1` hash every frame on both ends to prove the head holds
exactly what the client drew.

For the lowest latency run the head fullscreen: `i4-head --fullscreen` (or
`I4_FULLSCREEN=1`). The host compositor then scans the head's buffer out
directly instead of compositing it, which is most of the difference between
~18 ms windowed and ~4 ms fullscreen. Frames are presented without waiting
for vblank by default (`--vsync` / `I4_VSYNC=1` for tear-free).
