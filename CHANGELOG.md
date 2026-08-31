# Changelog

## Unreleased

### Changed — a cooler palette, and one place it lives

- **The colours are cool rather than warm.** A slate ground with cyan doing the
  acting, in place of the sepia and amber, which read as a leather-bound thing
  rather than as an instrument. The meters keep green through yellow to red:
  that is the convention every level meter uses, and the palette is not worth
  relearning a meter for.
- **A warning colour distinct from the accent.** Amber for anything that needs
  attention -- a monitor problem, the reason a record button will not go.
  Painted in the accent, a problem said "press me" in the colour of the thing
  that had just gone wrong.
- **The palette lives in one place.** The meters, the mix bar and the main
  screen each carried their own copy of the same hex values, so a recolour
  meant editing four files and finding out from a screenshot which one had
  been missed.

### Changed — the icon and the disk image follow the app

- **The app icon is recoloured**, and now carries the accent the way the meters
  do: bone skull on slate, filled from the jaw up to a level. A skull is a
  skull; a skull holding a level is a level meter, and that is what tells you
  at 32px what the app is for.
- **The disk-image window** matches, and its drag arrow is the accent rather
  than the faintest mark in the window -- it is the entire instruction that
  window exists to give.

### Fixed — the first-launch instruction, in the two places a Mac user reads it

The disk-image panel and the README's install steps both led with
*"Control-click the app, then choose Open."* That is the workaround for an
app with **no** signature. This build is ad-hoc signed, and Control-click →
Open does not clear the quarantine flag on a build with no Developer ID --
which is exactly the state someone seeing *"is damaged"* is in. So the first
thing a Mac user read was the one instruction that could not work, while the
Troubleshooting section further down said so. Both now lead with the
`xattr -dr com.apple.quarantine` command, which is what actually works.

### Changed — polish on both screens

- **Channel strips are rounded cards with air around them.** Each strip painted
  itself to its own edges, so a row of them shared hard seams and read as one
  striped slab rather than as separate channels. The skull outline is lighter,
  and the peak-hold bar is inset to the skull's width instead of running edge
  to edge, where it read as a rule drawn across the card.
- **The mix bar** puts its name on the left and its number on the right, both
  inside the track's padding. Justified into the corner, the label sat on the
  fill and became unreadable the moment the mix got loud.
- **Settings has four sections** -- where recordings go, recording format,
  microphones, monitoring and output -- each a heading over a hairline. It was
  one flat list of fifteen rows. Labels and values are two tones rather than
  one, and the local-backup toggle moves up beside the other storage choices.
- **The reason a record button is disabled only takes a row when there is one.**
  Reserved unconditionally it left a hole under the button whenever recording
  was possible, which is almost always.

## v0.5.0 — 2026-08-29

### Changed — one visual language, and controls where they are looked for

- **The app has a look of its own.** Every control was previously a JUCE
  default in JUCE blue. A single `AppLookAndFeel` now carries the palette and
  draws the buttons, sliders, scrollbars and tick boxes: flat, rounded, hairline
  outlines on a dark ground. The record button is the one saturated element on
  the screen and turns red while recording, so the state is never carried by
  colour alone.
- **The window opens at the height the screen needs.** It opened at 480 pixels
  while the main screen needed 528, which put the monitor volume, the mute
  button and the only way into Settings below the fold behind a scrollbar. Both
  screens still scroll on a display too short for them.
- **"Advanced" is now "Settings".** It is the one door out of the main screen
  and it holds ordinary choices — which microphones to record, where the files
  go. "Advanced" read as a warning to stay out of it.
- **Where recordings go is the first thing in Settings**, directly under Done.
  Picking an SD card before a take is what most people open the panel to do,
  and it sat at the bottom, below four read-only format rows, the microphone
  list and the drift report.
- **The monitor slider says what it is.** Its value box read a bare `70`; it
  now reads `Monitor 70`, naming the control in the space it already had.
- `Tools/screenshot_app.sh` renders the app headless and captures it, so a
  layout change can be looked at rather than reasoned about. `MMA_CLICK=x,y`
  clicks before capturing, which is how the Settings screen gets captured.

### Added — from the first working session

- **Choose which microphones to record.** Settings now lists every microphone
  the OS reports, with a checkbox each. Previously everything enumerated was
  recorded, which on a Mac meant the machine's own microphone was always a
  track. A deselected mic also gives its slot back, so turning off one you are
  not using makes room for a ninth you are.
