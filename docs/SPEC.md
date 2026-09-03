# SobStage — multi-microphone aggregator, recorder, and monitor
## Build Specification — v3, self-contained
Cross-platform desktop application (macOS + Windows). Aggregates multiple USB microphones into a single input device, records all channels directly to an external card, and outputs one shared live monitor mix to headphones. Must be operable by someone who has never configured audio hardware.
Reference hardware: Blue Yeti (standard model). Section 14 is written against its measured behavior and constrains decisions elsewhere in this document.
**Every value in this document is a specified default, not a placeholder.** Where a number appears, use it. Where a rule appears, follow it. Section 17 lists decisions that are closed and the reasoning behind them, so they are not reopened mid-build.
---
## 0. Non-negotiables
1. Audio is never lost. A dropped meter frame is acceptable. A dropped sample is not.
2. A novice reaches a finished recording without asking a question or reading anything.
3. Everyone hears the identical mix, containing every microphone including their own, from the moment the app opens.
4. The user never touches OS sound settings.
5. Nothing ships that requires disabling driver signature enforcement or running in test mode.
---
## 1. Scope and limits
| Property | Value |
|---|---|
| Minimum microphones | 1 |
| Maximum microphones | 8 |
| Behavior above 8 | Enumerate and display, but exclude from capture with a stated reason |
| Maximum session length | Unlimited; files split per §6.1 |
| Minimum macOS | 13.0 |
| Minimum Windows | Windows 10 22H2 |
| Minimum RAM | 4 GB |
| Minimum free internal disk | 2 GB, or the local mirror is disabled |
| Languages at v1 | English only. All user-facing strings externalized for later localization. |
**Single-microphone case.** With one mic the app is still a recorder and monitor. Drift compensation is bypassed (nothing to sync against). Nothing in the UI changes shape.
**Zero-microphone case.** The app opens, shows an empty skull row, and displays one line: "Plug in a microphone to get started." Monitoring and recording are unavailable but no error state is shown. This is a normal condition, not a failure.
---
## 2. Device aggregation
- Enumerate all USB audio input devices at launch and continuously after, polling on OS device-change notification, never on a timer.
- Every detected microphone is included automatically. Inclusion is opt-out, not opt-in.
- Hot-plug without restart, without dialogs, without interrupting an active recording. See §6.5.
### 2.1 Channel layout normalization
Many USB mics present as stereo with one silent side or a duplicated mono source. On connection, compare both channels over the first 3 seconds during which either channel exceeds −50 dBFS.
Collapse to mono when **either** condition holds:
- One channel stays below −80 dBFS for the whole window, or
- Inter-channel correlation exceeds 0.99 **and** RMS difference is under 0.5 dB.
Otherwise record true stereo.
If the 3-second signal window is not reached within 60 seconds of connection, default to mono and re-evaluate if signal later appears. Never block on this and never ask a novice "mono or stereo?"
### 2.2 Sample rate negotiation
Query supported rates for every device. Choose the highest rate common to all, capped at 48 kHz. If a device cannot reach the chosen rate, resample that device. **Never reject a microphone for rate reasons.**
### 2.3 Bit depth
Follows device capability per channel. Do not upconvert — it adds file size and no information. Where a device supports multiple depths, choose the highest, capped at 24-bit.
### 2.4 Port memory
Identify each physical device by USB location ID plus serial number where present, falling back to location ID alone. When a previously seen device reconnects to the same port, restore its assigned name, trim, and channel-layout decision without re-running §2.1 or §14.6.
---
## 3. Clock drift compensation
Build first. Everything else is easier afterward.
Every USB mic runs an independent crystal. Uncorrected, channels desync and a long take is unusable.
### 3.1 Master selection
Default master: the device with the lowest measured drift after 60 seconds of running. Before that measurement exists, use enumeration order. Overridable behind Advanced.

