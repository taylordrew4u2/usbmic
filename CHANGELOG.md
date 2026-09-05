# Changelog

## v1.6.0 -- 2026-09-05

### Added -- choose which sockets of an interface to record

An interface is one row in Settings, with a tick box per physical input
indented under it. An eight-input box with two people on it used to record
eight files, six of them silence, and report room for a fraction of the take
it could actually hold. Untick the unused sockets and they are not recorded:
no strip, no file, no share of the disk estimate. The sockets left keep their
physical numbers, so "input 3" is still the one labelled 3 on the box.

### Added -- name each socket of an interface separately

Clicking a strip's name used to name the whole box, so both people on a
two-input interface became "Kitchen 1" and "Kitchen 2". On an interface each
socket is a person, and naming a strip now names that socket: "Alex" and
"Sam", used on the strips and in the stem filenames, with no socket number
after a name that already says who it is. A single-input microphone still
takes the name as before.

Both are port memory (§2.4): they follow the interface across a replug and a
relaunch, verified by a round-trip test.

## v1.5.1 -- 2026-09-05

### Changed -- the clock master is this computer, always; the picker is gone

Every microphone was already corrected onto the output clock -- the machine's
own. Naming one microphone as "master" changed nothing about that path; it
only moved which crystal the drift figures were quoted against, and handed the
user a picker for a choice with no audible consequence. (The picker had also
been showing nothing selected since v1.2.1, because it was filled with
per-input strip names while its selection was a device name.)

Drift is now measured against this computer, so every microphone's figure
means the same thing, no master can be unplugged mid-take, and Settings shows
"This computer" where the picker was.

### Documentation

The Settings screenshot in the README was two versions stale: it showed sample
rate, bit depth and buffer size as read-only text, and the microphone rows
without their input counts. Retaken from the current build.

## v1.5.0 -- 2026-09-05

### Added -- sample rate, bit depth and buffer size are controls, like Audio MIDI Setup

All three were read-only lines in Settings, and the app once told a user to
change one of them there. They are pickers now:

- **Sample rate** offers every rate any recorded microphone reports, plus the
  rate each is running at. A pinned rate is honoured, full stop. The earlier
  rule quietly dropped a pin the rig "could not reach" and fell back to
  automatic, which from the user's side is a control that does nothing. If the
  hardware refuses, the main screen names the device and both rates.
