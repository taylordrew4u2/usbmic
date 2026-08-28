# Multi-Microphone Aggregator, Recorder, and Monitor

Cross-platform desktop application (macOS + Windows) that aggregates up to 8 USB
microphones, records every microphone to discrete files plus one summed mix
directly to an external card, and feeds one identical live low-latency monitor
mix to everyone in the room.

The full build specification lives in [`docs/SPEC.md`](docs/SPEC.md) and is the
source of truth for every constant and behavior in this codebase. Where the code
implements a spec rule, the section number is cited in a comment.

> **Read [Current status](#current-status) before running this on anything you
> care about.** The engine is complete and measured; the two platform audio
> backends have never executed against real hardware, because no build
> environment here has an audio device. That is stated precisely below rather
> than glossed.

## Download

Builds for macOS, Windows and Linux are produced by the
[Release workflow](.github/workflows/release.yml):

- **Tagged releases** — attached to the
  [Releases page](../../releases) as `MultiMicAggregator-macOS.zip`,
  `-Windows.zip` and `-Linux.zip`.
- **Any commit** — run the Release workflow from the Actions tab
  (`workflow_dispatch`) and download the artifacts it uploads. Same build, no
  tag required.

Each archive contains the application, this README, `LICENSE` and
`LICENSING.md`.

The Blue Yeti in the spec is reference hardware only: the code filters on
nothing device-specific, so any standard USB audio class microphone works.

§1 names macOS and Windows as the shipping targets. Linux now has a real ALSA
backend too, so the Linux build finds and records from microphones rather than
being a development shell — what it lacks is the §7 combined-device support,
which needs a driver on every platform but macOS.

Step-by-step setup is in [Installing](#installing) below.

### Licence

**GPLv3** — see [`LICENSE`](LICENSE). Chosen because it is the one licence that
works unconditionally with the JUCE dependency (free, no revenue limit).
[`LICENSING.md`](LICENSING.md) explains the choice and the closed-source
alternative JUCE's paid tiers would allow.

## What to expect on your platform

This is a v0.2.0 release. The recording engine is mature — it is covered by 241
automated tests plus three harnesses that run the real capture path and verify
the resulting audio files, all on every commit. What differs by platform is how
much of the *device* layer has been run against a live audio system.

| Platform | Status | What this means for you |
|---|---|---|
| **Linux** | Device layer verified in CI | Enumeration, exclusive-mode selection, capture and hot-plug all execute against a live ALSA system on every commit. Expect it to work; report anything that does not. |
| **macOS** | Device layer built, not yet run | The app, the engine and the combined-device support compile and link on macOS in CI, but the CoreAudio device code has not yet been executed on real hardware. Treat this release as a first run: it may work fully, and it may surface issues nobody has hit yet. |
| **Windows** | Device layer built, not yet run | Same position as macOS, for WASAPI. |

The reason for the split is availability, not design: the automated build
environment has a working Linux audio system but no macOS or Windows machine
with microphones attached, so those two code paths cannot yet be exercised
automatically. The interface all three share is fully validated by the Linux
implementation, so what remains unproven is platform-specific device behaviour
rather than the architecture around it.

**If you are on macOS or Windows and something misbehaves**, use *Advanced →
Export diagnostics*. It bundles the log, recent session metadata and your device
inventory — never audio — which is exactly what is needed to diagnose it.

## Installing

No installer is needed on any platform — the app is self-contained. Neither
the macOS nor the Windows build is code-signed (signing needs an Apple
Developer account and an EV certificate respectively, neither obtainable from
source code), so each OS asks for one extra confirmation on first launch.
The steps below include it.

### macOS

1. Unzip `MultiMicAggregator-macOS.zip`.
2. Drag `Multi-Mic Aggregator.app` into **Applications** (or run it from
   anywhere — location doesn't matter).
3. First launch only: **right-click the app → Open → Open**. A plain
   double-click will be refused because the build is unsigned. Terminal
   alternative:
   ```sh
   xattr -dr com.apple.quarantine "/Applications/Multi-Mic Aggregator.app"
   ```
4. macOS will ask for **microphone permission** — allow it, or every meter
   stays silent. If you declined by accident: System Settings → Privacy &
   Security → Microphone → enable Multi-Mic Aggregator.
5. Plug in your USB microphones and headphones. Monitoring is live from
   launch; there is nothing to arm.

### Windows

1. Unzip `MultiMicAggregator-Windows.zip` anywhere (e.g. a folder in
   `Program Files` or your Desktop).
2. Run `bin\Multi-Mic Aggregator.exe` from the unzipped folder. SmartScreen
   will warn once about an unrecognised app: click **More info → Run anyway**.
3. If no microphones appear: Settings → Privacy & security → Microphone →
   make sure **Let desktop apps access your microphone** is on.
4. Plug in mics and headphones; monitoring is live from launch.

### Linux

1. Unzip `MultiMicAggregator-Linux.zip`.
2. From the unzipped folder, run it:
   ```sh
   chmod +x "bin/Multi-Mic Aggregator"   # zip extraction can drop the execute bit
   "bin/Multi-Mic Aggregator"
   ```
3. Microphones are found through ALSA. If none appear, check that your user is
   in the `audio` group. For a machine with no sound hardware,
   `Tools/setup_alsa_fixture.sh` creates virtual microphones to try it with.

### Using it

- **One device for other apps (macOS)** — the app publishes a combined input
  device containing every connected microphone, created through CoreAudio's
  public aggregate-device API: no driver, no signing. It appears in every
  app's input list (Zoom, OBS, a DAW) under a name you set in **Advanced →
  Combined device name**, with one channel per mic and the same §3.1 clock
  master the app itself uses. It tracks hot-plug and is removed when the app
  quits. On Windows this needs the §7 virtual-device driver — the Advanced
  panel says so rather than pretending.

- **Tell your mics apart** — tap (or speak into) a microphone and its skull
  lights up. Click a skull to name that mic; the name sticks to the physical
  port across replug and goes into that mic's recording filename.
- **Name the take** — type into the *Session name* box before pressing record;
  the folder becomes `2026-08-27_1030_<name>`. Leaving it empty is fine.
- **Spacebar** mutes and unmutes the headphones instantly. Recording is never
  affected by muting.
- If the sound ever cuts out on its own, that is the feedback protection —
  the mute button becomes **Unmute (sound was cut)** and pressing it brings
  the sound back.
- After you stop, the screen says exactly where the take was saved.

### First run — where things go, on every platform

- **Recordings** default to a `RECORDINGS` folder in your home directory.
  Change the destination from **Advanced → Destination folder** — pointing it
  at an external card is the intended setup, and the app benchmarks a new
  destination before enabling the record button (§6.4).
- **A local backup copy** of each take is kept by default in
  `RECORDINGS-MIRROR` in your home directory, so a card failure is an
  inconvenience rather than data loss. Toggle it in the Advanced panel.
- **The log** lives at `MultiMicAggregator/log.txt` under your user
  application-data directory (`~/Library` on macOS, `%APPDATA%` on Windows,
  `~/.config` on Linux). **Export diagnostics** in the Advanced panel bundles
  it with the last five sessions' metadata — never audio.

### Uninstalling

Delete the app. The only things it leaves behind are your recordings
(`RECORDINGS`, `RECORDINGS-MIRROR`) and the log folder above — remove those
too if you want nothing left.

## Layout

```
Source/Core/        platform-independent engine logic, no JUCE dependency
Source/Platform/    audio backends (CoreAudio, WASAPI/ASIO) + virtual device backends A-D
Source/UI/          JUCE components: skull meters, main screen, advanced panel
Source/App/         composition root wiring devices + engine + monitor + UI
Tests/              headless unit tests for Source/Core
Tools/              capture harnesses: e2e_capture, soak_drift (see Building)
docs/SPEC.md        the build specification, verbatim
```

`Source/Core` deliberately has no JUCE dependency. That is what makes the engine
testable on a headless machine with no audio hardware, and it is where the
spec's hard numbers live.

## Building

**Core library and tests** (no network, no JUCE, no audio hardware required):

```sh
cmake -B build
cmake --build build -j
./build/Tests/mma_core_tests
```

**The full GUI application** (requires network access to fetch JUCE 7.0.12):

```sh
cmake -B build -DMMA_BUILD_APP=ON
cmake --build build -j
```

`MMA_BUILD_APP` is `OFF` by default so the engine and its tests build anywhere.

**Packaging a build to hand to someone:**

```sh
cmake -B build -DMMA_BUILD_APP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
cmake --install build --prefix dist --config Release   # a clean tree
cmake --build build --config Release --target package  # or a .zip
```

**The capture harnesses** (`Tools/`, built by default alongside the engine).
Neither is a unit test: they take minutes and answer questions unit tests
cannot. Both found real bugs — see [Executed, not just
compiled](#executed-not-just-compiled).

```sh
./build/e2e_capture /tmp/take   # two mics, mismatched clocks, decode the WAVs
./build/soak_drift 4.0          # §3.4: four clocks, four hours, drift at the end

./Tools/setup_alsa_fixture.sh   # Linux: virtual mics carrying known tones
./build/live_capture /tmp/live  # ...then the REAL ALSA backend, end to end
```

Linux needs JUCE's usual dependencies for the GUI build:

```sh
sudo apt-get install -y libasound2-dev libx11-dev libxext-dev libxinerama-dev \
  libxrandr-dev libxcursor-dev libxcomposite-dev libfreetype6-dev \
  libfontconfig1-dev libgl1-mesa-dev
```

## Continuous integration

`.github/workflows/ci.yml` runs two jobs on Linux, macOS and Windows for every
push to `main` and every pull request, and can be run on demand from the
Actions tab.

- **Core + tests** builds the engine and runs its unit tests. It needs no JUCE
  and no audio hardware, so it runs unchanged everywhere and guards the
  portability the platform builds depend on.
- **App** builds the full JUCE application. `CoreAudioBackend` and
  `WasapiAsioBackend` sit behind `JUCE_MAC` / `JUCE_WINDOWS` guards, so only
  the matching runner compiles each one; without this job neither is built
  anywhere.

The app job pins macOS to `macos-14`. JUCE 7.0.12 calls
`CGWindowListCreateImage`, which the macOS 15+ SDK marks unavailable, so JUCE's
own tooling fails to build on newer runners. Moving to a newer SDK means moving
to JUCE 8.

## Current status

### Implemented and verified

All of `Source/Core`, covered by 246 unit tests passing in CI on Linux, macOS
and Windows. The table below lists the largest areas rather than every file:

| Area | Spec | Tests |
|---|---|---|
| `MonitorBus` — sum, trim, brickwall limiter, runaway cut, feedback protection, master volume | §5 | 15 |
| `RecordingEngine` — mid-take unplug/reconnect/new-mic events | §6.5 | 9 |
| `PreflightThroughputTest` — rolling-minimum throughput, 2x gate, FAT32 | §6.4 | 9 |
| `SessionFolderNaming` — sanitization, truncation, collision suffixes | §6.2 | 8 |
| `DriftCompensator` — PI loop, ±200 PPM clamp, 5 PPM/s slew | §3.2 | 8 |
| `DeviceInputStream` — per-device ring, drift loop, resampler onto the master clock | §3.2, §3.3 | 8 |
| `AlsaBackend` — real Linux audio: enumeration, exclusive-mode gate, capture, inotify hotplug | §2, §5.4, §11 | live_capture |
| `DeviceManager` — 8-mic cap, 9th exclusion, master selection and failover | §1, §3.1, §3.3 | 8 |
| `RingBuffer` — lock-free SPSC, 30s / 64 MB minimum sizing | §6.3 | 7 |
| `Metering` — ballistics, peak hold, clip latch | §8.1 | 7 |
| `SessionWriter` — RIFF/WAVE headers, auto-split, periodic header rewrite | §6.1, §6.6 | 5 |
| `SampleRateNegotiator` — highest common rate capped at 48 kHz | §2.2 | 5 |
| `PolarPatternDetector` — non-cardioid detection | §14.4 | 5 |
| `ChannelLayoutAnalyzer` — mono collapse rules, 60 s timeout | §2.1 | 5 |
| `DeadChannelDetector` — silence against an active reference channel | §8.1 | 4 |
| `SessionMetadata` + JSON | §6.2 | 4 |

### Executed, not just compiled

`Tools/e2e_capture` drives the real capture path with synthetic audio — two
mics on mismatched clocks, recorded to disk, then the WAVs are decoded and
checked by Goertzel that each stem holds its own microphone's tone and the mix
holds both. `Tools/soak_drift` is the §3.4 gate above. Neither is a unit test:
they take minutes, and they answer questions unit tests cannot.

Both found real bugs that the unit-test suite did not:

- **Integral windup.** The PI loop's integral saturated at the ±200 PPM clamp
  long before the deliberately slow 5 PPM/s slew could deliver it, so every
  crossing had to unwind from saturation. Over hours the loop swung between
  +140 and −45 PPM instead of settling, starved rings, and failed §3.4 at
  3.04 ms. Integration now pauses whenever the output is rate- or clamp-limited.
- **Pre-roll sized from ring capacity.** Playout started at half the ring, which
  meant 1024 samples — **21 ms of monitor latency**, on its own more than twice
  the entire §5.4 budget. Pre-roll is now a fixed two blocks, and the monitor
  path measures 5.33 ms end to end against the 10 ms ceiling.

A third, smaller one: the output clock starts before any device has delivered,
so the first pull of every take underran. That is normal startup rather than
lost audio, and counting it made the §0.1 metric untrustworthy.

### Exercised against a real OS audio API

`Source/Platform/AlsaBackend.cpp` is a real Linux backend on ALSA, and
`Tools/live_capture` drives it: the OS enumerates the devices, ALSA opens them,
libasound delivers the audio on threads the backend creates, and the harness
checks what comes out. `Tools/setup_alsa_fixture.sh` builds file-backed virtual
microphones carrying known tones, so this runs on a machine with no sound
hardware — including in CI, on every commit.

Measured, five runs identical: each device delivers its own tone at 0.2000
magnitude with 0.0001 leakage of the other — a 2000:1 separation — and §5.4
correctly refuses a shared (`default`) output by name.

This does **not** make CoreAudio or WASAPI verified; those are different APIs.
What it retires is the broader claim that `IAudioBackend`'s contract had never
met a real audio system: it has, and it holds. Two honest limits of the fixture:
ALSA's `file` plugin delivers as fast as it is read rather than at 48 kHz, so
the timing is not real-time and the capture ring floods (which is why the
full-stack layer asserts frame-locked files and signal, not per-stem tone
coherence); and it cannot refuse a sample format, so the fixture must be
written in whatever format the backend negotiates.

### Compiled, but never exercised against hardware

The full application builds and links in CI on Linux, macOS and Windows, so
`Source/Platform` and `Source/UI` are no longer unverified code:

- `CoreAudioBackend` — macOS, guarded by `JUCE_MAC`. Accepts either buffer
  shape the HAL uses (one channel per buffer, or one interleaved buffer), so a
  stereo USB microphone records audio rather than silence; expands continuous
  sample-rate ranges during §2.2 negotiation; tolerates a device that refuses a
  rate change because it is already at that rate; and fails the monitor open,
  with a message naming the next step, when hog mode is unavailable rather than
  reporting an exclusive path it does not have.
- `WasapiAsioBackend` — Windows, guarded by `JUCE_WINDOWS`. Selects ASIO or
  WASAPI exclusive; shared-mode WASAPI is refused outright per §5.4. Exclusive
  mode performs no format conversion, so the open negotiates float32, then
  32-, 24- and 16-bit PCM, and the device's own channel count — most USB
  microphones are 16- or 24-bit devices and would otherwise refuse to open at
  all. The fixed-point conversion this requires lives in
  `Source/Core/SampleFormat.h`, away from the Windows headers, and is covered
  by unit tests that run on every platform.
- Hotplug on Windows goes through a registered `IMMNotificationClient`, the
  counterpart to the CoreAudio property listener — §2 requires the OS to tell
  us, never a timer.
- `SkullMeterComponent`, `MixBarComponent`, `MainScreen`, `AdvancedPanel`,
  `MainComponent`, `Main.cpp` — JUCE components using the §9.2 palette.

The app has also been run headless under Xvfb, where it renders the §1
zero-microphone state and survives with its 60 Hz refresh running.

What that does **not** establish is behaviour with real audio on those two
platforms: the OS calls themselves — `AudioDeviceStart`,
`IAudioClient::Initialize` in exclusive mode, the hotplug notifications above —
have not executed, because the build environment has no macOS or Windows host
with an audio device. Compiling and rendering are not the same as working.

Everything above those calls *is* established: the capture path, drift loop,
write pipeline and file format are driven with real and synthetic audio and the
results decoded and checked, and the interface those backends implement is
validated end to end by the ALSA one. The unexercised surface is two files, not
the engine behind them. See [What to expect on your
platform](#what-to-expect-on-your-platform) for what that means in practice.

### Wired but unreportable

Two inputs have no source on either platform, so the code that consumes them
is correct and permanently quiet rather than wrong:

- **Thermal throttling** (§6.6). `CpuPressureMonitor` takes it as an argument
  and acts on it; no backend reports it, so it is always passed `false`. The
  CPU-pressure half of §6.6 is live, measured from the audio callback's own
  deadline usage rather than from overall machine load.
- **USB host-controller topology** (§14.3). `ControllerContentionDetector`
  treats unknown topology as unjudgeable and stays silent, which is right —
  guessing would warn people whose card reader is fine.

### Deliberately stubbed

Virtual device backends per §7 — with one carve-out that ships: on macOS the
combined device needs no driver at all, because CoreAudio's public
`AudioHardwareCreateAggregateDevice` API publishes a system-wide aggregate
(`Source/Platform/MacSystemAggregateDevice.cpp`). Other apps see one named
multi-channel input containing every mic, with per-sub-device drift
compensation handled by the HAL. What remains stubbed is Windows, and the
§7 "virtual cable carrying the summed mix" use case; each is gated on
something that cannot be obtained from source code:

| Backend | State | Blocked on |
|---|---|---|
| A — none (standalone recorder + monitor) | Implemented (`NullBackend`) | nothing |
| B — ASIO output DLL | Interface + stub | ASIO SDK, COM registration |
| C — licensed signed virtual cable | Interface + stub | commercial per-seat license (VB-Audio / VAC / Thesycon) |
| D — own attestation-signed WDM driver | Interface + stub | registered legal entity, EV certificate, Partner Center |

### Not code, and therefore not done

Per §7 and §11, these are administrative and cannot be completed by writing
software:

- Apple Developer ID signing and notarization of the macOS build.
- Windows code-signing certificate and signed installer.
- Legal entity registration and EV certificate procurement, which §13 says
  should start on day one because backends C and D are gated on it.
- macOS `AudioServerPlugIn` bundle installation flow (the plugin itself is in
  scope for v1; the signed install is not until the certificate exists).

### Not yet validated against hardware

The entire §12 validation matrix is outstanding. Nothing here has seen a real
microphone. In particular:

- **§3.4 passes against simulated clocks, and only those.** `Tools/soak_drift`
  runs four dissimilar clocks (0 / +100 / −80 / +45 PPM) for four hours and
  measures inter-channel drift at the end: **1 sample = 0.021 ms** against the
  1 ms ceiling, zero underruns, each loop settling within 0.4 PPM of its true
  offset. Simulated offsets are steady, though; real crystals wander with
  temperature and load, so the hardware run is still owed. What this does
  retire is the question of whether the *software* holds alignment — it does,
  and it did not before the two bugs below were found.
- **§5.4 latency ceiling** — the 10 ms ceiling must be confirmed by loopback on
  macOS, Windows ASIO, and Windows WASAPI exclusive.
- Hostile-event matrix, card throughput on real slow media, bus-power
  exhaustion, and the §10.7 novice acceptance test.

## Judgment calls

- **No JUCE in `Source/Core`.** The spec does not require this, but without it
  nothing could be tested in an environment without JUCE, and §13 orders drift
  compensation first — before any UI exists to host it.
- **Hand-rolled test framework** (`Tests/TestFramework.h`) instead of Catch2,
  and a small hand-rolled JSON writer/parser (`Source/Core/Json.h`) instead of
  nlohmann. Both avoid a network fetch in the build. Either can be swapped for
  the mainstream library later; the JSON one is only used for `session.json`.
- **`MMA_BUILD_APP` defaults to `OFF`** so that `cmake -B build && cmake --build
  build` succeeds on any machine. Turn it on for real platform builds.
- Backends B/C/D return an explicit unavailable status rather than pretending to
  work, so §7's requirement that the UI names which applications can and cannot
  see the aggregate device stays truthful.

## Build order

Per §13, and where this repository sits against it:

1. Multi-device capture with drift compensation — **engine written, gated on the
   §3.4 hardware measurement**
2. Direct-to-card write pipeline with throughput benchmarking — **written and
   unit-tested, hostile-event matrix outstanding**
3. Shared monitor bus with limiter, mute, feedback protection — **written and
   unit-tested, gated on the §5.4 latency measurement**
4. Metering — **written and unit-tested**
5. Virtual device backends — **A implemented and compiled on all three platforms, B/C/D stubbed per above**
6. UI and zero-knowledge setup flow — **builds in CI on all three platforms and runs headless; never used with real microphones**