**The master is a reference, not an exemption.** It names (a) the channel §3.3's drift figures are quoted against and (b) the clock source handed to the OS aggregate device. It does **not** mean "the one input that is never resampled". In the in-app capture path the streams are pulled by the output device's callback, so the clock every input is converted onto is the *output* device's, not any microphone's. Exempting the master from correction there does not make it the timebase — it leaves one channel uncorrected against a clock it has no relationship to, and that channel's ring then walks to one end of its travel and stays there, dropping arrivals when full or holding its last sample when dry. That is drift, on the one channel the whole rig is quoted against, and it is invisible to the underrun counters. Correct every channel, master included.
### 3.2 Correction
- **Every** input passes through an asynchronous sample rate converter locked to the clock that pulls it — the output device's callback in the in-app path (§5.2), the OS in the aggregate path. This includes the §3.1 master; see the note there for why exempting it corrupts the reference rather than protecting it.
- Because the pulling clock's own skew lands in every channel's ratio identically, it cancels in any channel-to-channel comparison. §3.3's per-device figure is therefore reported as this channel's correction *minus the master's*, which is what makes "runs fast relative to the master" true of the number shown.
- ASRC ratio is driven by a PI control loop on measured ring-buffer fill.
  - Starting gains: Kp = 1e-6, Ki = 1e-8, per sample of fill error.
  - Maximum ratio deviation: ±200 PPM. Clamp there.
  - Maximum ratio slew: 5 PPM per second. **Never correct instantaneously** — that produces audible pitch artifacts.
- **Drift slack is capped by the monitor latency budget, not chosen freely.** Size it to 1 ms and tighten the loop to compensate. Drift safety and low latency pull against each other; latency wins, the loop absorbs the difference.
- **Short-kernel ASRC on the monitor path** — 16-tap sinc or equivalent. Long-kernel high-quality resamplers add delay and are disqualified live. If quality suffers, use the short kernel live and a higher-quality offline pass for recorded stems.
### 3.3 Reporting and failover
- Track per-device drift in PPM, displayed behind Advanced, updated every 10 seconds.
- Flag any device exceeding **100 PPM sustained** as unreliable. Flag, do not exclude.
- **Master failover.** If the master is unplugged, promote the remaining device with the lowest measured drift; tiebreak by enumeration order. Re-lock without stopping the recording. Log the switchover timestamp in `session.json`. A bounded transient is acceptable; a stopped recording is not.
  - Per §3.1 this moves a reference, not a correction: no channel's resampling changes, so mid-take there is no transient to bound and the take's channel list and file layout stay fixed (§6.5). What it does prevent is quoting every surviving microphone against a master that has stopped updating.
### 3.4 Validation gate
A 4-hour take with four dissimilar USB microphones finishes with **under 1 ms inter-channel drift**, measured by cross-correlating a shared transient at the start and end of the take. This passes before UI work is considered complete.

