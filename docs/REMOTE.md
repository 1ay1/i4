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

Open TCP 7400 in the firewall if there is one (`sudo ufw allow 7400/tcp`).
From outside your LAN, forward port 7400 on the router to this box.

With an NVIDIA GPU, apps keep rendering on the GPU and every frame is
encoded there (NVENC, HEVC 4:4:4, **lossless**, no reordering): the pixels
never touch a CPU. Without NVENC, apps render to shared memory and frames go
as compressed changed tiles (also lossless).

## On the Mac (the head)

```sh
brew install cmake pkg-config ffmpeg zstd
brew install libsodium
git clone --recursive git@github.com:1ay1/i4.git && cd i4
cmake -B build -DCMAKE_BUILD_TYPE=Release -DI4_BUILD_TESTS=OFF   # builds the head only
cmake --build build -j --target i4-head

./build/i4-head --link tcp:192.168.0.104:7400 --key fa64...2ed6   # the box's address, the printed key
```

Or save the key once (`mkdir -p ~/.config/i4 && echo fa64...2ed6 >
~/.config/i4/remote.key`) and drop `--key`. `I4_KEY=...` works too.

Video is decoded by VideoToolbox (the Mac's hardware decoder). `I4_HEAD_TRACE=1`
prints each frame (codec, decoder, bytes, decode time) and key->photon
latency.

## What it costs (measured, release build, kitty, 40 keys)

| path | key->photon p50 | p90 |
|---|---|---|
| local head (zero copy) | 18.4 ms | 22.3 ms |
| network head (direct, encrypted, GPU video) | 21.8 ms | 34.4 ms |

Encode 1.5 ms (NVENC), decode ~2 ms; a keystroke's frame is ~100-400 bytes.
Add your network's one-way latency on top.
