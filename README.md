<p align="center">
  <img src="docs/images/app-icon.png" alt="SobStage icon: a sobbing face on slate with a single cyan tear" width="128">
</p>

<h1 align="center">SobStage</h1>

<p align="center"><strong>Record up to eight USB microphones as sample-aligned multitrack, and give everyone in the room the same live headphone mix.</strong></p>

<p align="center">
  <a href="https://github.com/taylordrew4u2/usbmic/actions/workflows/ci.yml"><img src="https://github.com/taylordrew4u2/usbmic/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/taylordrew4u2/usbmic/releases/latest"><img src="https://img.shields.io/github/v/release/taylordrew4u2/usbmic?label=release" alt="Latest release"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue" alt="C++17">
  <img src="https://img.shields.io/badge/JUCE-7.0.12-8a2be2" alt="JUCE 7.0.12">
  <img src="https://img.shields.io/badge/platforms-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey" alt="Platforms">
  <img src="https://img.shields.io/badge/license-GPLv3-blue" alt="GPLv3">
</p>

<p align="center">
  <a href="docs/images/demo.mp4"><img src="docs/images/demo.gif" alt="A 20-second screen recording: two microphone meters moving, Start recording pressed, the take clock counting while the file count grows, then Stop and a Saved card listing every file with its size" width="660"></a>
</p>

## Overview

SobStage is a desktop recorder for podcasts, panels and live sessions where
several people each have their own USB microphone. It aggregates up to eight
external microphones (or the individual inputs of a multi-channel interface),
writes one 24-bit WAV per person plus a summed mix straight to an external
card, and plays one low-latency monitor mix back to every headphone jack.

The hard part is that eight USB microphones are eight independent crystal
oscillators. Over a four-hour take their clocks drift apart, and a multitrack
whose tracks do not line up is just eight files. SobStage resamples every
stream continuously against a reference clock inside a real-time callback that
may not allocate, lock or log, while also writing to disk and keeping monitor
latency inside a 10 ms budget.

**Measured result:** at most 0.042 ms of inter-channel drift across four-hour
simulated soaks at 44.1 and 48 kHz, against a 1 ms ceiling.

## Key features

- **Multitrack from mixed hardware.** One file per input, not per device, so a
  four-input interface yields four tracks. Stereo USB mics are collapsed to
  mono only after analysis proves both sides identical, and that verdict is
  remembered per physical port.
- **Shared live monitor mix** with per-channel trim, a brickwall limiter,
  runaway-level cut and feedback protection. On macOS it plays out of every
  microphone's own headphone jack at once.
- **Never loses audio silently.** Dropped samples, unplugged mics, a slow or
  departed card and low space are announced immediately, with the take time
  they happened at. A pre-flight benchmark refuses to arm a card that cannot
  sustain twice the required throughput.
- **Crash recovery.** After a kill mid-take, the next launch finds the
  unfinished session, repairs the WAV headers from the audio on disk and offers
  it back.
- **Loudness guidance** from a from-scratch ITU-R BS.1770-4 meter, with
  platform targets (Spotify, Apple Podcasts, EBU R128) corrected for mono
  delivery. It advises; it never changes the stems.
- **Camera capture** on macOS and Windows: one video file per camera beside the
  audio, with an optional lossless remux of picture and mix into one file.
- **Combined input device on macOS** via CoreAudio's public aggregate-device
  API, so Zoom, OBS or a DAW see every mic as one multichannel input.

<p align="center">
  <img src="docs/images/main-screen.png" alt="The main screen: a channel strip per microphone, a summed mix bar, a session name field, the record button, and monitor controls" width="430">
  <img src="docs/images/mid-take-alert.png" alt="A mid-take alert card listing that a microphone stopped sending sound, a camera went away, and a microphone came back, each with its time into the take" width="430">
</p>

More screenshots and a walkthrough of every screen are in the
[user guide](docs/USER-GUIDE.md).

## Tech stack

