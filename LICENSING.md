# Licensing

This project is released under the **GNU General Public License v3** — see
[`LICENSE`](LICENSE). GPLv3 was chosen because it is the one licence that works
with the JUCE 7 dependency for an open-source distribution. Read the official
[JUCE 7 licence](https://juce.com/legal/juce-7-license/) before distributing a
build; this summary is not a substitute for its terms.

## Distribution review (12 September 2026)

The release configuration was checked against the official JUCE 7 licence on
that date. This repository is public, ships its GPLv3 source, and pins JUCE
7.0.12 at commit `4f43011b96eb0636104cb3e433894cda98243626` in
`CMakeLists.txt`; those are the facts the GPL distribution path relies on. A
release owner must still confirm the final source archive and notices beside
each published binary. This engineering review is not legal advice or a
substitute for the release owner's approval.

## What GPLv3 means here

- You may use, build, modify and redistribute the app and its source freely.
- If you distribute binaries, you must make the corresponding source available
  under the same licence. Tagged SobStage releases are configured to ship a
  versioned source bundle containing both the exact SobStage revision and the
  pinned JUCE tree; the release owner still verifies that bundle and its
  notices against the binaries before publishing.
- This repository does not rely on a commercial JUCE price tier. Distribution
  still has to follow the GPLv3 and JUCE 7 terms linked above.

## If you ever want a closed-source build instead

That may be possible without changing this repository's history. Commercial
tiers, prices, revenue thresholds and permitted uses can change, so this file
does not reproduce them. Check JUCE's current official
[licensing and download page](https://juce.com/get-juce/) and the JUCE 7 licence
linked above before making or distributing a proprietary build. The copyright
holder can license their own SobStage code separately, but cannot change JUCE's
terms.

## Unrelated to JUCE

No other third-party source is vendored into `Source/Core` or `Tests`. The test
framework (`Tests/TestFramework.h`) and the JSON reader/writer
(`Source/Core/Json.h`) were written here, so the platform-independent core and
unit-test target do not add another source dependency.

The §7 virtual-device backends remain separate: backend C would bundle a
commercially licensed third-party driver and backend D needs an EV certificate;
both are documented in the README as unimplemented for exactly that reason.
