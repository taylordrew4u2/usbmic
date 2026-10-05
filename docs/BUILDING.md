# Building and testing SobStage

Source layout, build options, the capture and simulation harnesses, and what CI runs. Back to the [README](../README.md).

## Layout

```
Source/Core/        platform-independent engine logic, no JUCE dependency
                    (including settings persistence and §6.6 crash recovery)
Source/Platform/    CoreAudio, WASAPI and ALSA backends + isolated post-v1 stubs
Source/UI/          JUCE components: channel meters, main screen, settings and
                    camera panels, the save-location and saved-take cards
Source/App/         composition root wiring devices + engine + monitor + UI
Tests/              headless unit tests for Source/Core
Tools/              capture harnesses: e2e_capture, soak_drift, sim_* (see Building)
Simulation/         stand-in CoreAudio and WASAPI headers + virtual device layers,
                    so the macOS and Windows backends can be executed anywhere,
                    plus a stand-in juce_video so the camera path compiles on
                    a machine that has no camera API at all
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
compiled](VERIFICATION.md#executed-not-just-compiled).

```sh
./build/e2e_capture /tmp/take   # two mics, mismatched clocks, decode the WAVs
./build/soak_drift 4.0          # §3.4: four clocks, four hours, drift at the end

./Tools/setup_alsa_fixture.sh   # Linux: virtual mics carrying known tones
./build/live_capture /tmp/live  # ...then the REAL ALSA backend, end to end
```

**The platform simulations** run the macOS and Windows backends — unmodified —
against stand-in OS headers, so they execute on any machine rather than only on
the one OS that can compile them natively. `ctest` runs both, so they are
covered by the ordinary test command too.

```sh
./build/sim_coreaudio           # interleaved buffers, rate ranges, hog mode, hotplug
./build/sim_wasapi              # exclusive-mode negotiation, PCM conversion, threading
./build/sim_mix_bus             # both limiters: 8 mics at full scale, trims, NaN/inf
```

**The UI walk** drives the real app through every screen it has. A test build
(`-DMMA_ALLOW_TEST_INPUTS=ON`) walks its own live window: Settings, Help and
Cameras; every picker, tick box and slider moved and put back; a microphone
renamed; diagnostics exported; a take recorded through a mid-take buffer
change. It then kills the app mid-take and checks the next launch offers the
take back, and kills a microphone mid-take and checks the alert card.

```sh
./Tools/e2e_ui_walk.sh          # Linux, Xvfb; about five minutes
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
