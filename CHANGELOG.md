# Changelog

## Unreleased

### Fixed -- a failed backup drive stopped the mirror in silence

§6.3 has two ways a mirror can stop mid-take, and only one of them was ever
reported.

- **A failed mirror write said nothing at all.** `WritePipeline` has always
  detected it, stopped mirroring, and correctly left the card write running --
  §6.3's "the mirror must never take the recording down with it". But
  `hasMirrorWriteFailed()` had no callers outside its own tests, so nothing
  ever *noticed*. Pulling the backup drive mid-take produced no message and no
  line in `session.json`, because only the low-space stop was recorded. The
  user was left with a truncated backup copy that looked exactly like a
  complete one. `MirrorState` gains `StoppedWriteFailed` so the two reasons
  stay distinguishable -- "your disk is filling up" and "your backup drive is
  gone" need different words -- and both now reach the user and the record.
- **The emergency stop sat below two mirror checks.** The §6.5 card-removal
  branch is commented "checked before anything else because the take has to
  stop now", and it wasn't: the mirror checks returned ahead of it, so a mirror
  message could delay the stop by a poll. It is now where its comment says.

### Fixed -- a mid-take unplug pointed the clock master at silence

The take's channel list and the device list are two different index spaces.
Outside a recording they agree, which is what made conflating them so easy to
miss. During a take they do not: §6.5 freezes the channel list, so an unplugged
microphone keeps its slot and writes silence, while `DeviceManager` tracks what
the OS reports right now and drops anything unplugged. Five places paired an
index from one space with a lookup in the other.

- **The clock master locked onto the unplugged channel.** `applyClockMaster()`
  found its index by counting included devices, which is the right number only
  until a microphone leaves mid-take. Recording A, B and C and unplugging B
  made the device list `A, C`; asked to lock to C, the old arithmetic returned
  index 1 -- channel B, the one writing silence. Every other microphone was
  then drift-corrected against a channel carrying no audio. Resolution now
  goes by device id against the take's frozen list, and skips any candidate
  that is dead or is not in this take at all.
- **§3.3 master failover never ran.** `selectFailoverMaster` had no production
  callers -- the fifth unwired Core API in this series. Losing the master left
  the rig on whatever channel the shifted index happened to name. The
  switchover is now performed and, as §3.3 requires, logged with its timestamp
  in `session.json`.
- **Drift was attributed to the wrong microphone.** The reporting loop walked
  the device list while reading `getChannelDriftPpm(index)` from capture, so
  after an unplug the Advanced panel showed one microphone's PPM under
  another's name, and §3.3's 100 PPM "unreliable" flag could land on a device
  keeping perfect time.
- **Names, meters and the dead-channel indicator drifted apart.** The skull
  strips take their name from the device list and their level, liveness and
  dashed outline from capture. After a mid-take unplug the name and the meter
  on a strip belonged to different people, and the shorter device count meant
  the last channel's meter was never polled at all. `getIncludedMicCount()` and
  `getMicDisplayName()` now answer in the take's space while one is running,
  and an unplugged microphone's strip keeps the name it was opened with rather
  than going blank.
- **§10.5 advice addressed the wrong person.** `SetupAdvisor` is fed one peak
  per channel but was given names from the device list, so unplugging a
  microphone renamed everyone after it in every piece of advice.

### Fixed -- every microphone looked dead, and §6.5's hot-plug row said nothing

