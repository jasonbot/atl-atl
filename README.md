<h1 align="center">Atl-Atl: The Atltuner</h1>

<p align="center"><b>The M-VAVE FM-1 as a USB audio sink and a key-targeted autotuner.</b></p>

Atl-Atl is its own firmware. It turns the FM-1 into a class-compliant USB audio device: the computer plays into it,
the FM-1 tunes that audio to the note you hold on its keyboard, and the result comes out of the speaker or
headphones. The four knobs shape the effect; the screen shows the held note, the detected pitch and the settings.

It is built on the fm1-dock host code and the Felucca platform, but this firmware exposes **one thing only:
ATLTUNER**. There is no module picker, no OPTIONS page, no BLE-MIDI and no packaged synth. It is **not** fm1-dock:
Atl-Atl has its own name, its own identity (`FM-1_922`), its own autotune and its own UI. The upstream authors are
credited below; nothing here is endorsed by them.

Source: [github.com/jasonbot/atl-atl](https://github.com/jasonbot/atl-atl).

> **Status:** the USB sink, the host ABI and the ATLTUNER module build and pass the host-side tests. The
> isochronous OUT receive path follows FM1_AudioBase's hardware-tested implementation, but this exact build has
> not been run on an FM-1 here. Treat the first install as a bench test.

## What it does

```
computer / DAW
   │  USB audio OUT: 44.1 kHz, 16-bit stereo, isochronous adaptive
   ▼
jitter ring → cubic resampler (44,100 → 44,117.647 Hz) → ATLTUNER → MASTER → I2S DAC
                                                          ▲
                                      held key = target note, four knobs = parameters

FM-1 output ──► USB audio IN: the computer can record what the speaker plays
```

- **USB audio sink**: the FM-1 appears as a USB audio device. The platform receives the host's stream on EP3,
  buffers it and resamples it to the codec's own clock. The ring is primed at a fixed latency, re-primed after a
  stall, and the ratio is steered by a smoothed fill, so packet jitter does not modulate the pitch.
- **Key-targeted tuning**: a held key is the target note. The input is analyzed for its fundamental, the target
  is folded to the octave nearest the singer, and a phase-synchronous granular shifter moves the input to the
  target. Releasing the keys returns to the dry signal unless **FX** latches the note.
- **The four knobs** set speed, mix, correction and octave. **EDIT** switches auto-octave folding; **SAVE** stores
  the settings.
- **One app, no picker**: the package contains ATLTUNER and the built-in sine fallback only. It starts ATLTUNER
  directly; HOME no longer opens a module list, and there is no OPTIONS page or BLE-MIDI in this build.

## Install

Build it with the JieLi toolchain and the AC79 SDK (see [BUILDING](#building)), then:

```sh
# from the stock firmware or any FM-1 firmware that answers the installer
python3 tools/fm1_install.py build/atlatl.fwsc
```

It starts **ATLTUNER** directly. Play audio from the computer; the FM-1 reports itself as
`Atl-Atl: The Atltuner` over USB. `build/modules/atltuner.dock` is for development and the inherited emulator
tests, not part of a normal install.

To return to M-VAVE's firmware, use the official V15 package and its installer. Atl-Atl keeps the Felucca update
loader: an interrupted install can be resumed, and two failed boots open the USB rescue (or hold **OCT−** at
power-on).

### Releases

GitHub releases are built, attested and published by
[the shared FM-1 build workflow](https://github.com/zvenson/fm1-build) when a version tag is pushed. The release
contains the FM-1 package, the three SMK-37 packages and `SHA256SUMS`:

```sh
git tag -a v0.1 -m "Atl-Atl 0.1"
git push origin v0.1
```

That builds `atlatl-0.1.fwsc`, `atlatl-0.1-smk37.fwsc`, `-smk37-elite` and `-smk37-mke`, attests them with
GitHub's build provenance and creates the release for the tag. A manual run of the **release** workflow builds the
packages as artifacts without publishing a release; a pull request builds without publishing too.

Verify a downloaded release, if you have the GitHub CLI:

```sh
gh attestation verify atlatl-0.1.fwsc -R jasonbot/atl-atl
```

The workflow uses `zvenson/fm1-build@v1`, so the Firmware Hub can show it as **Built from source**. Tag only a
committed tree: `--release` names the packages, but the source hash in `ABOUT` is the commit the tag points at.

## Controls

| Control | What it does |
| --- | --- |
| **Keys** | hold a key: its note is the target. The octave is folded to the one nearest the input; last/highest held key wins |
| **KNOB1** | SPEED: 0 is instant hard tune, 100 is a slow glide |
| **KNOB2** | MIX: dry / tuned |
| **KNOB3** | CORRECT: how much of the target ratio is applied |
| **KNOB4** | OCT: shift the key range, −2 … +2 |
| **OCT− / OCT+** | shift the key range |
| **FX** | latch the last target after the keys are released |
| **EDIT** | AUTO vs FIXED octave folding |
| **SAVE** | store speed, mix, correction, octave, latch and fold |

The tuner owns the keys and the four knobs directly; the hidden OPTIONS settings cannot take them away.

## Building

Linux x86-64 (or Docker), Python 3 with Pillow, the JieLi toolchain and three files from the JieLi AC79 SDK:

```sh
pip3 install Pillow mido python-rtmidi
tools/get_toolchain.sh                       # to ~/.jieli/toolchain
# put the AC79NN_SDK_V1.2.1 files under ~/fw-AC79_AIoT_SDK (see tools/fm1pkg_make.py)
JIELI_TOOLCHAIN=~/.jieli/toolchain AC79_SDK=~/fw-AC79_AIoT_SDK ./build.sh
```

This default is the dedicated firmware: one module, ATLTUNER, with no picker or BLE. For the old fm1-dock module
host and its tests, build `./build.sh --multi`.

Outputs:

- `build/atlatl.fwsc` — the installable package, identity `FM-1_922`
- `build/modules/atltuner.dock` — the tuner as a deployable file (development/tests)
- `build/atlatl_xip.bin` — the app image for fm1-emu / fm1-live

The SMK-37 packages are built with `./build.sh --board smk37`; they keep the same short app name and the SMK model
in their identity. See [SMK37.md](SMK37.md) for the SMK-specific first-install rules.

## Sources and lineage

Atl-Atl stands on work by other people. The parts it uses keep their licences and notices; see
[LICENSING.md](LICENSING.md) for the file-by-file record.

- **fm1-dock** by Joakim Eriksson: the host and module ABI the ATLTUNER code was developed on, the build/deploy
  tools and the platform integration. Atl-Atl is a fork of it, trimmed to one app.
  [github.com/joakimeriksson/fm1-dock](https://github.com/joakimeriksson/fm1-dock)
- **Felucca** by Leo Kuroshita / Hügelton Instruments: the FM-1 platform, HAL, USB stack, update loader, OTA and
  storage; carried through SLOOP (isod89) and Dinghy (zvenson). GPL-3.0.
- **FM1_AudioBase** by stevexfer: the hardware-tested USB audio sink (EP3 adaptive OUT, cubic reader, ring prime,
  high-water re-prime and fill smoothing). Atl-Atl backports that control loop. GPL-3.0.
  [github.com/stevexfer/FM1_AudioBase](https://github.com/stevexfer/FM1_AudioBase)
- **Autotalent / TalentedHack** by Tom Baran and Jeremy Salwen: the phase-synchronous pitch shifter and the
  key-as-target idea the ATLTUNER DSP follows. GPL-3.0.
- **JieLi Technology**: three AC79 SDK files carried in every package, Apache-2.0. **M-VAVE**: the FM-1 hardware.
  Atl-Atl is unofficial and not affiliated with or endorsed by M-VAVE or any upstream author.

The player-facing documentation for the module catalog and the shared platform remains fm1-dock's:
[the page](https://joakimeriksson.github.io/fm1-dock/) and its
[guide](https://joakimeriksson.github.io/fm1-dock/guide.html). The `docs/` directory in this repository is that
upstream documentation, retained as the module/API reference; Atl-Atl's own identity is the one above and in the
firmware's UI.

## Limits

- **44.1 kHz only** on the USB sink. The FM-1 codec runs at 44,117.647 Hz; hosts that insist on 48 kHz will not
  open the stream. Offering 48 kHz needs either a 48 kHz platform mode or a 48→44.1 stage before the tuner.
- **Mono target, one note.** The detector follows a single fundamental; a full mix is not a reliable input. Solo
  voice or a monophonic instrument is the intended source.
- **No analog microphone input.** The FM-1 has no audio ADC; the input must arrive over USB.
- **Latency**: about 38 ms device-side (9.4 ms ring + 23 ms shifter + 5.8 ms DAC block), plus the computer's USB
  buffer. It is an effect box, not a zero-latency monitor.
- The PC's volume slider is not wired to a UAC Feature Unit; use the FM-1's **MASTER**.

## Licence

GPL-3.0-only. You may change and share it; give whoever gets your firmware its source too. The third-party parts
and their notices are in [LICENSING.md](LICENSING.md) and `LICENSES/`.
