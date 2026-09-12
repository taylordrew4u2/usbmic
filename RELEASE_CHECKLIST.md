# SobStage v1.12.0 release checklist

**Classification:** release candidate. Do not present it as a general consumer
release until every **GA blocker** below is closed with evidence.

## 1. Source and identity

- [ ] The release commit is on `main`, the worktree is clean, and all intended
  changes have been reviewed.
- [ ] `CMakeLists.txt`, the app About/version strings, package metadata, the tag
  (`v1.12.0`) and this changelog all agree.
- [ ] `CHANGELOG.md` covers every user-visible change since v1.11.0.
- [ ] Dependency revisions and third-party GitHub Actions are immutable pins;
  the GPLv3 source offer and release archive are present.
- [ ] Fresh release screenshots have been captured, or the README continues to
  label the existing screenshots as historical.

## 2. Automated gates

- [ ] CI is green for Core + tests and the full app on Linux, macOS and Windows.
- [ ] The release workflow is green at the exact candidate commit.
- [ ] All unit tests pass on all three operating systems. The current baseline
  is **528 unit tests**; if tests change, record the final discovered count here.
- [ ] `sim_coreaudio`, `sim_wasapi` and `sim_camera` pass. The candidate baseline
  is **115 CoreAudio checks**, **70 WASAPI checks** and **85 camera checks**;
  record final counts from the candidate run rather than copying these numbers
  blindly.
- [ ] The sanitizer jobs pass. Both platform simulators run with ASan/UBSan
  and TSan.
- [ ] `sim_camera`, `sim_capture_mac`, `e2e_capture`, `live_capture`,
  `e2e_app_take.sh` and `e2e_refusal.sh` pass where their workflows support them.

## 3. Artifact inspection

- [ ] SHA-256 checksums exist and verify for the Linux ZIP, Windows ZIP, macOS
  ZIP and macOS DMG.
- [ ] A clean-machine install and launch succeeds from each final downloaded
  artifact, not only from a build directory.
- [ ] The macOS app is universal (`arm64` and `x86_64`), has a macOS 13.0 minimum,
  reports 1.12.0 in its bundle, and passes `Tools/verify_macos_release.sh` both
  before and after ZIP/DMG round trips.
- [ ] The DMG opens with current SobStage artwork, a working Applications link
  and no historical “Multi-Mic Aggregator” name or command.
- [ ] Windows and Linux packages contain the executable, README, GPL license and
  licensing notice and start from their documented paths; the Linux executable
  retains its execute bit after the final ZIP round trip.

### GA blockers: trust and distribution

- [ ] **macOS is Developer ID signed and notarized**, and Gatekeeper opens the
  downloaded app without asking the user to remove quarantine.
- [ ] **Windows is Authenticode signed** and shows the expected verified
  publisher. Record the browser-download/MOTW SmartScreen result on a clean
  machine; if a new publisher still receives a reputation warning, document it
  as an owner-approved launch limitation rather than calling the signature bad.
- [x] The v1 update and crash-reporting/privacy decision is explicit: manual,
  checksum-verified updates and user-reviewed diagnostic exports; no automatic
  update or crash upload.

The release workflow fails closed on a public tag unless all production secrets
exist. macOS requires `APPLE_CERTIFICATE_P12`,
`APPLE_CERTIFICATE_PASSWORD`, `APPLE_ID`, `APPLE_APP_PASSWORD` and
`APPLE_TEAM_ID`; Windows requires `WINDOWS_CERTIFICATE_PFX` and
`WINDOWS_CERTIFICATE_PASSWORD`. A build-only rehearsal may remain ad-hoc or
unsigned, and its workflow log labels it as such.

Unsigned packages may be labelled and distributed to informed beta testers;
they are not a completed consumer release.

## 4. Physical-hardware validation

Run the §12 matrix on clean supported machines and attach the logs/results to the
release record. Simulation is not a substitute.

- [ ] **GA blocker:** Complete a real macOS take with a directly attached
  PUPGSIS T12S at its observed fixed **44.1 kHz** rate. Verify audible stems and
  MIX, correct duration/format, stop metadata, activity log and mirror copy.
