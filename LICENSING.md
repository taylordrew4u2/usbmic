# Licensing — one decision is still yours

This project has **no LICENSE file**, deliberately. Choosing one is a decision
about your rights that shouldn't be made on your behalf, and the JUCE
dependency makes it a real choice rather than a formality.

## What JUCE requires

The app links **JUCE 7.0.12**, which is tier-licensed (see
`LICENSE.md` in the fetched JUCE source, or <https://www.juce.com/juce-7-licence>):

| Tier | Cost | Revenue limit | Can you keep your source closed? |
|---|---|---|---|
| Personal | free | under 50K USD | yes |
| Indie | $40/month | under 500K USD | yes |
| Pro | $130/month | none | yes |
| Educational | free | none (bona fide institutions) | yes |
| — | free | none | **only if you release under GPLv3** |

So there are two workable paths:

1. **Stay under the JUCE Personal revenue limit** and license this project
   however you like — MIT, proprietary, anything. Nothing here forces your hand.
2. **Release under GPLv3**, which JUCE permits at no cost and with no revenue
   limit, at the price of publishing source for anything you distribute.

Until you pick, the code is under default copyright: you can use and build it,
and nobody else may redistribute it.

## To choose

Drop the text into `LICENSE` at the repo root. GPLv3's authoritative text is at
<https://www.gnu.org/licenses/gpl-3.0.txt>; on most Linux systems it is also at
`/usr/share/common-licenses/GPL-3`. The release workflow picks up a `LICENSE`
file automatically if one exists.

## Unrelated to JUCE

Nothing else in this repository carries a third-party licence obligation. The
test framework (`Tests/TestFramework.h`) and the JSON reader/writer
(`Source/Core/Json.h`) were written here rather than vendored in, specifically
so that `Source/Core` and the test suite have **no external dependencies at
all** — that is why they build on a machine with no network.

The §7 virtual-device backends are a separate matter and are not licensing
decisions you need to make now: backend C would bundle a commercially licensed
third-party driver, and backend D needs an EV certificate. Both are documented
in the README as unimplemented for exactly that reason.