| | |
|---|---|
| Language | C++17 (Objective-C++ for the macOS movie combiner) |
| Framework | [JUCE](https://juce.com) 7.0.12, pinned by commit and fetched by CMake; only the UI and app layers depend on it |
| Audio backends | CoreAudio (macOS), WASAPI exclusive mode (Windows), ALSA (Linux) |
| Build | CMake 3.22+, CPack packaging |
| Testing | Dependency-free test framework, CTest, ASan / UBSan / TSan, Xvfb-driven end-to-end runs |
| CI / release | GitHub Actions on Linux, macOS and Windows; DMG and ZIP packaging with SHA-256 sums and a source bundle |

## Engineering highlights

- **Clock-drift compensation.** Asynchronous sample-rate conversion driven by
  a PI loop on ring-buffer fill error, clamped to ±200 PPM and slewed at
  5 PPM/s so corrections are inaudible. The reference device is resampled too.
  Long-running harnesses found and fixed integral windup and a block-delivery
  limit cycle that unit tests could not see.
- **Real-time safety.** No allocation, locking, logging or file I/O on any
  audio thread. Cross-thread handoff uses lock-free SPSC ring buffers with
  acquire/release publication, sized for at least 30 s of audio.
- **Loudness metering to the standard.** K-weighting, 400 ms blocks at 75%
  overlap, absolute and relative gating, and true peak via 4x oversampling,
  checked against the standard's reference signals at 44.1, 48 and 96 kHz.
- **Testing platform code anywhere.** The CoreAudio and WASAPI backends are
  compiled *unmodified* against stand-in OS headers and driven by simulated
  device layers covering buffer layouts, sample formats, exclusive-mode
  refusals and hot-plug. Re-introducing nine known defects turned every one of
  them red. The camera path is simulated the same way.
- **Engine isolated from the framework.** `Source/Core` has no JUCE
  dependency, so the whole engine builds and tests on a headless machine with
  no audio hardware and no network.
- **Fail-closed device policy.** Only removable USB, FireWire and Thunderbolt
  inputs are admitted on each OS; built-in, Bluetooth, virtual and unknown
  inputs are excluded rather than guessed at.

Every constant and behavior traces to the build specification in
[`docs/SPEC.md`](docs/SPEC.md); the code cites the section it implements.

## Project status

The current source is the **v1.13.17 release candidate**. The engine, the
simulated backends and the end-to-end app runs pass in CI on all three
platforms. Real-driver timing and a completed take from physical microphones
are still release gates: a PUPGSIS T12S interface has been detected on a real
Mac, but the full hardware matrix has not yet passed. Linux has a working ALSA
backend and is an early-use build. See [verification status](docs/VERIFICATION.md)
and [`RELEASE_CHECKLIST.md`](RELEASE_CHECKLIST.md) for exactly what has and has
not been proven.

## Getting started

Download the latest build from the
[Releases page](https://github.com/taylordrew4u2/usbmic/releases/latest).
Current releases are not yet code-signed; [docs/INSTALLING.md](docs/INSTALLING.md)
covers checksum verification, the one-time Gatekeeper step on macOS,
SmartScreen on Windows, and runtime packages on Linux.

### Building from source

The engine and its tests need only CMake and a C++17 compiler: no JUCE, no
network and no audio hardware.

```sh
cmake -B build
cmake --build build -j
./build/Tests/mma_core_tests
```

The full GUI application fetches JUCE 7.0.12 at configure time:

```sh
cmake -B build -DMMA_BUILD_APP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

| Platform | Requirements |
|---|---|
| macOS | Xcode command-line tools. CI builds on `macos-14`, because JUCE 7.0.12 does not build against the macOS 15 SDK. |
| Windows | Visual Studio (MSVC) with the C++ desktop workload. |
| Linux | `libasound2-dev libx11-dev libxext-dev libxinerama-dev libxrandr-dev libxcursor-dev libxcomposite-dev libfreetype6-dev libfontconfig1-dev libgl1-mesa-dev` |

Packaging, build options and every harness are documented in
[docs/BUILDING.md](docs/BUILDING.md).

## Testing

```sh
ctest --test-dir build --output-on-failure
```

- **704 unit tests** covering the engine: drift loop, ring buffers, monitor
  bus, metering, loudness, session writer, crash recovery and more.
- **Platform simulators:** `sim_coreaudio`, `sim_wasapi`, `sim_camera` and
  `sim_mix_bus` run the shipping code against virtual devices; the audio
  simulators and unit tests also run under ASan, UBSan and TSan in CI.
- **Capture harnesses:** `e2e_capture` records two mics on mismatched clocks and
  decodes the WAVs; `soak_drift` runs the four-hour drift gate; `live_capture`
  drives the real ALSA backend against virtual microphones carrying known tones.
- **End-to-end app runs:** scripts in `Tools/` drive the real application under
  Xvfb through a full take, a refused take, a disk-full take, a mid-take crash
  and recovery, and every screen of the UI.

## Project structure

```
Source/Core/       platform-independent engine (no JUCE): drift, buffers, mixing, metering, writing, recovery
Source/Platform/   CoreAudio, WASAPI and ALSA backends; macOS aggregate device; OS integrations
Source/UI/         JUCE components: meters, main screen, settings, help, cameras, alert cards
Source/App/        composition root wiring devices, engine, monitor and UI
Tests/             unit tests for Source/Core
Tools/             capture harnesses, simulators, end-to-end scripts, release tooling
Simulation/        stand-in CoreAudio, WASAPI and camera APIs with virtual device layers
docs/              specification, guides and verification record
```

## Documentation

- [User guide](docs/USER-GUIDE.md): screens, device-to-track mapping, loudness, cameras
- [Installing](docs/INSTALLING.md): downloads, per-platform setup, file locations, troubleshooting
- [Building and testing](docs/BUILDING.md): build options, harnesses, CI
- [Verification status](docs/VERIFICATION.md): what is proven and how, design decisions
- [Specification](docs/SPEC.md) · [Changelog](CHANGELOG.md) · [Support](SUPPORT.md) · [Privacy](PRIVACY.md)

## License

Released under the **GNU General Public License v3**; see [`LICENSE`](LICENSE).
JUCE 7 is used under its open-source terms. [`LICENSING.md`](LICENSING.md)
explains the distribution obligations and links the official JUCE terms.

## Author

Taylor Drew Kozero ([@taylordrew4u2](https://github.com/taylordrew4u2))