- **The clock master is explained**, in the terms that matter to someone
  recording: each USB mic runs on its own crystal, no two tick at quite the same
  rate, one is the reference and the rest are nudged to match it, and that is
  what keeps a long take aligned.
- **Channel-strip meters.** The meters were short wide boxes stretched to fill
  the window, so two mics rendered as two enormous panels and eight as slivers.
  They are now fixed-width vertical strips laid side by side like a mixing desk,
  with the level, peak and name stacked tight beneath each meter.

### Fixed — four bugs from the first session with real microphones

The first time this app met physical hardware it produced four faults. None had
been caught by any test or by the CoreAudio simulation, because all of them
depend on how a real OS enumerates real devices.

- **Each microphone appeared several times** — one Yeti was listed five times.
  The device-change handler called `addDevice` for every enumerated device on
  every notification, and `addDevice` appended with no duplicate check. macOS
  fires that listener several times while a USB microphone initialises, so each
  firing re-added everything already present. Enumeration is now reconciled:
  new devices added, departed ones dropped, and devices that never left keep
  their enumeration order, drift history and inclusion untouched.
- **The clock master was always the computer.** Before any drift measurement
  exists, master selection fell back to enumeration order — and CoreAudio
  enumerates the machine's built-in microphone first, so it won on every Mac
  however many USB mics were attached. §3.1 now prefers a device the user
  plugged in, falling back to the built-in only when it is the only one.
- **Microphones were not recognised promptly.** The same defect as the first:
  the notification did arrive, and each arrival grew the list rather than
  reflecting it.
- **Settings had no way back.** Opening the Advanced panel hides the main
  screen, and the button that opens it lives on that screen, so nothing on
  display could return. The panel now has a Done button, laid out first so a
  long device list cannot push it out of view.

Deduplication keys on device identity, never on name: two Yetis report the same
product name and must remain two devices.

## v0.4.0 — 2026-08-28

### Fixed — five defects on the audio thread

None were caught by the previous 246-test suite. Each fix ships with a
regression test confirmed to fail against the old code.

- **Underrun counts were inflated roughly 30x.** When a device's ring buffer ran
  dry mid-block, `DeviceInputStream::pull` left the resampler's inner loop but
  let the outer loop continue, re-entering the failure path once per remaining
  sample. A single starved 64-sample block was reported as ~2,000 lost samples.
  §0.1 makes any non-zero underrun the one failure the user is shown, so this
  was a false alarm about the app's central promise.
- **The monitor's runaway cut fired on unrelated bursts.** `kLimiterReleaseSeconds`
  was declared and never used, so an engagement took as long to unwind as it
  took to build and carried its credit into the next one. Two 300 ms bursts
  10 ms apart — neither close to the 500 ms threshold — combined past it and cut
  the monitor mid-take.
- **Use-after-free between the audio callback and `stopRecording`.** The callback
  read a `std::unique_ptr` while another thread moved and then destroyed it. The
  audio thread now reads an atomic pointer published with release, and teardown
  waits for in-flight callbacks.
- **Three `std::pow` calls per sample in the callback**, from operands that never
  change. All are now computed once.
- **The ring buffer copied one sample at a time** — 384,000 atomic release-stores
  a second at 8 microphones. Both read and write now move data in two `memcpy`
  chunks.

### Added — installable rather than merely built

- **macOS `.dmg`**, ad-hoc signed and drag-to-install. The first attempt shipped a
  bundle Gatekeeper refused as *"is damaged"*: `install(DIRECTORY)` and `cp -R`
  both discard the ad-hoc signature Apple Silicon requires. Packaging now
  re-signs after the install, stages with `ditto`, and verifies the signature
  inside the mounted image.
- **An application icon**, generated by `Tools/make_icon.py` from the app's own
  §9.2 palette. CI fails the build if `Icon.icns` is absent from the bundle.
- **A laid-out disk-image window** — the app, an arrow, and the Applications
  folder, with the first-launch step written on the background art.

## v0.3.0 and earlier

`v0.1.0`, `v0.2.0`, `v0.2.1` and `v0.3.0` were cut before this changelog
existed. See the [Releases page](../../releases) for what each contained.

## Known limitations

- The macOS build is **not signed with an Apple Developer ID and not notarized**.
  That requires a paid Apple account and a certificate, which cannot be produced
  from source. Clearing the quarantine flag after download remains necessary;
  see Troubleshooting in the README.
- The §12 hardware validation matrix is still outstanding. v0.5.0 fixes the
  first four faults a real session found, but the matrix itself has not been
  worked through.