Found by sweeping `Source/Core` for public API with no production callers,
after two bugs of exactly that shape (#34, #35).

- **Every meter read `--.-` forever.** `SkullMeterComponent::setNoSignal` had no
  callers anywhere, and the flag it sets defaults to true -- and it gates both
  the dashed skull outline *and* both numeric readouts. So every microphone was
  drawn permanently dashed with a hardcoded `--.-` and `pk --.-`, whatever it
  was actually doing. §8.1 puts the meters live from launch and §9.2 asks for
  numeric readouts in a monospace face "so digits do not jitter as values
  change"; the digits never changed at all. Now dashed means what §6.5 says it
  means -- the channel is not delivering audio, either unbound or writing
  silence because its microphone was unplugged mid-take -- and a connected
  microphone in a quiet room shows its real level, which is the difference
  between "nobody is talking" and "this is not working".
- **A microphone plugged in mid-take said nothing.** §6.5 requires one line:
  "Mic added to monitoring. It'll be recorded starting with your next take."
  `RecordingEngine` has contained that sentence, verbatim, the whole time --
  nothing had ever asked it for one. So plugging a microphone in during a
  recording was completely silent, leaving the user to assume it was being
  captured.
- **An unplug mid-take was never logged.** §6.5 says "log the dropout";
  `RecordingEngine::onMicUnplugged` existed to record exactly that and was never
  called, so a microphone could fall out of a four-hour take and leave no trace
  in `session.json`. Reconnections were equally unrecorded. Both are now logged
  against the device they happened to.

The audio itself was never at risk here: an unplugged channel has always kept
its slot and written silence (§6.5), which is the part that protects the take.
What was missing was every way the user or the record would have known.


### Fixed -- §6.5's back-pressure row was policy nothing ever called

- **The ring-buffer warnings never fired.** `CapacityMonitor::evaluateFill` has
  always held §6.5's thresholds -- warn at 50% fill, fall back to mix-only at
  90% when there is no mirror -- and has always had tests. Nothing in the app
  ever called it. Neither did anything call `noteDegradationAt`, and
  `CaptureCoordinator::getRingFillFraction` was exposed and read by nobody. So
  a drive falling behind produced no warning at all, and §0.1's "never silently
  drop" described precisely what happened.
- **Now the fill is polled during a take** and the warning is shown, ranked
  above the remaining-time warnings: a ring filling up is audio about to be
  lost now, where running low on room is audio that will stop being recorded
  later.
- **At 90% with no mirror, the stems stop and the mix keeps going.** A complete
  mix is worth more than eight stems with the same hole in them, and the ring
  drains at a fraction of the byte rate while it recovers. The mix is still
  summed from every channel while degraded -- the point is to shed write
  bandwidth, not to drop anyone out of the recording that survives.
- **It never un-degrades within a take**, for the same reason the mirror never
  restarts (§6.3): stems that resume mid-file are worse than stems that
  stopped, because the gap is invisible in the waveform.
- **§6.5: "log the exact sample position of degradation."** It goes into
  `session.json` as a dropout entry, which is what it is -- the moment the
  stems stopped receiving audio they should have had.


### Fixed -- a pulled card no longer fails silently

- **Every failed write was being thrown away.** `SessionWriter::writeInterleaved`
  returns false on an unrecoverable write and its own header says the caller
  handles that per §6.5 "target card removed". `WritePipeline` was that caller,
  and it discarded all four of those return values -- the stems, the mix, and
  both mirror copies. So pulling the card mid-take wrote into the void: no stop,
  no finalize, no alert, the elapsed time still climbing and the screen still
  claiming to be recording. This is the data-loss case in the §12 hostile-event
  matrix and it was unhandled.
- **Now the take stops, finalizes and says so.** §6.5 in full: stop immediately,
  close every open file, alert loudly, and -- when the mirror was still running
  -- state that a complete copy survives and give its path. The panel that
  appears at the end of a take carries the alert and lists what actually
  survived, pointed at the mirror rather than the folder that is no longer
  there.
- **A mirror that fails never takes the recording with it.** §6.3 makes the
  mirror a safety net, so a failed mirror write stops the mirror for the rest of
  the take, exactly as running out of internal room does, and the card write
  continues untouched. The mirror exists to turn a card failure into an
  inconvenience; it must not become one.
- **A mirror with a hole in it is never offered as a copy.** If the mirror had
  already stopped earlier in the take, the alert does not name it -- a user told
  a complete copy survives will trust it.
- **Proven against a real failing write.** The process's maximum file size is
  capped so the writer's own file grows into a hard EFBIG, which is what a
  departed card looks like from inside `write()`, with SIGXFSZ ignored so the
  failure arrives as the return value the pipeline has to notice. Paired with a
  healthy take that must keep the flag down, so the test cannot pass by always
  reporting failure.


### Fixed -- the card-speed gate now counts the video, and is answered fresh

- **A camera could push a card past what it can sustain, and nothing checked.**
  §6.4 blocks arming unless the destination sustains twice what the take needs,
  but that figure counted the audio only. Eight microphones at 24-bit/48k need
  about 4.6 MB/s and one camera at its best quality can ask for as much again,
  so a card could pass the gate and then fail once a camera started -- the exact
  mid-take degradation §6.4 exists to refuse in advance. The video is now part
  of the required rate, added once rather than doubled, since the x2 covers the
  stems plus the mix and there is only ever one copy of the video.
- **The verdict is no longer frozen at benchmark time.** How fast the card is
  belongs to the card; how fast it needs to be belongs to the take, and the take
  changes whenever a microphone or a camera is switched on. The 200 MB
  measurement is still cached per volume and never re-run needlessly, but the
  gate is applied against the rig as it stands at the moment someone reaches for
  record. This also closes the same staleness for microphone count, which was
  there before cameras existed: the stored pass/fail was computed with whatever
  channel count happened to be live when the benchmark last ran.
- **§10.6: the way out is named.** When the cameras are what pushed a card over,
  the message says so -- and when the card was too slow for the audio alone, it
  does not, because turning the cameras off would not save it.


### Added -- the app remembers your rig

- **Setting up happens once.** Microphone names and trims, which microphones are
  switched off, where recordings go, the backup setting, the combined-device
  name, the cameras and their names, and the answer to the "where does this go"
  card are all still there next launch. §2.4 already carried a name and a trim
  across a replug, and PortIdentityStore said in as many words that writing them
  to disk was the App layer's job -- which the App layer had never done, so
  every one of them was carried faithfully across an unplug and lost completely
  on quit.
- **Keyed by port, not by name.** Four identical microphones share a product
  string (§14.6); switching one off by name would have switched off its three
  siblings on the next launch.
- **A settings file is never worth failing to launch over.** One that is
  corrupt, truncated by a power cut, or written by a newer version loads as
  defaults, and a file from an older version keeps every key it does recognise
  rather than being dropped whole.
- **A remembered destination has to still be there.** A card unplugged since
  last time falls back to the default rather than leaving the app pointed at a
  path that no longer exists.

### Added -- §6.6 crash recovery

- **A take the app was killed in the middle of is handed back.** On launch the
  destination and the mirror are checked for sessions with no stop timestamp;
  their file headers are repaired from the audio actually on disk, and what was
  found is presented before the main screen with a button that opens the folder.
- **This collects a guarantee that was already being paid for.** SessionWriter
  has always rewritten each file's RIFF and data sizes every five seconds
  precisely so an interrupted file stays playable -- and nothing had ever gone
  looking for those files afterwards. The audio was on the card, under a header
  describing a file up to five seconds shorter than it really was, and the app
  came up as though nothing had happened.
- **Under a second is reported, not offered.** §6.6 calls such a file an
  unplayable stub. It is excluded from what is presented, named in the count so
  nobody is left wondering where a file went, and left on disk rather than
  deleted -- see *Judgment calls*.
- **A take where nothing survived is not mentioned at all**, and neither is a
  clean shutdown, which is nearly every launch.

### Changed -- cameras say what they will write

- Each camera row now shows the file it will produce (`Writes
  V01_Kitchen-Cam.mov`), updating as it is renamed. Renaming is the moment
  someone wants to see what the name does.
- The Cameras panel's way back moved to the top left, where the one in Settings
  already was. Two doors off the main screen that close in different corners is
  two things to learn instead of one.


### Added -- you are told where the files go before there are any, and shown them after

- **A card before your first take asks where it's going.** Two questions --
  what to call this recording, and where to put it -- with the exact folder
  that will be created shown underneath, updating as you type the name. It
  lists what will be in the folder, says where the backup copy goes, and
  offers a button to pick somewhere else. §6.2 calls a novice losing track of
  their recording a total product failure, and the cheapest place to prevent
  that is before there is anything to lose.
- **It is asked once.** The answer is remembered against the folder it was
  given about, so every later press of record starts immediately, exactly as
  §10.4 requires. It is asked again only when the destination moves somewhere
  the user has not agreed to, or when they tick *Ask me this before every
  recording*.
- **The files can be watched appearing.** While a take runs, the main screen
  shows that take's own folder rather than its parent, and a live count and
  total size read off the disk -- not inferred from the channel count, so what
  is on screen is what is on the card.
- **Stopping produces the files, not a sentence.** §6.2 has always asked for
  "show the location and offer to open the containing folder"; until now that
  was a status line that said "Saved to ..." for ten seconds and offered
  nothing. It is now a panel naming every file that was written with its size,
  the backup copy's location, and an **Open the folder** button. A take whose
  files came out empty says so, and says to check the mute switches -- §10.5's
  single most common failure.

### Added -- cameras

- **Any camera the OS lists can be recorded**, from a new **Cameras** door on
  the main screen: USB webcam, built-in camera, a capture card presenting an
  HDMI feed. The app does not vet where the picture comes from, the same way
  §2 does not vet a microphone's.
- **Live from the moment the panel opens.** Framing is something you fix before
  a take, and a picture you cannot see until you press record is a picture you
  aim afterwards.
- **Recording is always at the camera's best quality, and nothing on screen can
  change that.** The preview toggle decides how large the picture is *drawn*,
  so the live view can be cheap without ever costing the file quality -- §6.6
  warns about spending CPU before it starts dropping audio, and a 4K frame
  redrawn to check someone is in shot is exactly that spend.
- **Picture and sound are separate files.** Each camera writes one file into the
  same session folder as the audio, under §6.2's naming rules, with no sound
  track of its own -- the sound is the microphone tracks beside it. `session.json`
  records the pairing and states that the video carries no audio, so an editor
  reading the folder later is told rather than having to find out.
- **The remaining-time figure counts the video.** "Room for 8h" with two cameras
  running would have been out by an order of magnitude, and §6.5's whole point
  is that a novice cannot act on a surprise part-way through a take.
- **Nothing is opened until asked for.** No camera is opened at launch, so no
  camera light comes on and no privacy prompt is spent before the user has
  opened the panel or armed a take with a camera switched on.
- **The camera path compiles everywhere.** JUCE implements camera capture on
  macOS and Windows only, so on any other machine it would have been unverified
  by construction -- which is how CoreAudioBackend and WasapiAsioBackend came to
  be carrying five defects each. `Simulation/Camera` supplies a stand-in
  `juce_video`, and `sim_camera` compiles the controller unmodified against it,
  so a wrong signature fails the build on Linux too.

### Fixed

- **The elapsed time was set on a label with no bounds.** The main screen splits
  its status row in two while recording and leaves it single when not, but the
  flag that chose between the two layouts was never assigned, and changing state
  never re-laid the screen out -- so "Recording for 4m 12s" was written every
  tick to a label nobody could see.


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
