#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Host checks for the sampler and MIDI-key input. Run after ./build.sh:
   tests/sampler_test.py
   SIM_NO_SANITIZE=1 tests/sampler_test.py   # when ASan is not installed locally
"""
import math
import os
import struct
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "tests" / "sampler"
SIM = ROOT / "tools" / "sim.py"


def run(name, script):
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / f"{name}.sim"
    path.write_text(script)
    cmd = [sys.executable, str(SIM)]
    if os.environ.get("SIM_NO_SANITIZE"):
        cmd.append("--no-sanitize")
    cmd.append(str(path))
    r = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"simulator failed: {name}")


def read_wav(path):
    with wave.open(str(path), "rb") as w:
        assert w.getnchannels() == 2 and w.getsampwidth() == 2
        sr, n = w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    pcm = struct.unpack("<%dh" % (len(raw) // 2), raw)
    return sr, pcm[0::2]                      # left channel


def tail(x, sr, seconds):
    return x[-int(sr * seconds):]


def tone(x, sr, hz):
    re = im = 0.0
    for i, v in enumerate(x):
        a = 2.0 * math.pi * hz * i / sr
        re += v * math.cos(a)
        im += v * math.sin(a)
    return math.hypot(re, im) / max(1, len(x))


def peak(x):
    return max(abs(v) for v in x)


def main():
    wav = OUT
    midi_script = f"""master 1023
frames 10
usbtone 220 -6
usbrec 2
btn ARP down
frames 1
btn ARP up
frames 2
midi 0x90 60 100
midi 0x90 64 100
frames 8
midi 0x80 64 0
frames 25
wav {wav}/fallback.wav
midi 0x80 60 0
key 7 down
frames 8
midi 0x90 64 100
frames 8
midi 0x80 64 0
frames 25
wav {wav}/key.wav
key 7 up
frames 5
midi 0x90 0 100
frames 25
wav {wav}/note0.wav
"""
    run("midi", midi_script)
    for name in ("fallback", "key", "note0"):
        sr, x = read_wav(wav / f"{name}.wav")
        x = tail(x, sr, 0.20)
        wet, dry = tone(x, sr, 261.6256), tone(x, sr, 220.0)
        assert wet > dry * 2.0, f"{name}: target C4 missing (wet {wet:.0f}, dry {dry:.0f})"

    trim_script = f"""master 1023
frames 20
usbtone 0
usbrec 2
btn REC down
frames 15
usbtone 220 -6
frames 13
usbtone 0
frames 15
btn REC up
frames 4
btn PLAY down
frames 1
btn PLAY up
frames 20
wav {wav}/trim.wav
"""
    run("trim", trim_script)
    sr, x = read_wav(wav / "trim.wav")
    play = tail(x, sr, 0.30)
    assert peak(play) > 2000, "sampler did not play the recorded tone"
    assert peak(tail(x, sr, 0.08)) < 500, "leading/trailing silence was not trimmed"

    print("sampler_test: ok")


if __name__ == "__main__":
    main()
