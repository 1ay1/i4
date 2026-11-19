# Remote heads: i4 from another machine

i4d runs where the apps run (Linux). i4-head runs where you sit: another
Linux box, or a Mac. Keys and the pointer go up, frames come down.

## On the Linux box (the session)

```sh
i4d --remote tcp:127.0.0.1:7400      # heads reach it through ssh (recommended)
# or: i4d --remote tcp::7400          # any address, no auth or encryption yet: trusted LAN only
```

With an NVIDIA GPU, apps keep rendering on the GPU and every frame is
encoded there (NVENC, HEVC 4:4:4, **lossless**, no reordering): the pixels
never touch a CPU. Without NVENC, apps render to shared memory and frames go
as compressed changed tiles (also lossless).

## On the Mac (the head)

```sh
brew install cmake pkg-config ffmpeg zstd
git clone --recursive git@github.com:1ay1/i4.git && cd i4
cmake -B build -DCMAKE_BUILD_TYPE=Release -DI4_BUILD_TESTS=OFF   # builds the head only
cmake --build build -j --target i4-head

ssh -N -L 7400:127.0.0.1:7400 you@linuxbox &   # the tunnel
./build/i4-head --link tcp:127.0.0.1:7400
```

Video is decoded by VideoToolbox (the Mac's hardware decoder). `I4_HEAD_TRACE=1`
prints each frame (codec, decoder, bytes, decode time) and key->photon
latency.

## What it costs (measured, release build, kitty, 40 keys)

| path | key->photon p50 | p90 |
|---|---|---|
| local head (zero copy) | 18.4 ms | 22.3 ms |
| TCP head, GPU video | 20.9 ms | 32.1 ms |

Encode 1.5 ms (NVENC), decode ~2 ms; a keystroke's frame is ~100-400 bytes.
Add your network's one-way latency on top.
