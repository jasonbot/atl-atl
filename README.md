<h1 align="center">Atl-Atl: The Atltuner</h1>

<p align="center"><b>The M-VAVE FM-1 as a USB audio sink and a key-targeted autotuner.</b></p>

Atl-Atl is its own firmware. It turns the FM-1 into a class-compliant USB audio device: the computer plays into it,
the FM-1 tunes that audio to the note you hold on its keyboard, and the result comes out of the speaker or
headphones. The four knobs shape the effect, **SELECT** sets the tempo, and the screen shows the held note, the
detected pitch, the sample and the settings. A three-second sampler records the USB input while the tuner keeps
playing; its loop snaps to the note length nearest the sample at the current tempo.

There is **one app**: ATLTUNER. No module picker, no OPTIONS page, no BLE-MIDI, no packaged synth. The firmware is a
single-purpose build on the FM1_AudioBase platform, with its hardware-tested USB audio sink. The tuner's DSP and UI
were developed first in an fm1-dock module and carried over. It is **not** fm1-dock: Atl-Atl has its own name, its own
identity (`FM-1_922`) and its own UI. The upstream authors are credited below; nothing here is endorsed by them.

Source: [github.com/jasonbot/atl-atl](https://github.com/jasonbot/atl-atl).

> **Status:** builds, passes the host simulator, and has been installed and updated on a real FM-1 here. The USB
> identity and audio path come up. The USB sink is FM1_AudioBase's hardware-tested path; the autotuner is still a
> work in progress, so treat the first listen as a bench test.

## What it does

```
computer / DAW
   │  USB audio OUT: 48 kHz, 16-bit stereo, isochronous adaptive
   ▼
jitter ring → cubic resampler (48,000 → 48,076.9 Hz) → ATLTUNER → MASTER → I2S DAC
                                                          ▲
                                      held key = target note, four knobs = parameters

FM-1 output ──► USB audio IN: the computer can record what the speaker plays
```

- **USB audio sink**: the FM-1 appears as a USB audio device. FM1_AudioBase's `usb_play_*` path receives the host's
  stream on EP3, buffers it and resamples it to the codec's own clock. The ring is primed at a fixed latency,
  re-primed after a stall, and the ratio follows a smoothed fill, so packet jitter does not modulate the pitch.
- **Key-targeted tuning**: a held key is the target note. The input is analyzed for its fundamental, the target is
  folded to the octave nearest the singer, and a phase-synchronous granular shifter moves the input to the target.
  Releasing the keys returns to the dry signal unless **FX** latches the note.
- **Three-second sampler**: hold **REC** to capture the USB input unaltered (mono, at the codec rate) while the
  live tuner keeps playing. **PLAY** plays it once; **SEQ** loops it. **GLO** selects play-note mode: every physical
  or MIDI key press restarts the sample, retuned to that key, as a one-shot. GLO and SEQ are exclusive. The recorded
  note is the sample's root. The loop length is the note length nearest the sample's own length at the current tempo
  (whole, half, quarter, eighth, sixteenth, and thirds/ninths), so it is clipped or followed by silence to land on the
  note grid. It mixes with the live input when both play.
- **Tempo and mask**: **SELECT** sets 30–300 BPM; USB-MIDI clock overrides it while clocks arrive. **ARP** masks
  the output so sound only passes while a key is held. **FX** latches the target, which makes the mask always on.
- **The four knobs** set speed, mix, correction and octave. **EDIT** switches auto-octave folding; **SAVE** stores
  the settings.
- **No module system**: the app starts directly; HOME does not open a picker, and there is no OPTIONS page.

## Install

Build it with the JieLi toolchain (see [BUILDING](#building)), then:

```sh
# from the stock firmware or any FM-1 firmware that answers the installer
python3 tools/fm1_install.py build/atlatl.fwsc
```

It starts **ATLTUNER** directly. Play audio from the computer; the FM-1 reports itself as
`Atl-Atl: The Atltuner` over USB.

To return to M-VAVE's firmware, use the official V15 package and its installer. Atl-Atl keeps the Felucca update
loader: an interrupted install can be resumed, and two failed boots open the USB rescue (or hold **OCT−** at
power-on).

### Releases

GitHub releases are built and published by `.github/workflows/release.yml` when a version tag is pushed. The release
contains `atlatl-X.Y.fwsc`, `SHA256SUMS` and the licences:

```sh
git tag -a v0.1 -m "Atl-Atl 0.1"
git push origin v0.1
```

A manual run of the **release** workflow builds the package as an artifact without publishing a release. The
workflow runs the host simulator and the installer tests before it creates the release.

## Controls

| Control | What it does |
| --- | --- |
| **Keys** | hold a key: its note is the target. The octave is folded to the one nearest the input; last/highest held key wins |
| **KNOB1** | SPEED: 0 is instant hard tune, 100 is a slow glide |
| **KNOB2** | MIX: dry / tuned |
| **KNOB3** | CORRECT: how much of the target ratio is applied |
| **KNOB4** | OCT: shift the key range, −2 … +2 |
| **OCT− / OCT+** | shift the key range |
| **REC** | hold to record the USB input; release to stop. Auto-stops at three seconds |
| **PLAY** | play the sample once, or stop playback; with LOOP, start or stop the loop |
| **SEQ** | ONCE / LOOP for sample playback |
| **GLO** | play-note mode: each key restarts the sample at that key's pitch; one-shot, and exclusive with LOOP |
| **ARP** | MASK: pass sound only while a key is held |
| **FX** | latch the last target after the keys are released; with MASK, the output stays on |
| **EDIT** | AUTO vs FIXED octave folding |
| **SAVE** | store speed, mix, correction, octave, latch, fold, BPM, loop, mask and play-note mode |
| **SELECT** | BPM, 30–300; USB-MIDI clock takes over while clocks arrive |

The tuner owns the keys and the four knobs directly. USB-MIDI note on/off is a second keyboard: held MIDI notes
are tracked like the keys, a release falls back to the next held note, and both sources drive the target and the ARP
mask. CC 74/71/73/72 do the same from USB.

## Building

Linux x86-64 (or Docker), Python 3 with Pillow, and the JieLi Linux toolchain. The three AC79 SDK files are in
`tools/sdk/`, so a full SDK checkout is optional:

```sh
pip3 install -r requirements.txt
tools/get_toolchain.sh                       # to ~/.jieli/toolchain
./build.sh                                   # -> build/atlatl.fwsc
```

`--fs 44` builds the 44.1 kHz codec family instead of the default 48 kHz. The app is
`firmware/apps/atlatl/`; its FPU DSP is `dsp.c` + `at_dsp.c`, and `app.c` calls it through integer-only bridges.

Outputs:

- `build/atlatl.fwsc` — the installable package, identity `FM-1_922`
- `build/app.bin` — the app image
- `build/host/out/` — the simulator's WAV and PNG

Host simulator:

```sh
python3 tools/sim.py                         # host/atlatl.sim: 220 Hz in, C4 held
python3 tests/sampler_test.py                # MIDI held keys, note 0, record trim, playback, GLO retrigger
```

## Sources and lineage

Atl-Atl stands on work by other people. The parts it uses keep their licences and notices; see
[LICENSING.md](LICENSING.md) for the file-by-file record.

- **FM1_AudioBase** by stevexfer: the single-app Felucca platform, the hardware-tested USB audio sink and the
  simulator/release tooling. Atl-Atl is a fork of it. [github.com/stevexfer/FM1_AudioBase](https://github.com/stevexfer/FM1_AudioBase)
- **fm1-dock** by Joakim Eriksson: the module prototype where the ATLTUNER DSP and UI were developed. The DSP was
  carried over; the module host was not.
- **Felucca** by Leo Kuroshita / Hügelton Instruments, through SLOOP by isod89 and Dinghy by zvenson: the FM-1
  platform, HAL, update loader and storage (GPL-3.0).
- **Autotalent / TalentedHack** by Tom Baran and Jeremy Salwen: the phase-synchronous shifter and key-as-target
  idea the DSP follows (GPL-3.0).
- **JieLi Technology**: three AC79 SDK files in `tools/sdk/`, Apache-2.0. **M-VAVE**: the FM-1 hardware. Atl-Atl is
  unofficial and not affiliated with or endorsed by M-VAVE or any upstream author.

## Limits

- **FM-1 only.** The AudioBase platform is the FM-1's; the SMK-37 port from fm1-dock is not in this tree.
- **Mono target, one note.** The detector follows a single fundamental; a full mix is not a reliable input. Solo
  voice or a monophonic instrument is the intended source.
- **No analog microphone input.** The FM-1 has no audio ADC; the input must arrive over USB.
- **Latency**: about 36 ms device-side (9.4 ms ring + 21.3 ms shifter at 48 kHz + 5.3 ms block), plus the
  computer's USB buffer. It is an effect box, not a zero-latency monitor.
- The PC's volume slider is not wired to a UAC Feature Unit; use the FM-1's **MASTER**.
- 44.1 kHz is available with `--fs 44`; 48 kHz is the tested default.

## Licence

GPL-3.0-only. You may change and share it; give whoever gets your firmware its source too. The third-party parts
and their notices are in [LICENSING.md](LICENSING.md) and `LICENSES/`.
