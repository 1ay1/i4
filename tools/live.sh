#!/bin/sh
# tools/live.sh — run a live i4 session for a bounded time, then tear it down.
#
#   tools/live.sh [-b debug|release] [-s SECONDS] [-e VAR=VAL]... -- CLIENT [ARGS...]
#
# Starts i4d and i4-head on private sockets, runs CLIENT against them, and
# GUARANTEES to return within SECONDS (default 12) plus a 2 s teardown:
#   * every process is its own session (setsid), stdin from /dev/null, all
#     output to files: nothing can hold this shell's pipes open
#   * every wait is bounded (timeout / a counted poll), never a bare `wait`
#   * teardown kills whole process groups, TERM then KILL
# Logs: /tmp/i4-live/{i4d,head,client}.log; the client gets $I4_LINK too.
set -u
build=debug secs=12 envs=""
while [ $# -gt 0 ]; do
    case "$1" in
        -b) build=$2; shift 2 ;;
        -s) secs=$2; shift 2 ;;
        -e) envs="$envs $2"; shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done
root=$(cd "$(dirname "$0")/.." && pwd)
bin=$root/build/$build
out=/tmp/i4-live
mkdir -p $out
rm -f $out/*.log
sock=wayland-i4live-$$
link=$XDG_RUNTIME_DIR/i4-live-$$
pids=""

start() {  # start NAME CMD...: own session, no tty, logged
    name=$1; shift
    setsid sh -c 'exec "$@"' sh "$@" </dev/null >$out/$name.log 2>&1 &
    pids="$pids $!"
}

teardown() {
    for p in $pids; do kill -TERM -- -$p 2>/dev/null; done
    i=0; while [ $i -lt 20 ]; do
        alive=0; for p in $pids; do kill -0 $p 2>/dev/null && alive=1; done
        [ $alive = 0 ] && break; sleep 0.1; i=$((i + 1))
    done
    for p in $pids; do kill -KILL -- -$p 2>/dev/null; done
    rm -f $link $XDG_RUNTIME_DIR/$sock $XDG_RUNTIME_DIR/$sock.lock
}
trap teardown EXIT INT TERM

start i4d timeout $((secs + 2)) $bin/i4d --socket $sock --link $link
i=0; until [ -S $link ] || [ $i -ge 50 ]; do sleep 0.05; i=$((i + 1)); done
[ -S $link ] || { echo "live: i4d did not start"; cat $out/i4d.log; exit 1; }

start head env $envs I4_HEAD_TRACE=1 timeout $((secs + 1)) $bin/i4-head --link $link
i=0; until grep -q 'GPU ready' $out/head.log 2>/dev/null || [ $i -ge 60 ]; do sleep 0.05; i=$((i + 1)); done

[ $# -gt 0 ] && start client env WAYLAND_DISPLAY=$sock I4_LINK=$link timeout $secs "$@"
# Wait for the client (bounded by its own timeout), not for the servers.
set -- $pids
client=${3:-}
i=0; while [ -n "$client" ] && kill -0 $client 2>/dev/null && [ $i -lt $((secs * 20)) ]; do sleep 0.05; i=$((i + 1)); done
exit 0
