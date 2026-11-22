#!/usr/bin/env python3
"""Minimal DFPlayer Mini emulator on a pty, with scripted misbehaviour per command.

Usage: dfplayer_emu.py <pty-symlink> <program> [args...]
Starts the pty, launches the ESPHome host binary, emulates the module.
"""
import heapq
import os
import pty
import select
import subprocess
import sys
import time
import tty

LINK = sys.argv[1]
PROGRAM = sys.argv[2:]

POWER_UP_DELAY = 1.2  # module answers only this long after start
TRACK_LEN = 1.5
START_DELAY = 0.2

# (cmd, arg) -> behaviour per occurrence; default "ok"
RULES = {
    (0x0F, 0x0101): ["drop", "ok"],
    (0x0F, 0x6301): ["err:6"],
    (0x01, 0x0000): ["drop_ack_exec"],
    (0x06, 21): ["late:350"],
    (0x06, 22): ["badck", "ok"],
    (0x06, 23): ["drop"] * 10,
    (0x0F, 0x0103): ["ack_reset", "ok", "ok"],
    (0x12, 4): ["err:1", "ok"],
    (0x12, 5): ["ack_noplay"] * 10,
}
if os.environ.get("SCENARIO") == "busy":
    RULES = {
        (0x0F, 0x0201): ["drop_ack_exec"],        # ACK lost, BUSY shows play before ack_timeout
        (0x0F, 0x0202): ["drop_ack_exec_slow"],   # ACK lost, playback starts only after ack_timeout
        (0x12, 5): ["ack_noplay"] * 10,
        (0x01, 0x0000): ["drop_ack_exec"],        # non-idempotent, but BUSY proves it ran
    }
BUSY_FILE = os.environ.get("BUSY_FILE")
PLAY_CMDS = {0x01, 0x02, 0x03, 0x08, 0x0D, 0x0F, 0x12, 0x14, 0x17, 0x18}

t0 = time.monotonic()
events = []  # heap of (time, seq, fn)
seq = 0
counts = {}
st = {"up": False, "playing": False, "track": 0, "gen": 0, "volume": 30}


def now():
    return time.monotonic() - t0


def log(msg):
    print(f"[emu {now():7.3f}] {msg}", flush=True)


def at(delay, fn):
    global seq
    seq += 1
    heapq.heappush(events, (time.monotonic() + delay, seq, fn))


def frame(cmd, arg, bad_checksum=False):
    b = [0x7E, 0xFF, 0x06, cmd, 0x00, (arg >> 8) & 0xFF, arg & 0xFF]
    cs = (-sum(b[1:7])) & 0xFFFF
    if bad_checksum:
        cs ^= 0x0100
    return bytes(b + [cs >> 8, cs & 0xFF, 0xEF])


def send(cmd, arg=0, bad=False, note=""):
    if not st["up"]:
        return
    os.write(master, frame(cmd, arg, bad))
    log(f"TX {cmd:02X} {arg:04X} {note}")


def power_up():
    st["up"] = True
    set_playing(False)
    st["volume"] = 30
    send(0x3F, 0x0002, note="(online)")


def set_playing(p):
    st["playing"] = p
    if BUSY_FILE:
        with open(BUSY_FILE, "w") as f:
            f.write("1" if p else "0")
        log(f"BUSY {'LOW (playing)' if p else 'HIGH'}")


def start_track(track, delay=START_DELAY):
    st["gen"] += 1
    gen = st["gen"]

    def begin():
        if st["gen"] != gen or not st["up"]:
            return
        set_playing(True)
        st["track"] = track
        log(f"playing track {track}")

    def finish():
        if st["gen"] != gen or not st["up"] or not st["playing"]:
            return
        set_playing(False)
        send(0x3D, track, note="(finished)")
        at(0.03, lambda: send(0x3D, track, note="(finished, duplicate)"))

    at(delay, begin)
    at(delay + TRACK_LEN, finish)


def execute(cmd, arg, delay=START_DELAY):
    if cmd in PLAY_CMDS:
        start_track(arg if arg else 1, delay)
    elif cmd in (0x16, 0x0E):
        st["gen"] += 1
        set_playing(False)
    elif cmd == 0x06:
        st["volume"] = arg


def brownout():
    log("BROWN-OUT: module down for 1.5 s")
    st["up"] = False
    st["gen"] += 1
    set_playing(False)
    at(1.5, power_up)


def handle(cmd, fb, arg):
    if not st["up"]:
        log(f"RX {cmd:02X} {arg:04X} ignored (module down)")
        return
    if cmd == 0x42:
        send(0x42, 0x0200 | (1 if st["playing"] else 0), note="(status)")
        return
    key = (cmd, arg)
    n = counts.get(key, 0)
    counts[key] = n + 1
    rules = RULES.get(key, [])
    beh = rules[n] if n < len(rules) else "ok"
    log(f"RX {cmd:02X} {arg:04X} fb={fb} #{n + 1} -> {beh}")
    if beh == "ok":
        if fb:
            send(0x41, note="(ack)")
        execute(cmd, arg)
    elif beh == "drop":
        pass
    elif beh == "drop_ack_exec":
        execute(cmd, arg)
    elif beh == "drop_ack_exec_slow":
        execute(cmd, arg, delay=0.35)
    elif beh.startswith("err:"):
        send(0x40, int(beh[4:]), note="(error)")
    elif beh.startswith("late:"):
        at(int(beh[5:]) / 1000, lambda: send(0x41, note="(late ack)"))
        execute(cmd, arg)
    elif beh == "badck":
        send(0x41, bad=True, note="(ack, corrupted checksum)")
        execute(cmd, arg)
    elif beh == "ack_noplay":
        send(0x41, note="(ack, but nothing plays)")
    elif beh == "ack_reset":
        send(0x41, note="(ack)")
        at(0.05, brownout)


if BUSY_FILE:
    with open(BUSY_FILE, "w") as f:
        f.write("0")
master, slave = pty.openpty()
tty.setraw(slave)
if os.path.lexists(LINK):
    os.unlink(LINK)
os.symlink(os.ttyname(slave), LINK)

proc = subprocess.Popen(PROGRAM, stdout=sys.stdout, stderr=subprocess.STDOUT)
at(POWER_UP_DELAY, power_up)
rx = bytearray()
try:
    while proc.poll() is None:
        timeout = 0.01
        if events:
            timeout = max(0, min(timeout, events[0][0] - time.monotonic()))
        r, _, _ = select.select([master], [], [], timeout)
        if r:
            rx += os.read(master, 256)
            while len(rx) >= 10:
                i = rx.find(0x7E)
                if i < 0:
                    rx.clear()
                    break
                del rx[:i]
                if len(rx) < 10:
                    break
                f = bytes(rx[:10])
                del rx[:10]
                if f[9] != 0xEF or (sum(f[1:7]) + ((f[7] << 8) | f[8])) & 0xFFFF:
                    log(f"bad frame from ESP: {f.hex(' ')}")
                    continue
                handle(f[3], f[4], (f[5] << 8) | f[6])
        while events and events[0][0] <= time.monotonic():
            _, _, fn = heapq.heappop(events)
            fn()
finally:
    if proc.poll() is None:
        proc.kill()
