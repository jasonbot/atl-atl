# Picking up Atl-Atl

This is the short handoff for an agent starting on Atl-Atl: what the project is, what is already working, where the
sharp edges are, and which commands not to improvise.

## Snapshot

- Repository: `jasonbot/atl-atl` (fork of FM1_AudioBase).
- Branch: `main`. Last code commit: `b076818` (“Match the new UI slices and enlarge the sample length”); this
  document may be a later docs-only commit.
- The working tree was clean before this file was added.
- Latest release: **v0.8**, tag points at `41be8a3` (“Redraw the UI from the pixel assets”).
- The code is **after v0.8**; there is no release containing the latest UI bounding boxes / sample-length change.
  The local FM-1 was last flashed from the `b076818` build, not from the v0.8 release asset.
- Firmware identity is `FM-1_922`.
- Default build is 48 kHz; `./build.sh --fs 44` builds the 44.1 kHz codec family.
- Live site: <https://www.jasonscheirer.com/atl-atl/>. The emulator is
  <https://www.jasonscheirer.com/atl-atl/emu/>. It currently publishes the **v0.8** package, not `b076818`.

## What this is

Atl-Atl is a single-purpose FM-1 firmware:

- class-compliant USB audio sink (computer → FM-1 audio),
- key-targeted phase-vocoder autotuner,
- three-second sampler with root-note playback, loops, and GLO play-note mode,
- USB audio return to the computer,
- USB-MIDI in/out and a small MIDI-clock/tempo section,
- the new 240×240 pixel UI.

The FM-1 has no analog audio input. The tuner input is always the USB audio stream.

The app is `firmware/apps/atlatl/`. `app.c` is the main/UI layer; `dsp.c` and `at_dsp.c` are the FPU DSP and its
integer-only bridge. Do not add floating point to `app.c`: the unity build compiles it without the FPU. The FPU
files are built separately and exposed through `atlatl_dsp.h`.

## Start here

```sh
git status
git log --oneline -8
cat BUILDING.md
cat docs/DEVELOPING.md
```

Build and test:

```sh
./build.sh                    # 48 kHz, -> build/atlatl.fwsc
./build.sh --fs 44            # 44.1 kHz, -> build/atlatl-44k.fwsc
SIM_NO_SANITIZE=1 python3 tests/sampler_test.py
SIM_NO_SANITIZE=1 python3 tests/ui_test.py
python3 tools/sim.py --test   # flash-storage test
```

`SIM_NO_SANITIZE=1` is needed on the current development machine because ASan is not installed there. CI runs the
tests with ASan; keep them passing under both.

The simulator (`tools/sim.py`) compiles the app for the host and can produce WAV and PNG output:

```text
frames N | key K down|up | btn NAME down|up | knob NAME STEPS
master 0..1023 | battery RAW | usb 0|1 | midi STATUS D1 D2
usbtone HZ [DBFS] | usbrec 0|1|2 | screen PATH.ppm | wav PATH.wav | leds
```

Hardware:

```sh
python3 tools/fm1_install.py --info
python3 tools/fm1_install.py build/atlatl.fwsc --yes   # flash the current build
```

## Current behavior

### Tuner

- A held physical key or MIDI note is the target. The target is folded to the octave nearest the detected voice.
- The shifter is integer-only in `app.c`; the actual DSP is `at_dsp.c`.
- FX latches the target; ARP masks output while a key is held.
- `SAVE` is supposed to store speed, mix, correction, octave, latch, fold, BPM, loop, mask and GLO. **See known
  issues: saving currently does not load back.**

### Sampler

- REC captures the USB input before tuning, mono at codec rate, max three seconds.
- The recorded fundamental becomes the sample root.
- PLAY plays once; SEQ loops; GLO is one-shot play-note mode.
- **GLO and SEQ are mutually exclusive.** Turning one on turns the other off.
- In GLO mode, every physical/MIDI note-on restarts the sample and latches that note’s pitch. Releasing the key does
  not reset the sample pitch. `note_step` exists specifically so the audio ISR uses the triggered pitch rather than
  the live target after a note-off.