- [ ] **GA blocker:** Complete representative one-, two- and eight-input takes
  on macOS and Windows with physical USB interfaces, output hardware and a
  removable destination.
- [ ] Confirm the macOS list includes eligible directly attached USB, FireWire
  and Thunderbolt inputs and excludes built-in Mac, iPhone/Continuity,
  Bluetooth/AirPlay, network, aggregate, virtual, PCI and unknown inputs.
- [ ] Confirm Windows admits only eligible USB, FireWire and Thunderbolt device
  branches with positive removable capability and removal-policy evidence, and
  excludes fixed/internal USB, phone, Bluetooth, software and unknown inputs.
- [ ] Confirm Linux admits only kernel ALSA cards whose sysfs ancestry reports
  removable hardware. Verify the production package rejects virtual/plugin
  PCMs and that the test-fixture compile option is absent from it.
- [ ] **GA blocker or owner-approved limitation:** decide how to handle a phone
  or wireless receiver that identifies as generic removable USB Audio Class
  hardware. The current policy cannot distinguish it from an interface and uses
  no product-name or vendor-ID heuristics, so it cannot promise “never a phone.”
- [ ] Measure end-to-end monitor latency by loopback and verify the §5.4 ceiling
  for macOS CoreAudio and Windows WASAPI; add ASIO only if a real ASIO path ships.
- [ ] Run the drift/long-take gate on real independent device clocks.
- [ ] Exercise hot-plug, output loss, full/slow/card removal, mirror failure,
  device-busy, rate refusal, sleep/wake, power loss and recovery during takes.
  On known slow media, confirm preflight measures post-flush throughput and
  enforces the 2× gate rather than accepting page-cache speed.
- [ ] Run the novice acceptance test and verify every failure is visible and
  leaves an honest session/activity record.
- [ ] On macOS and Windows, verify cameras remain off at first launch, require an
  explicit enable, and can preview/record alongside the maximum audio load.

## 5. Product, legal and support

- [ ] `README.md`, in-app Help, [`SUPPORT.md`](SUPPORT.md) and
  [`PRIVACY.md`](PRIVACY.md) agree with the candidate's behavior.
- [ ] The GitHub issue route is monitored:
  <https://github.com/taylordrew4u2/usbmic/issues/new>.
- [ ] Diagnostic zips were manually inspected to confirm they contain no audio
  or video and that the documented identifiers, paths, logs and metadata match.
- [ ] App name, bundle identifier, copyright holder, privacy wording and GPLv3
  notices have owner/legal sign-off.
- [ ] JUCE licensing is checked against the current official JUCE licensing page
  for the selected distribution model; no undated price table is relied on.
- [ ] Release notes state the supported targets and known limitations without
  describing simulator results as hardware certification.

## 6. Release and rollback

- [ ] Keep the v1.11.0 tag and assets immutable and available until v1.12.0 is
  proven in production.
- [ ] Publish v1.12.0 from the exact tested commit; never move or reuse the tag.
- [ ] Smoke-test each URL, checksum, install, launch, short recording, playback,
  diagnostics export and uninstall from the public release page.
- [ ] If a serious regression appears, mark v1.12.0 as pre-release/not latest,
  point users to v1.11.0 when safe, preserve reports and publish a fixed v1.12.1
  from a new commit/tag. Do not silently replace assets or retag.
- [ ] Before advising a downgrade, back up `settings.json` and confirm the older
  version can read it; otherwise remove or restore settings explicitly. Never
  delete recordings or session folders as part of rollback.

## Sign-off

| Gate | Owner | Evidence | Date |
|---|---|---|---|
| Engineering and CI |  |  |  |
| macOS artifact/signing/notarization |  |  |  |
| Windows artifact/signing |  |  |  |
| Physical audio matrix |  |  |  |
| Camera matrix |  |  |  |
| Privacy/licensing/support |  |  |  |
| Release owner go/no-go |  |  |  |

The original engineering findings and their historical evidence remain in
[`docs/AUDIT-2026-09-07.md`](docs/AUDIT-2026-09-07.md).
