#!/bin/sh
# tools/latency.sh — key->photon into kitty, local or over TCP.
#
#   tools/latency.sh [-r PORT] [-n KEYS]
#
# Starts a bounded live session (tools/live.sh, release build), types KEYS
# letters into the i4 window through Hyprland, and prints p50/p90 of the
# head's key->photon samples (key received by the head -> presented frame
# showing kitty's answer). Needs a Hyprland session (hyprctl).
set -u
port="" n=40
while [ $# -gt 0 ]; do
    case "$1" in
        -r) port=$2; shift 2 ;;
        -n) n=$2; shift 2 ;;
        *) break ;;
    esac
done
root=$(cd "$(dirname "$0")/.." && pwd)
export HYPRLAND_INSTANCE_SIGNATURE=$(ls -t /run/user/$(id -u)/hypr/ | head -1)
secs=$((n / 5 + 10))
r=""; [ -n "$port" ] && r="-r $port"
setsid "$root/tools/live.sh" -b release -s $secs $r -- kitty -o cursor_blink_interval=0 sh -c cat \
    </dev/null >/tmp/i4-live-latency.log 2>&1 &
i=0; until grep -qE ' frame ' /tmp/i4-live/head.log 2>/dev/null || [ $i -ge 100 ]; do sleep 0.1; i=$((i + 1)); done
sleep 1
k=0
while [ $k -lt $n ]; do
    c=$(printf "\\$(printf %o $((97 + k % 26)))")
    hyprctl eval "hl.dispatch(hl.dsp.send_shortcut({ mods = \"\", key = \"$c\", window = \"title:^(i4)\$\" }))" >/dev/null
    sleep 0.1
    k=$((k + 1))
done
sleep 0.5
grep 'key->photon [0-9]' /tmp/i4-live/head.log | awk '{print $3}' | sort -n |
    awk -v how="${port:+tcp}" '{a[NR]=$1} END{if (NR==0) {print "no samples"; exit}
        printf "%s key->photon  n=%d  min %.1f  p50 %.1f  p90 %.1f  max %.1f ms\n", (how==""?"local":how), NR, a[1], a[int(NR*0.5)+1], a[int(NR*0.9)+1], a[NR]}'
grep 'remote frame' /tmp/i4-live/head.log | awk '{s+=$(NF-3); t+=$(NF-1); n++} END{if (n) printf "remote frames: %d, mean %.0f bytes, decode %.2f ms\n", n, s/n, t/n}'
