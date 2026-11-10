#!/usr/bin/env python3
"""Checks the replayer against known answers. It builds tiny ProTracker modules, renders them
with tools/render.c and measures the pitch, the row timing and the effect behaviour.

    python3 tools/selftest.py
"""
import math, os, struct, subprocess, sys, tempfile, wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="mpctracker-selftest-")
RENDER = os.path.join(TMP, "render")
subprocess.run(["cc", "-O2", "-I" + os.path.join(ROOT, "src"), os.path.join(ROOT, "tools", "render.c"),
                os.path.join(ROOT, "src", "song.c"), os.path.join(ROOT, "src", "player.c"),
                "-lm", "-lpthread", "-o", RENDER], check=True)

def make_mod(cell, sample, loop=True):
    """cell(row, channel) -> (period, sample number, effect, parameter); one sample, 4 channels, 1 pattern."""
    n = len(sample) // 2
    head = b"selftest".ljust(20, b"\0") + b"s".ljust(22, b"\0") + struct.pack(">HBBHH", n, 0, 64, 0, n if loop else 1)
    head += b"".join(b"\0" * 22 + struct.pack(">HBBHH", 0, 0, 0, 0, 1) for _ in range(30))
    head += bytes([1, 127]) + bytes(128) + b"M.K."
    pat = b""
    for r in range(64):
        for c in range(4):
            per, ins, fx, prm = cell(r, c)
            pat += bytes([(ins & 0xF0) | (per >> 8), per & 255, ((ins & 15) << 4) | fx, prm])
    return head + pat + bytes(x & 255 for x in sample)

def render(mod, seconds):
    path = os.path.join(TMP, "t.mod"); wav = os.path.join(TMP, "t.wav")
    open(path, "wb").write(mod)
    subprocess.run([RENDER, path, wav, str(seconds)], check=True, capture_output=True)
    w = wave.open(wav); n = w.getnframes()
    d = struct.unpack("<%dh" % (n * 2), w.readframes(n))
    return [d[2 * i] for i in range(n)]

def freq(x, a, b):
    seg = x[int(a * 44100):int(b * 44100)]; cr = []
    for i in range(1, len(seg)):
        if seg[i - 1] < 0 <= seg[i]: cr.append(i - 1 + (-seg[i - 1]) / (seg[i] - seg[i - 1]))
    return (len(cr) - 1) / ((cr[-1] - cr[0]) / 44100) if len(cr) > 2 else 0

def peak(x, a, b): return max(abs(v) for v in x[int(a * 44100):int(b * 44100)]) / 32768

def onsets(x, thr=0.02):
    out, above = [], False
    for i, v in enumerate(x):
        a = abs(v) / 32768
        if a > thr and not above: out.append(i / 44100); above = True
        elif a < thr / 4: above = False
    return out

SQUARE = [100 if i < 16 else -100 for i in range(32)]          # 32 bytes: one cycle
HZ = lambda period: 3546895 / period / 32                      # what an Amiga plays at that period
failed = 0
def check(name, got, want, tol):
    global failed
    ok = abs(got - want) <= tol
    failed += not ok
    print(("ok    " if ok else "FAIL  ") + f"{name}: {got:.2f} (expected {want:.2f})")

for period in (856, 640, 428, 226, 113):
    x = render(make_mod(lambda r, c: (period, 1, 0, 0) if r == 0 and c == 0 else (0, 0, 0, 0), SQUARE), 2)
    check(f"pitch at period {period} (Hz)", freq(x, 0.1, 1.5), HZ(period), 0.3)

click = [100] * 40 + [0] * 24
for label, speed, tempo in (("speed 6, 125 bpm", 0, 0), ("F03 and F99", 3, 0x99), ("F06 and FA0", 6, 0xA0), ("F04 and F7D", 4, 0x7D)):
    def cell(r, c, speed=speed, tempo=tempo):
        if c == 0 and r >= 1: return (428, 1, 0, 0)
        if c == 1 and r == 0 and speed: return (0, 0, 0xF, speed)
        if c == 2 and r == 0 and tempo: return (0, 0, 0xF, tempo)
        return (0, 0, 0, 0)
    o = onsets(render(make_mod(cell, click, loop=False), 8))
    gaps = [b - a for a, b in zip(o, o[1:])][2:14]
    want = (speed or 6) * 2.5 / (tempo or 125)
    check(f"row length, {label} (ms)", sum(gaps) / len(gaps) * 1000, want * 1000, 0.1)

for name, fx, start, limit in (("2xx slide stops at C-1", 2, 428, 856), ("1xx slide stops at B-3", 1, 428, 113)):
    x = render(make_mod(lambda r, c, fx=fx, start=start: (start, 1, fx, 0x20) if r == 0 and c == 0 else (0, 0, fx, 0x20) if c == 0 else (0, 0, 0, 0), SQUARE), 7)
    check(name + " (Hz)", freq(x, 3.5, 6.5), HZ(limit), 0.3)

x = render(make_mod(lambda r, c: (428, 1, 0xA, 0x04) if r == 0 and c == 0 else (0, 0, 0xA, 0) if c == 0 and r < 12 else (0, 0, 0, 0), SQUARE), 1.6)
check("Axy with a zero parameter changes nothing", peak(x, 1.0, 1.3), peak(x, 0.12, 0.2), 0.01)

print("all checks passed" if not failed else f"{failed} check(s) failed")
sys.exit(1 if failed else 0)
