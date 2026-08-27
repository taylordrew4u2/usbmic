# Licensing

This project is released under the **GNU General Public License v3** — see
[`LICENSE`](LICENSE). GPLv3 was chosen because it is the one licence that works
unconditionally with the JUCE dependency: JUCE 7 permits GPLv3 distribution at
no cost and with **no revenue limit**, whereas every other arrangement is tied
to a paid tier or a revenue ceiling.

## What GPLv3 means here

- You may use, build, modify and redistribute the app and its source freely.
- If you distribute binaries, you must make the corresponding source available
  under the same licence. The source archive shipped alongside each release
  already satisfies this.
- No revenue limit, no fees, no JUCE tier to track.

## If you ever want a closed-source build instead

That is possible without touching this repository's history: JUCE's paid tiers
(Personal under 50K USD revenue is free; Indie/Pro above that) allow proprietary
distribution. As the copyright holder of this code you can dual-license your own
work; only the JUCE terms for that build would change.

## The JUCE tier table, for reference

| Tier | Cost | Revenue limit | Closed source allowed |
|---|---|---|---|
| Personal | free | under 50K USD | yes |
| Indie | $40/month | under 500K USD | yes |
| Pro | $130/month | none | yes |
| Educational | free | none (bona fide institutions) | yes |
| GPLv3 (this project's choice) | free | none | no — source must ship |

## Unrelated to JUCE

Nothing else in this repository carries a third-party licence obligation. The
test framework (`Tests/TestFramework.h`) and the JSON reader/writer
(`Source/Core/Json.h`) were written here rather than vendored in, so
`Source/Core` and the test suite have no external dependencies at all.

The §7 virtual-device backends remain separate: backend C would bundle a
commercially licensed third-party driver and backend D needs an EV certificate;
both are documented in the README as unimplemented for exactly that reason.
