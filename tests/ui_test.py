#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Host checks for the pixel UI: battery charge fill, charging mask, toggles and the beat light."""
import json
import os
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "tests" / "ui"
SIM = ROOT / "tools" / "sim.py"
JSON = ROOT / "assets" / "atl-atl-ui.json"


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


def bounds(name):
    j = json.loads(JSON.read_text())
    for s in j["meta"]["slices"]:
        if s["name"] == name:
            b = s["keys"][0]["bounds"]
            return b["x"], b["y"], b["w"], b["h"]
    raise SystemExit(f"missing slice: {name}")


def read_ppm(path):
    data = path.read_bytes()
    assert data[:2] == b"P6"
    parts, i = [], 2
    while len(parts) < 3:
        while i < len(data) and data[i] in b" \t\r\n":
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i] not in b"\r\n":
                i += 1
            continue
        j = i
        while j < len(data) and data[j] not in b" \t\r\n":
            j += 1
        parts.append(int(data[i:j]))
        i = j
    while data[i] in b" \t\r\n":
        i += 1
    w, h, _ = parts
    return w, h, data[i:i + w * h * 3]


def pixel(img, x, y):
    w, _, data = img
    at = (y * w + x) * 3
    return data[at], data[at + 1], data[at + 2]


def differs(img, base, box):
    x0, y0, x1, y1 = box
    return any(pixel(img, x, y) != base.getpixel((x, y)) for y in range(y0, y1) for x in range(x0, x1))


def black(img, x0, y0, x1, y1):
    return sum(pixel(img, x, y) == (0, 0, 0) for y in range(y0, y1) for x in range(x0, x1))


def green(img, x0, y0, x1, y1):
    return any((lambda p: p[1] > p[0] + 20 and p[1] > p[2] + 20)(pixel(img, x, y))
               for y in range(y0, y1) for x in range(x0, x1))


def main():
    bx, by, bw, bh = bounds("battery indicator")
    battery_script = f"""master 1023
battery 0
usb 0
frames 8
screen {OUT}/battery0.ppm
battery 700
frames 4
screen {OUT}/battery100.ppm
battery 561
usb 1
frames 40
screen {OUT}/batterycharge.ppm
"""
    run("battery", battery_script)
    img = read_ppm(OUT / "battery0.ppm")
    empty = black(img, bx + 3, by + 3, bx + bw - 3, by + bh - 3)
    img = read_ppm(OUT / "battery100.ppm")
    full = black(img, bx + 3, by + 3, bx + bw - 3, by + bh - 3)
    assert empty == bh - 6, f"0% battery fill: {empty} black pixels"
    assert full == (bw - 8) * (bh - 6), f"100% battery fill: {full} black pixels"
    img = read_ppm(OUT / "batterycharge.ppm")
    assert green(img, bx, by, bx + bw, by + bh), "charging battery mask was not drawn"

    base = Image.open(ROOT / "assets" / "atl-atl-ui.png").convert("RGB")
    modes_script = f"""master 1023
battery 700
usb 0
frames 5
btn FX down
frames 1
btn FX up
btn ARP down
frames 1
btn ARP up
btn GLO down
frames 1
btn GLO up
frames 5
screen {OUT}/modes.ppm
btn REC down
frames 5
screen {OUT}/recording.ppm
"""
    run("modes", modes_script)
    img = read_ppm(OUT / "modes.ppm")
    for name in ("fx toggle", "arp toggle", "glo toggle indicator"):
        x, y, w, h = bounds(name)
        assert differs(img, base, (x, y, x + w, y + h)), f"{name} stayed on the base image"
    img = read_ppm(OUT / "recording.ppm")
    x, y, w, h = bounds("rec indicator")
    assert differs(img, base, (x, y, x + w, y + h)), "REC overlay did not appear"

    play_script = f"""master 1023
battery 700
usb 0
frames 10
usbtone 440 -10
usbrec 2
btn REC down
frames 30
btn REC up
usbtone 0
frames 4
btn SEQ down
frames 1
btn SEQ up
btn PLAY down
frames 1
btn PLAY up
frames 40
screen {OUT}/playing.ppm
"""
    run("playing", play_script)
    img = read_ppm(OUT / "playing.ppm")
    sx, sy, sw, sh = bounds("sample presence indicator")
    assert green(img, sx, sy, sx + sw, sy + sh), "sample presence did not turn green"
    px, py, pw, ph = bounds("play indicator")
    assert green(img, px, py, px + pw, py + ph), "PLAY indicator did not turn green"
    qx, qy, qw, qh = bounds("bpm blink")
    assert green(img, qx, qy, qx + qw, qy + qh), "BPM beat light did not pulse on"
    for name in ("seq toggle", "play indicator"):
        x, y, w, h = bounds(name)
        assert differs(img, base, (x, y, x + w, y + h)), f"{name} did not turn on"
    print("ui_test: ok")


if __name__ == "__main__":
    main()
