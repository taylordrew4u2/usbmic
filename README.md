# Multi-Microphone Aggregator, Recorder, and Monitor

Cross-platform desktop application (macOS + Windows) that aggregates up to 8 USB
microphones, records every microphone to discrete files plus one summed mix
directly to an external card, and feeds one identical live low-latency monitor
mix to everyone in the room.

The full build specification lives in [`docs/SPEC.md`](docs/SPEC.md) and is the
source of truth for every constant and behavior in this codebase. Where the code
implements a spec rule, the section number is cited in a comment.

## Layout

```
Source/Core/        platform-independent engine logic, no JUCE dependency
Source/Platform/    audio backends (CoreAudio, WASAPI/ASIO) + virtual device backends A-D
Source/UI/          JUCE components: skull meters, main screen, advanced panel
Source/App/         composition root wiring devices + engine + monitor + UI
Tests/              headless unit tests for Source/Core
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

**The full GUI application** (requires network access to fetch JUCE 7.0.12, and
a macOS or Windows host):

```sh
cmake -B build -DMMA_BUILD_APP=ON
cmake --build build -j
```

`MMA_BUILD_APP` is `OFF` by default so the engine and its tests build anywhere.

## Current status

### Implemented and verified

All of `Source/Core`, covered by 94 passing unit tests:

| Area | Spec | Tests |
|---|---|---|
| `MonitorBus` — sum, trim, brickwall limiter, runaway cut, feedback protection | §5 | 10 |
| `RecordingEngine` — mid-take unplug/reconnect/new-mic events | §6.5 | 9 |
| `PreflightThroughputTest` — rolling-minimum throughput, 2x gate, FAT32 | §6.4 | 9 |
| `SessionFolderNaming` — sanitization, truncation, collision suffixes | §6.2 | 8 |
| `DriftCompensator` — PI loop, ±200 PPM clamp, 5 PPM/s slew | §3.2 | 8 |
| `DeviceManager` — 8-mic cap, 9th exclusion, master selection and failover | §1, §3.1, §3.3 | 8 |
| `RingBuffer` — lock-free SPSC, 30s / 64 MB minimum sizing | §6.3 | 7 |
| `Metering` — ballistics, peak hold, clip latch | §8.1 | 7 |
| `SessionWriter` — RIFF/WAVE headers, auto-split, periodic header rewrite | §6.1, §6.6 | 5 |
| `SampleRateNegotiator` — highest common rate capped at 48 kHz | §2.2 | 5 |
| `PolarPatternDetector` — non-cardioid detection | §14.4 | 5 |
| `ChannelLayoutAnalyzer` — mono collapse rules, 60 s timeout | §2.1 | 5 |
| `DeadChannelDetector` — silence against an active reference channel | §8.1 | 4 |
| `SessionMetadata` + JSON | §6.2 | 4 |

### Written but not verified here

`Source/Platform` and `Source/UI` are real code against the real APIs, but this
sandbox is Linux with no audio hardware and no JUCE, so none of it has been
compiled or run:

- `CoreAudioBackend` — macOS, guarded by `JUCE_MAC`.
- `WasapiAsioBackend` — Windows, guarded by `JUCE_WINDOWS`. Selects ASIO or
  WASAPI exclusive; shared-mode WASAPI is refused outright per §5.4.
- `SkullMeterComponent`, `MixBarComponent`, `MainScreen`, `AdvancedPanel`,
  `MainComponent`, `Main.cpp` — JUCE components using the §9.2 palette.

These need a first compile-and-run on their target OS before they can be
trusted.

### Deliberately stubbed

Virtual device backends per §7. The interface is real; the implementations
behind B, C and D are not, because each is gated on something that cannot be
obtained from source code:

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

- **§3.4 is the gate on everything else** — a 4-hour take with four dissimilar
  USB microphones finishing under 1 ms inter-channel drift. `DriftCompensator`
  implements the specified loop and passes its unit tests, but unit tests cannot
  substitute for that measurement.
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
5. Virtual device backends — **A implemented, B/C/D stubbed per above**
6. UI and zero-knowledge setup flow — **written, never compiled or run**