- The loop note length is the nearest division of a whole note at the current tempo: `1/1`, `1/2`, `1/3`, `1/4`,
  `1/8`, `1/9`, `1/16`.

### UI

The current UI is the user’s pixel design, not the old SVG:

- `assets/atl-atl-ui.png` — at-rest base.
- `assets/atl-atl-ui-on.png` — active versions of toggles/indicators at the same coordinates.
- `assets/atl-atl-ui.json` — Aseprite slice boxes. This is the source of truth for coordinates and labels.
- `tools/gen_ui.py` — quantizes the base and emits `build/gen/fw_ui.h` with macros and RGB565 on-slices.
- `firmware/sys/gfx.c` — `ui_draw_bg`, `ui_pixels`, `ui_bg_rect`, `ui_slice`, `ui_text`.
- `firmware/apps/atlatl/app.c` — `draw_*` functions consume the generated `FW_UI_*` macros.

Do not edit `build/gen/fw_ui.h`; rebuild from the assets. If a bounding box changes, edit the JSON and rerun the
build. The build passes the canonical assets directly; there is no per-app `ui.png` copy anymore.

UI rules already implemented:

- Text is centered in the supplied box; `FONT_L` is used for the selected key and BPM, `FONT_S` for everything else.
- Active text is `#000000`; inactive text is `#cbdbfc`.
- On-slices are drawn for latch, mask, PLAY, LOOP, REC, GLO, sample presence, USB and charging.
- The BPM blink sprite pulses with the beat.
- The sample length is large `FONT_L` with a small `s` suffix, because `0.4s` does not fit the 54 px box at 28 px.
  Empty state remains the small `EMPTY` placeholder.
- Battery fill is inset 3 px left / 5 px right. 0% is the leftmost pixel column; 100% is the full inset width.
  Battery percentage uses the stock ADC thresholds 531/591 (`BATT_EMPTY`/`BATT_FULL` in `app.c`).
- The charging battery slice is shown when `fw_stats.usb_up` is set. That is USB enumeration, not a dedicated charger
  detect: a dumb charger that does not enumerate may not light the charging border.

`tests/ui_test.py` checks the battery 0/100 fill, charging mask, mode overlays, sample presence, PLAY/SEQ and BPM
pulse. A screenshot suitable for docs is generated from the simulator and stored as `docs/card.png`.

## USB / endpoint map

The current app build (`firmware/apps/atlatl/build.txt`) enables:

- `FW_USB_PLAY=1`
- `FELUCCA_UAC=1`
- no `FELUCCA_CDC`

Endpoints:

| EP | Use |
|---|---|
| EP0 | Control |
| EP1 | USB-MIDI |
| EP2 | Free while CDC is off |
| EP3 | USB audio playback (OUT) |
| EP4 | USB audio record (IN) |

The controller exposes EP0–EP4. If CDC is ever enabled, EP2 and EP3 are used by CDC and there is no free HID endpoint
without dropping something.

## Known issues and sharp edges

1. **SAVE does not restore settings.** `load_cfg` in `firmware/apps/atlatl/app.c` checks
   `fw_store_load(...) == 0`. `fw_store_load` returns the number of bytes read (or `-1`), so a valid 40-byte config
   is rejected. This was discovered during review but is still present. It also means the advertised persisted GLO
   mode does not survive a reboot. A fix needs to accept the byte count, zero-initialize the struct, and decide
   whether to migrate the old v2 36-byte record. Add a save/load round-trip test before calling it fixed.
2. **Config version reset.** The struct gained `note` and `CFG_VER` moved to 3. If `load_cfg` is fixed later, decide
   explicitly whether old v2 records are migrated or intentionally reset; do not silently claim persistence.
3. **The site is in a separate repository.** The live URL is served by `jasonbot/jasonbot.github.io` on `master`,
   under `atl-atl/`; the publication commit was `ff544bb`. The `pages.yml` workflow in `atl-atl` is not what serves
   this URL and its deploy job has been failing. The previous fix copied a generated site into the personal-site
   repo:
   `python3 web/make_site.py --release build/release-0.8`, then copy `build/site/*` to `atl-atl/` there.
   The live site currently exposes v0.8, not HEAD. If you change web content, republish deliberately and check:
   `/atl-atl/`, `/atl-atl/emu/firmwares.json`, `/atl-atl/firmware/manifest.json`, `.fwsc`, and the SHA-256.