The master's own crystal is part of what this measures. A gate that gives the master a zero offset — a crystal identical to the pulling clock's — assumes away exactly the case §3.1's note describes, and will pass over a master that is drifting. `Tools/soak_drift.cpp` gives it a non-zero offset for that reason.
---
## 4. Gain, trim, and the recorded signal
- Per-microphone trim exists as a single shared-bus trim. It does not create a second mix.
- Range: −20 dB to +20 dB. Default 0 dB. Step 0.5 dB.
- **Trim affects the monitor mix and the summed mix file. Trim does not affect the discrete per-microphone files.** Stems are always written at unity gain, unprocessed.
- Rationale: the novice gets a mix matching what they heard; the editor gets clean stems that a bad trim decision cannot destroy.
- `session.json` records trim values so the mix is reproducible from stems.
- **On hardware with an analog gain knob, software trim cannot recover a clipped input** — the knob sits ahead of the converter. On persistent clipping, direct the user to the physical knob on the named microphone. Never present software trim as the fix.
---
## 5. Monitoring — locked topology
### 5.1 The mix
- One mix. Identical for every listener. Contains every microphone, including each listener's own.
- No mix-minus, no per-listener variation, no solo, no cue routing, no exceptions.
- Live from launch, independent of record state. No arm step, no enable toggle. Hearing the room is the app's proof of life.
- Summing is unity, no per-channel attenuation with channel count. Level is managed by trim and the master control, not by automatic gain.
- Master monitor volume: 0–100, default 70, mapped logarithmically. Recorded files are unaffected by it.
### 5.2 Physical topology
```
Computer output -> headphone amp or splitter -> all listeners
```
- **Microphone headphone jacks are NOT used.** The app opens exactly one audio output stream, ever, and never opens a microphone's playback endpoint.
- Reason: on hardware with non-defeatable analog direct monitoring (§14.5), a listener plugged into a mic hears their own voice twice — once analog at 0 ms, once from the shared mix at ~8 ms. That comb-filters and sounds worse than plain latency. Since the shared mix already contains their voice, the mic jack adds nothing.
- Detect headphones connected to a microphone jack where the hardware exposes it. Warn plainly: "Unplug headphones from the microphones. Use the headphone amp instead, or you'll hear yourself twice."
- **A headphone amp or splitter is required hardware**, with at least as many outputs as listeners. State this in first-run guidance, not in an error.
### 5.3 Output device selection
Automatic, in this priority order:
1. A device the user explicitly chose in a previous session that is currently present.
2. A newly connected output device (a device appearing after launch is assumed to be the one the user just plugged in).
3. A device exposing a physical headphone jack.
4. System default output.
Never a microphone's playback endpoint, at any priority.
### 5.4 Latency budget — hard constraint
Zero latency is not achievable in a software monitor path. The requirement is a budget.
- **Target: under 6 ms** mic diaphragm to headphone, **measured by loopback at startup, not estimated.**
- **Ceiling: 10 ms.** Above this the listener hears a distinct echo of themselves against their own bone conduction and the product has failed.
| Stage | Budget |
|---|---|
| USB transport + ADC/DAC | ~2–4 ms, fixed, not reducible |
| Input buffer, 32–64 samples @ 48 k | 0.7–1.3 ms |
| Drift compensation slack | ≤ 1 ms |
| Sum + limiter | < 0.1 ms |
| Output buffer, 32–64 samples | 0.7–1.3 ms |
Requirements this imposes:
- **Exclusive-mode audio only on the monitor path.** CoreAudio direct on macOS; ASIO or WASAPI exclusive on Windows. **Shared-mode WASAPI is disqualified** regardless of which app-facing backend is active — it delivers 40–100 ms, which makes the product unusable.
- If neither exclusive path is available, say so and name the cause. Never ship a 40 ms mix silently.
- **Nothing on the monitor bus but summing, trim, and the safety limiter.** No EQ, no filtering, no lookahead, no processing. A lookahead limiter is disqualified; use a zero-lookahead design and accept its distortion, which only engages in a fault.
**Buffer ladder.** Start at 64 samples. Step up through 128, 256, 512 on trigger. Trigger: 3 or more callback overruns within any 30-second window. Never step down automatically during a recording; re-evaluate only on next launch or on device change. Log every step in `session.json`.
Note: everyone is in the same room and already hears each other acoustically at 0 ms. The headphone mix's job is confidence and level-checking, not intelligibility. That lowers the stakes on 8 ms considerably. It does not lower the stakes on 40 ms.
### 5.5 Safety
- **Monitor bus brickwall limiter, mandatory.** Ceiling −3 dBFS, zero lookahead, 1 ms release. A loose connector or device fault can put full-scale noise into headphones on someone's head. This is a hearing-safety requirement, not a mix decision.
- **Runaway cut.** If the limiter is engaged continuously for more than 500 ms, mute the monitor bus, show the reason, and require a manual unmute.
- **Feedback protection.** Detect narrowband energy growth: any 1/3-octave band rising more than 10 dB over 500 ms while remaining within 6 dB of the broadband peak. On detection, mute the monitor bus with a visible reason and a one-tap unmute.
- **Global monitor mute** on the spacebar, instantly reversible, with a visible muted state.
- At startup, refuse to route monitor output to a device that is also a selected input, and explain why.
- Comb filtering from self-monitoring is inherent. Mitigate with low latency; never attempt to correct it with processing.
---
## 6. Recording
### 6.1 What is written
- Discrete per-microphone files at unity gain, plus one summed mix file.
- The mix file is trim + sum + a **mix-bus limiter at −1 dBFS**. This is a separate instance from the monitor safety limiter in §5.5 and is not affected by the runaway cut — a monitor mute must never silence the recording.
- The mix file is not affected by master monitor volume.
- WAV/BWF. Bit depth per §2.3. Sample rate per §2.2.
- BWF timestamp and a shared session origin in every file, so a DAW aligns stems on import without manual nudging.
- Auto-split at 3.9 GB, or write RF64. Split files are numbered `_001`, `_002`, and carry continuous BWF timestamps.
### 6.2 Session structure
A novice losing track of their recording is a total product failure even when the software worked perfectly.
```
/RECORDINGS/2026-08-26_1432_Session/
    session.json
    MIX.wav
    01_Yeti-Kitchen.wav
    02_Yeti-Couch.wav
    03_Yeti-Corner.wav
```
- Folder name: `YYYY-MM-DD_HHMM_<name>`. Default `<name>` is `Session`. User-editable before or after recording.
- File names carry channel number and the user-assigned mic name, never a bare index.
- Sanitize names to `[A-Za-z0-9 _-]`, collapse whitespace to single hyphens, truncate at 40 characters.
- On folder name collision, append `_2`, `_3`, and so on. Never overwrite, never prompt.
- `session.json` contains: app version, start and stop timestamps, sample rate, bit depth, buffer size, measured latency, device list with names and USB IDs, trim values, per-device drift log, buffer-size changes, dropout events, master failover events, and mirror status.
- On stop, show the location and offer to open the containing folder.
### 6.3 Write pipeline
- Lock-free ring buffer in RAM, dedicated writer thread. **No allocation, locking, logging, or file I/O inside the audio callback. Ever.**
- RAM buffer sized for 30 seconds at current channel count and rate, minimum 64 MB.
- **Redundant local mirror**, default on. If internal free space exceeds 2 GB plus the projected session size, write a second copy to the user's home directory in parallel. Converts most card failures from data loss into inconvenience.
- If internal space drops below 1 GB during recording, stop the mirror, keep the card write going, and note it in `session.json`. Do not interrupt the recording and do not show a modal.
### 6.4 Pre-flight
Run on volume selection and again on arming; cache per volume ID with a 30-day expiry.
- Write a 200 MB test file. Measure **sustained minimum throughput over rolling 1-second windows**, not average. Delete the test file.
- Required rate = channels × sample rate × bytes per sample × 2 (card plus mix file overhead).
- **Block arming if sustained minimum is under 2× required.** Refuse before recording starts; **never degrade mid-take.** A novice cannot act on a mid-take warning, and a corrupted three-hour recording is worse than a delayed start.
- Report free space as remaining recording time in hours and minutes, not bytes.
- Detect FAT32, or any filesystem the OS cannot write reliably. Offer reformat to exFAT, state clearly what is erased, require typed confirmation.
### 6.5 Mid-recording events
| Event | Behavior |
|---|---|
| Microphone unplugged | Continue writing silence to that channel. Never change channel count or file layout mid-file. Log the dropout. Skull goes dashed. |
| Microphone reconnected mid-take | Resume writing its live signal to its existing channel. Restore name and trim per §2.4. Log the reconnection. |
| New microphone plugged in mid-take | Add to the monitor mix immediately. Do **not** add to the in-progress recording. State in one line: "Mic added to monitoring. It'll be recorded starting with your next take." |
| Clock master unplugged | Failover per §3.3. Recording continues. |
| **Target card removed** | Stop immediately, finalize every open file, alert loudly. If the mirror is running, state that a complete copy survives locally and give its path. |
| Card full | Stop at the last complete buffer block, finalize, report that available time was exhausted. Warn at 10 minutes and 2 minutes remaining. |
| Sustained buffer overrun | Warn visually at 50% ring fill. Never silently drop. At 90% fill with the mirror unavailable, fall back to writing the mix file only and log the exact sample position of degradation. |
| Output device disappears | Monitoring stops, recording continues uninterrupted. Re-select per §5.3 and resume monitoring automatically. |
### 6.6 Crash and power loss
- Update WAV headers every 5 seconds so an interrupted file remains playable to within 5 seconds of the cut.
- On launch, scan the last-used card and the mirror directory for sessions lacking a stop timestamp. Repair headers, present recovered recordings before the main screen.
- Discard any recovered file containing under 1 second of audio; report it as empty rather than presenting an unplayable stub.
- **Inhibit system sleep, screensaver, and display sleep for the duration of a recording.** A laptop sleeping mid-take is a common and total failure.
- Monitor for thermal throttling and sustained CPU pressure above 80% for 30 seconds; warn before it causes dropouts.
---
## 7. Virtual device strategy — Windows is the whole problem
**macOS:** `AudioServerPlugIn`. User-space CoreAudio HAL plugin, no kernel extension, Developer ID and notarization only. Ship in v1.
**Windows:** kernel-mode WDM/KS is the only native path to a system-visible audio endpoint, and Microsoft is the sole signer of production kernel drivers. Cross-signing is dead and the April 2026 Windows update removed default trust for cross-signed kernel drivers. Keep a self-built driver off the v1 critical path.
Implement the app-facing output as a swappable backend behind one interface:
| Backend | Reach | Cost |
|---|---|---|
| **A — none.** Standalone recorder + monitor. | n/a | Ships immediately. Must be saleable alone. |
| **B — ASIO output.** User-mode DLL, COM registered, no Microsoft signing. | DAWs, OBS. Not browsers or conferencing. | Days. |
| **C — licensed signed virtual cable** (VB-Audio, Virtual Audio Cable, Thesycon). Bundle their signed driver, write into their endpoint. | Every app. | Per-seat licensing. **Intended production path.** |
| **D — own attestation-signed WDM driver.** | Every app. | Registered legal entity, EV certificate, Partner Center, CAB submission. Longest lead item; administrative, not technical. |
- **Start the legal entity and EV certificate paperwork on day one** regardless of backend. Every path except A and B is gated on it.
- The UI always shows which backend is active and names, in plain language, which applications can and cannot see the aggregate device.
- Attestation-signed is not WHQL: it works on Windows 10/11 desktop but cannot be distributed via Windows Update, which requires full HLK certification. Windows Server ignores attestation entirely. Neither matters for this market.
- macOS already aggregates natively via Audio MIDI Setup. Do not position aggregation as the macOS feature. macOS differentiation is automatic setup, direct-to-card recording, always-on shared monitoring, and drift telemetry.
---
## 8. Metering
### 8.1 Behavior
- One meter per microphone, plus one for the shared mix.
- **Meters run from launch, not from record.** No visual mode change when recording starts.
- Each shows instantaneous level, peak hold, clip state, and an always-visible numeric dBFS readout.
- Scale −60 to 0 dBFS, logarithmic.
- Ballistics: 10 ms attack, 1.5 s decay, peak hold 2 s then decay at 20 dB/s. Raw sample values strobe and are unreadable.
- Clip latches with a running count until tapped. Threshold: 3 consecutive samples at or above −0.1 dBFS.
- **Dead-channel detection:** a channel below −60 dBFS for 20 continuous seconds while at least one other channel has exceeded −40 dBFS during the same window. Flag it and name the likely cause.
### 8.2 Implementation
- Audio thread computes only max-abs and RMS per block into a per-channel atomic. Nothing else.
- UI thread polls at 60 Hz. Meter rendering never blocks and is never blocked by the audio callback.
- Metering is lossless with respect to recording: dropping a meter frame is fine, dropping a sample is not.
---
## 9. Visual treatment — skull meters
The signature element. Each microphone is a skull; the skull **is** the meter, not decoration beside one.
### 9.1 Behavior
- Level fills the silhouette from the jaw upward. Empty = −60 dBFS, full = 0 dBFS, mapped on the §8.1 scale.
- Peak hold is a 2.5 px bone-white bar across the skull at the peak position.
- Eye sockets are the clip indicator: dark by default, lit amber with a glow on clip, latched until tapped.
- No signal renders as a dashed hollow outline at 42% opacity.
- Mic name, device name, numeric dBFS, and peak value below each skull.
- The shared mix uses a horizontal bar, not a skull — visually distinct so the bus is never confused with a channel.
### 9.2 Palette
| Role | Hex |
|---|---|
| Background | `#16110F` |
| Panel | `#1E1816` |
| Skull outline / bone | `#EDE4D3` |
| Empty skull interior | `#2A2320` |
| Fill, below −18 dBFS | `#7A9E7E` |
| Fill, −18 to −3 dBFS | `#D9A441` |
| Fill, above −3 dBFS | `#C3352B` |
| Clip eyes | `#F2C14A` |
| Dimmed / no signal outline | `#6E645B` |
| Secondary text | `#8C8177` |
| Tertiary text | `#5E554D` |
### 9.3 Constraints
- **Color never carries meaning alone.** Every colored state also changes shape, text, or number. Clip indication cannot depend on hue.
- Silhouette legible at 48 px. Test small before committing to detail.
- Respect `prefers-reduced-motion`: glow and pulse off, fill and numbers still live.
- Numeric readouts in a monospace face so digits do not jitter as values change.
---
## 10. User experience — zero-knowledge setup
Target user has never configured an audio device, does not know what a sample rate or buffer is, and will not read documentation.
### 10.1 First run
- Launches to a working state. No wizard, no account, no device picker.
- Every connected microphone detected and included automatically.
- Rate, depth, buffer, clock master, output device, and backend chosen automatically. The user is never asked.
- Monitoring live the instant the window opens.
- Destination defaults to the connected external card; if none, `~/RECORDINGS`, stated in one line.
- **Permissions are the first real obstacle.** macOS microphone permission, macOS removable-volume access, Windows microphone privacy setting. If any is denied, show what to enable and why, inside the app, without sending the user hunting through system settings unaided.
### 10.2 Main screen
One window. One primary control: a large record button. Everything else is status.
Visible by default:
- Skull row with names and levels
- Record button
- Elapsed time
- Remaining recording time
- Where it saves
- Master headphone volume
- Mute state
**No audio jargon in the default view.** Not "aggregate device," "ASIO," "buffer," "sample rate," "gain staging," "monitor bus." Say microphones, headphones, where it saves, how long you can record.
### 10.3 Advanced panel — exhaustive contents
One door, one click, nothing behind it required for correct operation:
sample rate · bit depth · buffer size · measured latency · clock master selection · per-device drift in PPM · per-microphone trim · output device selection · active app-facing backend and its limitations · local mirror on/off · destination folder · diagnostics export.
### 10.4 Record button behavior
- Idle → press → recording starts immediately, no countdown, no confirmation.
- Recording → press → recording stops immediately, files finalize, location is shown. No confirmation dialog. Stopping is not destructive.
- Disabled only when zero microphones are connected or pre-flight (§6.4) has failed, and in both cases the reason is shown next to the button.
### 10.5 Physical setup guidance
Novices fail on hardware, not software. Detect and explain, with the fix stated:
- Bus power exhausted → a hub **with its own wall adapter** is needed, and why.
- Card filesystem wrong → offer reformat to exFAT, state what is erased.
- Card too slow → state measured speed, required speed, and the two ways out.
- Headphones plugged into a microphone → redirect to the headphone amp (§5.2).
- Only one headphone output for several listeners → a splitter or amp is required.
- **Microphone muted at the hardware switch** → the single most common failure. Name it explicitly whenever a channel reads silence.
- **Non-cardioid polar pattern** → see §14.4.
### 10.6 Language
- Errors name what happened, then what to do, in one sentence. No codes, no apologies, no vagueness.
  - Good: "Mic 4 isn't sending sound. Check the mute button on the mic."
  - Bad: "Input stream error (0x8007001F)."