- **Bit depth**: 16, 24 or 32. Applies to the next take; no stream is reopened.
- **Buffer size**: Automatic (§5.4's ladder) or a fixed 64 to 1024 samples.

All three persist across relaunch, verified by a round-trip test.

## v1.4.2 -- 2026-09-05

### Verified -- the backend actually reports the rate a device is running at

v1.4.1's rule depends on one number: the rate the interface is on right now.
Nothing checked that CoreAudioBackend reports it. A field left at 0 would have
silently reverted the whole rule to highest-common -- the same way v1.4.0
shipped unable to fire. `sim_coreaudio` now drives the real backend against a
fake interface sitting at 44.1 kHz and asserts it says 44100.

### Fixed -- a key mismatch can no longer silently reselect 48 kHz

If the microphones in the take fail to match what the OS listed, the negotiator
would have been handed an empty list and returned its 48 kHz default -- the
exact failure this rule exists to end, through a side door. Every enumerated
device votes instead, and a debug build asserts.

## v1.4.1 -- 2026-09-04

### Fixed -- a microphone nobody is recording no longer decides the sample rate

v1.4.0 was supposed to keep the recording on whatever rate the interface was
already using. On the rig that reported it, nothing changed: the app still
demanded 48 kHz from hardware locked at 44.1, and still would not open.

The vote was taken over every microphone the OS lists, not the ones being
recorded. A MacBook's built-in microphone sitting at 48 kHz -- switched off, no
strip on screen, no file in the take -- outvoted the interface the recording
actually runs on. The rig "disagreed", so v1.4.0's rule fell through to
highest-common, which is exactly where it came in.

A microphone nobody is recording cannot resample, cannot drift, and cannot be
harmed by the choice. Letting it constrain the rate only ever costs the
microphones that are. Only included devices vote now.

### Fixed -- the rate a device is running at counts as a rate it supports

An interface advertising only 48 kHz while sitting at 44.1 is doing 44.1. Some
report only the rate they would prefer, and taking that list as the whole truth
ruled out the one rate guaranteed to work -- the one already running.

### The rule now lives where the tests can reach it

`SampleRateNegotiator::votingDevices()` decides who votes, in Core, tested. It
was app code, which needs JUCE and no headless test can reach -- which is how
v1.4.0 shipped a fix that could not fire on the hardware that reported the bug.
The same mistake as v1.1.0.

## v1.4.0 -- 2026-09-04

### Fixed -- the app stays on the rate your interface is already using

A PUPGSIS mixer running at 44.1 kHz, which advertises 48 kHz and then refuses
to switch to it, could not record at all:

    PUPGSIS-T12S 1 couldn't be opened for recording. This interface is running
    at 44.1 kHz and won't change to the 48 kHz this recording uses.

§2.2 chose the highest rate common to every device, which is right on paper and
worth nothing when the hardware will not move. An interface clock-locked to
44.1, or one another process has a claim on, lists 48 kHz among its
capabilities and refuses the write -- and the take never starts.

The rule now has one exception: when every device is ALREADY running at one
common rate, that rate is chosen even if a higher one is also available.
Switching is the step that fails; staying put cannot. The difference between
44.1 and 48 kHz is inaudible next to a recording that did not happen. A rig
whose devices disagree still gets highest-common, and §3 resamples whichever
cannot follow.

### Added -- Sample rate is a control in Settings, not a read-only line

The message above told people to "set the recording to 44.1 kHz in Settings".
Settings displayed the sample rate and offered no way to change it, so the one
instruction on screen named a control that did not exist.

It is now a picker, defaulting to **Automatic**, listing the rates every
microphone in the rig can actually reach. The choice is remembered, and is
ignored if the rig later cannot reach it -- a rate pinned for last week's
interface must not silently break this week's.

## v1.3.0 -- 2026-09-04

### Fixed -- a mixer that is also your headphones is opened once, not twice

A small livestream mixer presents its microphone inputs and its monitor output
as a SINGLE duplex device. SobStage opened it for output, took hog mode on it --
§5.4 requires the monitor path be exclusive -- and then opened it a second time
for input. That asks macOS for another claim on a device this process has just
taken exclusively, and the refusal arrives as:

    <mic name> couldn't be opened for recording.

against a microphone that is plugged in and working. Because every take channel
on that rig comes off the one device, the whole capture failed, which is a
silent recording from hardware with nothing wrong with it.

The device is now opened once and that single stream carries both halves of the
cycle: the microphones are read from the same callback that fills the
headphones, which is also how CoreAudio means a duplex device to be driven. A
rig where the microphones and the output are different boxes is untouched.

### Fixed -- an oversized callback no longer drops the whole block in silence

CoreAudio is allowed to hand a callback more frames than the buffer size that
was asked for, and `CoreAudioBackend` sizes its own scratch for exactly that.
The coordinator did not: one oversized slice made the entire pull return early,
so no audio, no meters and no error -- the silent failure §0.1 forbids. Its
scratch now carries the same headroom the backend gives its own.

### Documentation

A diagram of how a rig becomes tracks, covering the single-input microphone, the
multi-input interface, and the duplex mixer -- the mapping that was got wrong
repeatedly and is impossible to see from the UI alone.

## v1.2.2 -- 2026-09-04

### Fixed -- when a microphone won't open, the app says why

"PUPGSIS-T12S 1 couldn't be opened for recording." was the whole message, and
it named a microphone rather than a cause. Four different faults produced that
one sentence, and they have four different fixes:

- the interface is running at a sample rate the recording isn't using
- macOS has not granted SobStage microphone access
- another app is holding the interface
- the microphone was unplugged between being listed and being opened

The backend knew which of those it was at the moment it failed and threw the
answer away. §0.1 is about audio, but the principle is the same: the app knew
and said nothing, and the person was left to guess. The monitor-output path had
always reported its cause; the microphone path now does too, naming the rates
involved where a rate is the problem, and where to change them.

### Fixed -- a reason that doesn't fit on one line is no longer clipped

The problem line was a fixed 20px band, so a message long enough to explain
anything was cut off after a few words -- which left exactly the dead end the
message was written to end. It now grows with its text, capped at four lines so
a runaway message can never push the record button off the bottom of the
window, and `layout_probe` checks the screen still fits.

## v1.2.1 -- 2026-09-04

### Fixed -- the screen agreed with the files about how many microphones there are

v1.2.0 made a two-input interface record two tracks. It did not make the app
*look* like it had. Everything the user sees before pressing record was still
counted per device, while the recording itself was counted per input, and the
two answers disagreed:

- The main screen showed **one** meter strip for a two-input interface, and
  only grew to two at the moment recording started. Before that, the app
  looked exactly like one that could not see the second microphone -- which is
  how it was reported.
- The Settings list showed one row for the interface with nothing to say it
  carried two microphones, so the row read as the app refusing the second mic.
- §6.4's remaining-time figure was computed for the device count, so it
  promised twice the recording time a two-input interface would actually fit,
  and four times on a four-input one. The disk cannot keep that promise.

### Fixed -- an interface's microphones are told apart on screen, not just on disk

Asked for the name of a channel, the app answered per device. On a four-input
interface that gave four identically-named strips over four correctly-named
files: the meters were right, the files were right, and there was no way to
tell which meter belonged to which person.

### How this is prevented from coming back

The rule was written down twice -- once in the capture builder, per input, and
once in the screen's accessors, per device -- and the copies drifted. There is
now one description of what a rig records, `planChannels()` in Core, and every
caller resolves through it. It is Core rather than app code for the reason
`takeChannelsForDevice` was moved there: the app's builder needs JUCE and the
headless tests cannot reach it, so a rule that lives only there is a rule
nothing checks.

## v1.2.0 — 2026-09-04

### Fixed -- two microphones on a two-input interface are both recorded

A two-input interface with two people plugged into it is the commonest
small multi-mic rig there is, and this app collapsed it to a single track.

Two channels were assumed to be a stereo USB microphone presenting the same
voice on both sides, so §2.1 picked one side and discarded the other. One of
the two people was thrown away without a word -- and if the discarded side
was the one carrying the microphone that mattered, the whole take came back
silent from a rig that was working perfectly.

v1.1.0 addressed this for interfaces with more than two inputs and left
two-input devices collapsing exactly as before, which is why it changed
nothing for the rig that reported it.

§0.1 settles which way to guess when the app cannot tell the two apart.
Keeping both sides of a duplicated mono microphone costs a redundant file,
which is untidy. Collapsing two microphones into one loses somebody's audio
entirely, with nothing said. Those are not comparable, so both sides are now
kept until §2.1's analyzer has actually examined the audio and decided they
carry the same source -- a verdict §2.4 already remembers per port, so a
stereo USB mic still collapses correctly from its second take onward.

Covered in Tools/sim_capture_mac by the failure as reported: a two-input
interface where only the second person is speaking, checked to confirm both
files exist at full length and the talking one carries signal.

## v1.1.1 — 2026-09-04

### Fixed -- the interface fix in v1.1.0 never actually took effect

v1.1.0 made a device contribute one take channel per input it presents.
For anyone whose interface was already in the device list, it changed
nothing at all, and the report was exactly that: absolutely nothing
changed.

How many inputs a device has is the OS's to say, and syncToEnumeration
refreshes what the OS owns each time the device list changes -- the display
name, whether it is built in. The input count was not in that list. So a
device that arrived by any other route, or was present before the first
enumeration, kept the default of one input for the rest of the session. The
app went on believing a four-microphone interface had one microphone on it,
and the v1.1.0 rule never fired.

The count is now refreshed like every other fact the OS owns.

### Added -- the rule that decides this is testable now

The decision lived inside the app's channel builder, which needs JUCE and so
cannot be reached by the Core tests at all. That is how it shipped untested
and how the stale count went unnoticed: nothing anywhere could have caught
either.

takeChannelsForDevice() is a plain Core function now, with the reconcile
covered by a test that fails against the old code.

## v1.1.0 — 2026-09-04

### Fixed -- an audio interface's microphones are all recorded, not just one

Reported from a real rig: "it's not letting me add the mics I have
connected to my interface."

One device is not one microphone. An interface with four microphones
plugged into it is a single device presenting four inputs, and this app
took exactly one channel from any device it opened. §2.1's stereo collapse
-- written for a USB mic that presents the same voice on both sides -- was
being applied to interfaces, where the two sides are two different people.
It picked one and discarded the rest without a word. Inputs three and four
were never even read.

Anyone recording several people through one interface got one of them. If
their own microphone happened to be on a discarded input, they got a
silent recording and nothing on screen explaining why.

§2.1 had already said what should happen: collapse to mono when a side is
silent or duplicated, "otherwise record true stereo". The otherwise was
never implemented.

A device now contributes one take channel per input it presents, each with
its own strip, its own name and its own file. Two-input devices are
unchanged -- that is the USB-mic case §2.1 was written for, and the
analyzer still decides. Above two, a device is an interface and every input
is a microphone.

The streams are also grouped by device now. Opening one stream per channel
would have asked the OS for the same exclusive device once per microphone,
and on macOS the second request is refused -- the take would have died
naming a microphone that was plugged in and working.

Covered by Tools/sim_capture_mac: a four-input interface carrying a
different tone on each input, recorded through the real coordinator, with
every tone checked into its own file.

## v1.0.3 — 2026-09-04

### Added -- the macOS recording path is tested end to end

sim_coreaudio proved the backend hands over the right samples. live_capture
proved the coordinator and writer turn samples into files. Nothing joined
them, and the join is where macOS actually lives -- so three things reached
users untested:

- live_capture's fixture microphones are mono, so the stereo path, which is
  what a USB mixer or any stereo interface takes through §2.1's
  channel-layout analysis, had never reached a file in any test.
- every harness and every unit test recorded at 16 bits. The app ships 24.
- the backend had only ever been asked for audio in isolation, never while
  a take was running.

Tools/sim_capture_mac closes all three: a stereo interleaved device, at 24
bits, recorded through the real coordinator into real files, with the bytes
checked rather than the file size. It runs in CI on every platform.

### Added -- a silent take now says whose fault it is

When a take comes back with no sound, the single most useful thing to know
is whether any audio arrived. If none did, the rig is worth checking: a
mute switch, a cable, the wrong input selected. If audio DID arrive and
none of it reached the files, nothing about the rig explains it -- the
samples were here and this program lost them.

The app could not tell those apart, so it said "silent" for both, and the
only available next step was to go and check hardware that may have been
working perfectly the whole time.

The capture path now records the loudest sample that ARRIVED, alongside the
loudest the writer managed to WRITE. When audio arrived and nothing was
written, the take is reported as a fault in SobStage, in those words,
rather than as a silent microphone.

This is the §0.1 rule taken one step further. Reporting that audio was lost
is the requirement; saying which half of the system lost it is what makes
the report worth reading.

## v1.0.2 — 2026-09-04

### Fixed -- a take could record silence while the meters showed signal

Reported from a real Mac: the on-screen meters moved while the user
talked, and the files came out silent.

The capture path hands the writer whatever channels the device delivered,
clamped to the take's channel list. The writer then required that count to
match the take's exactly, and rejected the whole block when it did not --
every channel of it, including the ones that had arrived intact. If a
device simply reports fewer inputs than the take was built for, that is not
an occasional dropped block: it is the entire recording.

It was invisible on screen because metering happens after, reads the same
buffer, and has no equivalent check. So the app displayed live level for
audio it was dropping on the floor, which is §0.1's unacceptable failure at
its largest -- everything lost, nothing said.

The earlier audit found this code and fixed the wrong half of it. It saw
that a rejected block left framesDropped at zero and made it count instead,
which made the loss visible without stopping it. The remedy was wrong: §6.5
already settles what to do when a channel is not there, for the case of a
mic unplugged mid-take -- the file layout holds and that channel writes
silence. A short block is the same problem and now gets the same answer, so
every channel that arrives is written.

A block carrying MORE channels than the take is the genuinely lossy case,
since those samples have nowhere to go. Those are still counted, and the
channels that fit are still written rather than the whole block going in
the bin.

## v1.0.1 — 2026-09-04

### Fixed -- a take that records silence is no longer called saved

Reported from a real Mac: the files came out with no sound in them, and the
app said nothing about it.

The rule that decides whether a take holds audio judged it by file size
alone -- under a kilobyte per file meant nothing was written. That catches
a card pulled before any audio landed, and misses the failure people
actually hit. A device that is present and streaming digital silence -- a
microphone muted at its own switch, a dead channel on an interface, a USB
board whose audio never carried signal -- fills every stem for the full
length of the take. The files are megabytes. The rule weighed the bytes,
found plenty, and the app said "Saved." over a folder holding nothing.

That is §0.1's one unacceptable failure: audio lost without a word.

The writer now tracks the loudest sample it writes, and the verdict uses
that alongside the file sizes. Three outcomes, and the two failures are
told apart because they send you to look at different things:

- nothing arrived -- "the files are empty, no audio reached the drive"
- a stream ran and was flat -- "the files are silent, the microphones were
  connected but sent no sound"
- a real take -- "Saved to ..."

The silence bar is -90 dBFS. A 24-bit LSB sits near -138 dBFS and preamp
noise in a quiet room is far above -90, so a whisper is still a recording;
only a genuinely dead stream falls under it.

The saved-take panel is now told the verdict rather than working it out.
It can see file sizes and nothing else, so from there a silent take and a
good one are the same list of megabyte files -- and a panel reaching its
own verdict is how it once came to warn that files were empty while the
status line beside it said "Saved to ...".

## v1.0.0 — 2026-09-03

The first stable release. No code changes from v0.9.4: this marks the point
at which the camera work was confirmed on real hardware, which is what the
0.9.x line was waiting on.

### What 1.0 claims

The feature set is complete and the interface has been used on a Mac. Eight
microphones to separate tracks, a live mix bus, camera preview and capture,
combined video-and-audio output, streaming-loudness targets, and session
recovery are all implemented and covered by 375 tests plus two capture
harnesses that run on every commit.

### What 1.0 does not claim

Three things remain unproven against real hardware, and 1.0 does not pretend
otherwise -- see the platform table in README.md:

- **No recording has been made from a physical microphone.** The CoreAudio
  and WASAPI device layers are exercised every commit against simulated
  hosts that reproduce the awkward shapes real devices take, but a real
  driver's timing and firmware quirks are not something a simulation
  reproduces. Linux is the exception: it is verified against live ALSA.
- **The ffmpeg muxing has never run.** Combining picture and sound is built
  and tested as a command, not as an execution. It needs ffmpeg installed
  (on a Mac: brew install ffmpeg).
- **The loudness meter has only met synthetic signals.** It matches the
  BS.1770-4 reference tones to within 0.02 LU, which is the right check, but
  not the same as a real take.

A first stable release is a statement that the software is finished enough
to depend on, not that every path has been walked. These are the paths that
have not.

## v0.9.4 — 2026-09-03

### Fixed -- the camera picture fits the window it opens in

The picture was measured against the wrong height, and three faults came
out of that one cause.

MainScreen gives the picture whatever height is left over, and took that
from its own getHeight(). But the owner sizes MainScreen to
max(viewport, requiredHeight) -- so a taller picture grew the canvas,
which offered more spare height, which grew the picture again. It settled
at the fraction cap: 1007px of content inside the 560px window the app
opens at. The picture overflowed the window, and the record button went
below the fold.

It is now measured against the viewport -- what the user can actually see
-- which is an input from the owner rather than something derived from the
content, so the loop cannot form.

### Fixed -- switching a camera on makes room for it

That change alone left the picture small at launch (352px wide), because
the window opens sized for an audio-only rig and never grew when a camera
arrived. Switching one on now grows the window to fit it, bounded by the
display, and only ever upward: a window sized by hand is never shrunk
behind the user's back.

At the size the app opens at, the picture is now 1146x644 -- the full
width of the window.

### Fixed -- no more empty band above the footer

With the window free to grow, the opening height no longer has to reserve
space for cameras nobody switched on, so it drops from 560 to the window's
own 420 minimum. An audio-only rig had been opening with roughly 250px of
empty background between the last status line and the footer.

### Added -- the layout check now tests the size the window actually opens at

Every case in Tools/layout_probe had assumed a window someone had already
dragged bigger, which is exactly how a picture that was small on launch
went out reported as large. It now covers the real opening size and prints
OVERFLOWS WINDOW when content does not fit, so this class of fault fails
loudly instead of needing to be noticed.

## v0.9.3 — 2026-09-03

### Fixed -- the app now reports the version it actually is

`getApplicationVersion()` returned a hardcoded "0.1.0". The window title
had therefore misreported the version on every build from v0.1.0 to
v0.9.2 -- nine releases -- and the one place a user could read it was
wrong the whole time.

It now comes from the version in CMakeLists.txt, which is the single
place the number is written down, so it cannot go stale again.

### Added -- the version is on screen

Beside the tagline, at the top of the main window. "Which build am I
running" is the first question asked when a change appears not to have
arrived, and an answer that takes hunting through menus is an answer
nobody checks.

This is not hypothetical. A report that the enlarged camera picture had
not changed turned out to be an older build still installed, and nothing
in the app could have shown that.


## v0.9.2 — 2026-09-03

### Changed -- the camera picture is as big as the window will allow

The live picture topped out at 456 pixels wide in a 760-pixel window. That
was enough to tell someone was in shot and not enough to judge focus or
framing on, which is the entire reason the picture is on the main screen
rather than behind a door.

It is now sized as a share of the window rather than as a fixed number of
pixels, defaulting to the whole of the width available. So making the window
bigger makes the picture bigger -- which is what dragging a window larger was
asking for, and what the old fixed sizes answered with more empty background.

It grows in both directions. The picture is given the height left after
everything else on the screen has been laid out, so a taller window is a
taller picture, and a picture can never grow far enough to push the record
button off the bottom.

On an ordinary 1680x1050 display that is a 1223x687 picture where the old
maximum was 456x256 -- about seven times the area -- and on a larger display
it simply keeps going.

The arrows still work exactly as before, and now choose how much of the
window to spend on the shot rather than picking from a list of fixed sizes.
A size chosen before this release is kept; the untouched default is lifted,
since it was never really a choice.

### Fixed -- the window could open larger than the screen

Sizing the window to its content and centring it did not clamp to the
display, so a window taller than the screen hung off both ends of it and took
the record button with it. Harmless while the pictures were thumbnails, and
not once they were worth looking at. The window is now bounded by the display
it opens on, and the screen scrolls if its content is taller.

## v0.9.1 — 2026-09-03

### Changed -- the combined file no longer re-encodes the sound

The picture was always copied bit for bit. The sound was not: it was going out
as 256 kbps AAC, which is transparent enough for most listening and still
throws the take away. The stems are 24-bit PCM, and a lossy codec is a one-way
door -- nobody records at 24 bits in order to deliver a generation-loss copy of
it, and a combined file worse than the parts it was made from is not worth
making.

The audio is now kept as it was recorded: 24-bit PCM in a `.mov`, or FLAC in
the Matroska case, which is also lossless. Neither stream is re-encoded any
more, so the combined file is exactly as good as the stems and the video beside
it.

That changed the container on macOS from `.mp4` to `.mov`. mp4's PCM support is
an afterthought, so audio in an mp4 is lossy in practice; `.mov` carries PCM
natively, is what the camera already writes, and opens in every editor.

The audio is also written at the depth the take was actually recorded at rather
than a fixed one, so a 24-bit take does not quietly lose its bottom eight bits
on the way out.

## v0.9.0 — 2026-09-03

### Added -- aim the loudness at where the take is actually going

Every streaming service turns everything it plays to the same loudness. So
how loud a take is decides what a listener hears, and the peak meters this app
already had say nothing about it: two takes peaking at the same number can be
six decibels apart to the ear, and it is the louder one the platform turns
down.

Pick where the take is going in Settings and the app measures the mix the way
the platforms do -- ITU-R BS.1770, K-weighted and gated, the same standard they
all normalise against -- then says which way to move and by how much. Nothing
is changed for you. The stems stay at unity, as they always have.

The gating matters more than it sounds. Ungated, a recording of someone talking
with pauses measures quieter than the same voice without them, so the advice
would be "turn it up" for nothing more than leaving space to breathe. The two
gates in the standard are what stop that.

Two things the app now knows that are easy to get wrong:

**Mono needs a different number.** Every file this app writes is mono, and a
mono file played through both speakers is the same signal twice -- which
measures 3 LU louder than the single channel. Delivered at Spotify's published
-14, a mono take plays back at -11: three decibels hotter than everything
around it, every time. So the aim is -17 mono for Spotify, -19 for Apple
Podcasts, and the advice says so rather than quietly applying it.

**It will not tell you to clip.** If a take is under the target but its peaks
are already near the platform's ceiling, the suggested gain is cut to what the
ceiling allows and the app says why. Meeting a loudness figure by clipping
trades a number the platform would have fixed anyway for distortion it cannot.

True peak is measured rather than assumed from the sample peak, because a
waveform can pass between two samples higher than either -- a file that looks
like it sits at -1 dBFS can still clip a platform's decoder.

Targets are the platforms' own published figures: Spotify, YouTube, Amazon and
Tidal at -14; Apple Music and Apple Podcasts at -16; EBU R128 broadcast at -23.
Off by default -- someone recording a rehearsal is not delivering anywhere, and
a number telling them they are 8 dB under Spotify is noise.

### Changed -- the app is called SobStage

The name changes everywhere it is visible: the window, the .app bundle, the
Windows executable, the DMG, and the virtual device other apps see in their
input list. Anyone who had renamed that device keeps their own name; anyone who
had not will find "SobStage" where "Multi-Mic Aggregator" used to be, and will
need to pick it again once in Zoom, OBS or whatever else was pointed at it.

Everything the app remembers about a rig moves with it. Every microphone's name
and trim, which ones are switched off, the destination folder, the backup
setting and the camera choices all live in a folder named after the app, and a
rename that simply looked somewhere new would have presented as all of it being
forgotten -- which is exactly the thing the settings file exists to prevent. The
old folder is moved across the first time the new name is used.

The internal namespace is still `mma`. It is not visible anywhere a user can
look, and renaming it would have touched a hundred files without changing
anything.

### Added -- save the video with the sound in one file

The picture and the sound have always been written separately, for a reason
worth keeping: the platform camera capture is video-only, the microphones are
the sound, and one clean track per person is the point of the rig. But separate
files mean opening an editor before anyone can watch, send or upload a take.

Switching this on writes one more file per camera with both in it, beside the
originals rather than instead of them. The stems, the mix and the silent video
all stay exactly where they were, so a combined file that fails to appear costs
nothing that was not already saved. It is off by default: it costs disk and
minutes of processing after every take, and nobody who does not want it pays.

The sound is lined up rather than assumed to match. The stems start before any
camera is asked to record, so a camera's file begins a fraction of a second into
the take -- small enough to look like nothing, large enough to look wrong.
How far in is measured for each camera as it starts, and that much is trimmed
off the front of the audio.

The picture is copied rather than re-encoded, so the file appears in minutes
rather than hours and loses nothing. That means the container has to be one that
can carry what the camera already wrote: mp4 on macOS, Matroska on Windows,
whose format mp4 cannot legally hold.

This needs ffmpeg, which the app cannot install for anyone. If it is missing,
the toggle says so and says how to get it -- before a take rather than after
one, since finding out afterwards is too late to do anything about.

## v0.8.0 — 2026-09-01

### Added -- the camera pictures can be resized from the main screen

A fixed 176px tile was a guess, and the wrong one for half the rigs it will
meet. One camera across a table wants a picture you can judge focus on; four in
a row want to fit. Which of those someone is doing is not something the app can
work out for them, so it is a control rather than a better guess.

Two arrows above the pictures, and the up and down keys, step through five 16:9
sizes from 120 to 456 wide. The default is the old 176, so nothing moves for
anyone who does not touch it.

The arrows wrap rather than shrink: ask for a bigger picture with four cameras
and you get one, on two rows. Squeezing them all onto a single row would make
the arrows do nothing on exactly the rig that wants them. The height the screen
declares follows the wrap, so the record button stays on screen rather than
being pushed under the fold by pictures that grew without saying so.

The buttons disable at the ends of the range, and the keys work only while the
main screen is the thing on screen -- behind Settings or Cameras they would
resize something the user cannot see. A focused slider or text box consumes its
own arrows first, so this cannot steal them from the volume.

The chosen size is remembered across launches. A settings file written before
this existed has no such key and loads as the default rather than as zero, which
would have silently shrunk the pictures of everyone who upgraded.

Not visually verified: JUCE implements CameraDevice on macOS and Windows only,
so the tiles cannot be seen from the Linux build this was written on. The five
sizes are a considered first guess, not a measured one.

## v0.7.0 — 2026-08-31

### Changed -- the cameras are on the main screen now

The picture and the levels were on two different screens, so you could watch one
or the other and never both. That is the wrong way round for the moment that
matters: a microphone goes quiet while the shot still looks fine, and the only
place that shows is the meters you left behind to look at the camera.

Every camera switched on for the take now has a live tile on the main screen,
above the channel strips, captioned with its name -- four identical webcams
being the same problem §14.6 solves for microphones. A rig with no camera
enabled is laid out exactly as before and costs an audio-only user no space.

The Cameras door keeps doing what it did: switching cameras on and off, naming
them, choosing preview quality. That is settings. What moved is the watching.

One viewer per camera, enforced by ownership rather than hoped for: whichever
screen is visible owns the viewers, and the main screen releases its tiles
before either panel opens. Opening a camera the user already switched on does
not move the macOS privacy prompt -- the first grant is still spent behind the
camera door, with the reason on screen -- and a rig with nothing enabled opens
nothing and prompts for nothing.

Not visually verified: JUCE implements CameraDevice on macOS and Windows only,
so the tiles cannot be seen from the Linux build this was written on.

## v0.6.0 — 2026-08-31

### Fixed -- "Saved to ..." for a take that saved nothing

The saved-take card has always been able to tell that a finished take holds
nothing but WAV headers, and it warns when it does. The status line beside it
said `Saved to <folder>` regardless, because `SavedTakePanel::takeIsEmpty()` --
whose own comment reads *"The owner uses this to say so rather than calling it
saved"* -- had no callers. So the two disagreed on screen at the same moment,
and the status line is the one a user reads on their way out of the room.

The rule moves to `Core/TakeCompleteness` so both places reach one verdict
instead of keeping two copies that can drift, and the §10.6 notice now says the
files are empty when they are. §0.1 is about never silently dropping audio;
telling someone it was saved when it was not is the same failure wearing a
better word.

### Fixed -- two meter-strip setters that nothing ever called, and a screenshot tool that hid it

- **Every strip painted an empty row.** `SkullMeterComponent` reserves an 11px
  line under the bold microphone name and draws `deviceName` into it --
  and `setDeviceName()` had no callers anywhere, so that line was blank on every
  channel for the life of the app. It now carries the hardware's own product
  string, so a port the user named "Kitchen" shows `Kitchen` with `Blue Yeti`
  faint beneath it. Left deliberately empty when the two would be the same
  words, so an unnamed microphone does not print its product string twice.
- **§9.3's `prefers-reduced-motion` was never read.** The gate has always been
  there -- the clip-eye glow draws only when `reducedMotion` is false -- and
  `setReducedMotion()` had no callers, so the flag sat at its default and the
  glow drew for everyone regardless of the setting. The preference is now read
  from the OS once at startup (macOS `com.apple.universalaccess`, Windows
  `SPI_GETCLIENTAREAANIMATION`) and applied to every strip. Where no single
  setting exists to read, "no preference" is reported rather than guessed.
- **`Tools/screenshot_app.sh` silently photographed stale binaries.** The README
  builds into `build`; the script only ever looked in `build-app`, so it either
  refused to run or -- worse -- rendered whatever old artefact happened to be
  sitting in the other directory. It now looks in both, prefers the **newest**
  binary, and prints which one it is capturing and when that was built. This is
  not hypothetical: it produced a screenshot offered as evidence for a change
  the binary did not contain.

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