4. **No release for current HEAD.** The latest UI work is committed but not released; do not assume the website or
   Hub has it. Neither can include HEAD until a new release is published.
5. **Charging is a proxy.** There is no exposed charger-detect signal in the app API; the green battery mask uses
   `fw_stats.usb_up`.
6. **USB HID keyboard is viable but not implemented.** EP2 is free in the current CDC-off build. A standard boot
   keyboard report (modifiers + reserved + six usages), a HID interface and an EP2 interrupt IN endpoint would fit.
   The FM-1 has 27 keys, so full QWERTY needs layers/modifiers; 6KRO is the compatible choice, not NKRO. The FM-1 is
   a USB device, not a USB host: it can type into a computer, but cannot accept a USB keyboard itself.
7. **Do not add floating point to `app.c`.** It is built without the FPU; the build will fail if soft-float is linked.
8. **Do not test only with the sanitizer disabled if you touch memory/layout.** CI uses ASan; run the CI path when
   available. Local no-sanitize is a fallback, not proof of memory safety.
9. **Keep the UI assets canonical.** The old `assets/ui.svg` and per-app `firmware/apps/atlatl/ui.png` were removed.
   Adding one back creates a second source of truth.
10. **Composite USB order matters.** The existing comments in `usb.c` call out macOS dropping MIDI when audio
    interfaces are ordered badly. Adding HID or audio changes should be tested on Windows, macOS and Linux.

## Release procedure

Releases are tag-driven:

```sh
git tag -a vX.Y -m "Atl-Atl X.Y"
git push origin vX.Y
```

The release workflow builds and tests the package, creates the GitHub release, and starts `pages.yml` (which has not
been a reliable publication path for the personal site). For the personal site, build the site and publish it
separately as described above.

A local release package can be made without a tag:

```sh
python3 tools/release.py X.Y
```

Do not use a tag or create a release unless the user asks; the current HEAD intentionally has no release.

## If you implement HID keyboard

Start from the existing descriptors in `firmware/sys/usb.c` rather than writing a second USB stack:

- Add a HID interface after the existing MIDI/audio interfaces.
- Class 3, subclass 1, protocol 1 for boot keyboard.
- EP2 IN (`0x82`), 8-byte report, 1–10 ms interval.
- HID descriptor, report descriptor, and GET_DESCRIPTOR/SET_IDLE handling in `ep0_service`.
- A pending report buffer serviced from `usb_poll` (do not send from `app_audio`).
- App-side held-key state, modifiers, layers, and key-up reports.
- Test on a real host; macOS may cache descriptors, so bump `bcdDevice`.
- If CDC is enabled, resolve the EP2 conflict first.

The simplest good design is a boot-protocol US QWERTY keyboard, 6KRO, with buttons providing Shift/Ctrl/Alt/layer
changes.

## Useful files

```text
firmware/apps/atlatl/app.c        modes, sampler, UI, MIDI/key handling
firmware/apps/atlatl/dsp.c        FPU DSP bridge
firmware/apps/atlatl/at_dsp.c     tuner/shifter implementation
firmware/apps/atlatl/build.txt    USB feature flags
firmware/sys/usb.c                descriptors and USB device stack
firmware/sys/gfx.c                UI blits and text
firmware/sys/fw.h                 app/input API
tools/gen_ui.py                   pixel UI generator
assets/atl-atl-ui.json            UI bounding boxes
tests/sampler_test.py             sampler/GLO/audio regression test
tests/ui_test.py                  pixel-UI regression test
web/make_site.py                  installer/emulator site builder
```

When in doubt, trust the simulator output and the live hardware over assumptions. Reproduce audio issues with
`tests/sampler_test.py`; reproduce UI issues with `tests/ui_test.py` and a PNG screenshot.