- Buttons say what happens. "Start recording" → "Recording." "Stop recording" → a file, and where it went.
- Technical detail is never hidden from someone who wants it, and never shown to someone who doesn't. That is what the Advanced panel is for.
### 10.7 Acceptance test
Hand the app and a bag of microphones to someone who has never recorded audio. They must reach a finished recording on a card, with everyone hearing everyone, without asking a question or reading anything. **Every question they ask is a bug in the interface, not a gap in the documentation.**
---
## 11. Engineering constraints
- Suggested stack: C++ with JUCE, or PortAudio/miniaudio. CoreAudio on macOS; WASAPI exclusive plus ASIO on Windows.
- Real-time thread does no allocation, locking, logging, file I/O, or drawing. Ever.
- Signed and notarized macOS build. Signed Windows installer.
- **Diagnostics export**: one button writes a zip containing app logs, the last five `session.json` files, and a device inventory. Never includes audio. This is how support requests are handled without screen-sharing.
- Crash reporting on by default, opt-out in Advanced. **Never upload audio, file names, or session contents.** A recorder that phones home with content is unshippable.
- Auto-update. If a bundled driver is present, updating requires elevation and possibly a reboot — design that flow before shipping backend C or D. Never auto-update while a recording is in progress.
---
## 12. Validation matrix
Test before release, not after.
- **Microphones:** at least six models across price tiers, including one that presents as stereo-with-silent-right and one with a hardware mute switch.
- **Counts:** 1, 2, 4, and 8 simultaneous microphones, plus a 9-mic case to confirm §1 behavior.
- **Duration:** one 4-hour continuous take at maximum channel count, passing §3.4.
- **Cards:** one fast card and one deliberately slow card that fails pre-flight.
- **Latency:** measured loopback figure on macOS, Windows ASIO, and Windows WASAPI exclusive. Confirm the 10 ms ceiling holds on the slowest supported configuration.
- **Hostile events:** unplug a mic mid-take · reconnect it mid-take · unplug the clock master mid-take · pull the card mid-take · force-quit mid-take · cut power mid-take · exhaust bus power mid-take · disconnect the output device mid-take · fill the card mid-take.
- **OS:** current and previous major versions of macOS and Windows 11, plus the §1 minimums.
---
## 13. Build order
1. Multi-device capture with drift compensation. Gated on §3.4.
2. Direct-to-card write pipeline with throughput benchmarking and the §12 hostile-event matrix.
3. Shared monitor bus with limiter, mute, and feedback protection. Gated on the §5.4 latency ceiling.
4. Metering.
5. Virtual device backends — macOS first, Windows tiered.
6. UI and the zero-knowledge setup flow.
Legal entity registration and EV certificate procurement run in parallel from day one.
---
## 14. Hardware profile — Blue Yeti
Verified: 16-bit / 48 kHz, 5 V / 150 mA, four polar patterns, 3.5 mm headphone jack with analog direct monitoring, hardware mute button, analog gain knob. Confirm on a bench unit; Yeti X and Yeti Nano differ from the standard Yeti.
### 14.1 Format
- **Fixed at 16-bit / 48 kHz.** A 24-bit default is unreachable with this hardware. Bit depth follows §2.3.
- An all-Yeti rig lands on 48 kHz with no inter-device resampling needed, which relaxes the drift budget. Drift compensation is still required — separate crystals, separate clocks — but the ASRC ratio stays near unity.
### 14.2 Power and USB topology
Yeti draws 150 mA. USB 2.0 port budget is 500 mA; USB 3.0 is 900 mA.
**Acceptable:** one Yeti per dedicated computer port, up to the machine's port count · any number of Yetis on a hub with its own AC adapter.
**Not acceptable:** three or more Yetis behind a bus-powered hub · four Yetis sharing one USB 2.0 port by any means.
**"Powered hub" means a hub with a wall adapter.** A hub that only plugs into the computer adds ports, not power. The two products look identical on a shelf and the distinction is invisible to a novice — first-run guidance must state it in exactly those words. Recommend a powered USB 3.0 hub rated at least 2 A total.
**Detection:** bus power exhaustion does not produce a clean error. It produces failed enumeration, intermittent dropouts, and devices disappearing mid-session. On two or more enumeration failures or device drops within 5 minutes with three or more mics attached, state the cause directly: "Your microphones need more power than this computer's USB port can supply. Use a USB hub with its own power adapter."
### 14.3 Controller contention — separate issue from power
Most laptops route all USB ports to one controller. Sustained SD card writes on the same controller as four isochronous audio streams cause dropouts that present as an application fault.
- Prefer a built-in SD slot over a USB card reader.
- If a USB reader is used, place it on a different controller from the microphones where the OS exposes controller topology.
- Detect the co-located case and warn **before arming**, not during: "Your card reader and microphones share one USB connection. Use the built-in card slot if you have one."
### 14.4 Polar pattern — the sleeper failure
- The pattern knob is physical with no software readback. Omni, stereo, and bidirectional capture the whole room.
- Multiple Yetis in omni or stereo in one room produce heavy bleed, unusable stems, inter-channel comb filtering, and elevated feedback risk.
- **Every microphone must be set to cardioid for multi-person recording.**
- **Detection:** correlation above 0.6 between two channels sustained for 10 seconds, while a third channel is below −45 dBFS, indicates at least one non-cardioid mic. Name the mic, name the knob, state the setting. Do not say "bleed" — the word means nothing to the target user. Say: "Mic 2 is picking up the whole room. Turn its pattern knob to the single-heart setting."
### 14.5 Headphone jack
- The standard Yeti's analog direct monitoring **cannot be disabled** — there is no blend control, and the direct signal is always present at the jack.
- This is why §5.2 prohibits using the mic jacks. It is a hardware property, not a configuration choice.
### 14.6 Identical device names — first-run blocker
Four Yetis enumerate with the same product string. A novice cannot tell which skull is which person.
- Disambiguate internally per §2.4.
- **Tap-to-name flow:** prompt the user to tap or speak into one microphone. Detect which channel exceeds −25 dBFS while all others stay below −45 dBFS for 300 ms, highlight that skull, and let them type a name. Repeat per mic. If two channels trigger together, say "Two mics heard that — try tapping closer to one" and retry.
- Takes seconds, requires no technical understanding, and works without headphones plugged into the mics.
- Names persist per §2.4 so the second session does not repeat setup.
### 14.7 Room reality
Yetis are sensitive condensers. Several open in one untreated room will each capture every voice. Stems will not be clean isolation. This is a hardware limit, not a software defect — state it in product documentation rather than letting users conclude the app is broken.
---
## 15. Out of scope for v1
Stated so they are not assumed:
- Timecode, word clock, or sync to external video.
- Networked or remote microphones.
- Post-production editing, effects, or noise reduction.
- Mobile.
- Localization beyond English.
- **More than one monitor mix.** Deliberate product decision, not a limitation to be fixed later.
- **Zero-latency monitoring.** Physically unavailable given a single shared mix from USB microphones. The 10 ms ceiling in §5.4 is the commitment.
---
## 16. Closed decisions
These were resolved deliberately. They look like oversights and are not. Do not reopen them without raising the trade-off explicitly.
| Decision | Why |
|---|---|
| Trim affects the mix file but not the stems | A novice's bad gain decision must not be permanent. The editor gets clean raw material regardless. |
| No mix-minus, no per-listener feeds | Everyone hears the identical mix. This was chosen over zero-latency self-monitoring, knowingly. |
| Microphone headphone jacks unused | Non-defeatable analog direct monitoring would double each person's voice ~8 ms apart against the shared mix. |
| Shared-mode WASAPI disqualified | 40–100 ms makes own-voice monitoring an audible echo. The product fails at that latency. |
| Pre-flight blocks rather than degrading mid-take | A novice cannot act on a mid-take warning. A delayed start beats a corrupted three-hour recording. |
| New mics join monitoring but not the in-progress take | Channel count cannot change inside a WAV file. |
| Unplugged mics write silence rather than shrinking the file | Same reason. File layout is fixed at record start. |
| Local mirror on by default | For this user, silent data loss is worse than a full internal drive. |
| Bit depth follows the device | Upconverting 16-bit Yeti audio to 24-bit adds size and no information. |
| Monitor mute never silences the recording | Separate limiter instances exist specifically to guarantee this. |
| 8-microphone ceiling | Above that, bus power, controller bandwidth, and room bleed all fail before the software does. |
