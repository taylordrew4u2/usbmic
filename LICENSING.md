# Licensing

This project is released under the **GNU General Public License v3** — see
[`LICENSE`](LICENSE). GPLv3 was chosen because it is the one licence that works
with the JUCE 7 dependency for an open-source distribution. Read the official
[JUCE 7 licence](https://juce.com/legal/juce-7-license/) before distributing a
build; this summary is not a substitute for its terms.

## What GPLv3 means here

- You may use, build, modify and redistribute the app and its source freely.
- If you distribute binaries, you must make the corresponding source available
  under the same licence. The source archive shipped alongside each release
  already satisfies this.
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

Nothing else in this repository carries a third-party licence obligation. The
test framework (`Tests/TestFramework.h`) and the JSON reader/writer
(`Source/Core/Json.h`) were written here rather than vendored in, so
`Source/Core` and the test suite have no external dependencies at all.

The §7 virtual-device backends remain separate: backend C would bundle a
commercially licensed third-party driver and backend D needs an EV certificate;
both are documented in the README as unimplemented for exactly that reason.
