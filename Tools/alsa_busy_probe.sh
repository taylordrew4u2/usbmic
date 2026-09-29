#!/usr/bin/env bash
# A four-input interface must record four tracks, and keep its four strips
# when the device list changes while it is recording.
#
# Both used to reopen the device to ask how many inputs it has. Real hardware
# is -EBUSY to a second open -- from this app as much as any other -- so the
# answer came back "one". The fixture's file plugin never says busy, so the
# shim makes it: MMA_SHIM_EXCLUSIVE gives each capture PCM a hw: device's
# one-open rule. MMA_SIM_REALTIME paces the reads like a card instead of
# failing them.
#
#   Tools/alsa_busy_probe.sh
#
# Runs in its own HOME, so the four-input device it adds to the fixture is
# not listed by every other harness that shares ~/.asoundrc.
set -euo pipefail

GATE=""
for CANDIDATE in build/alsa_busy_probe build-app/alsa_busy_probe; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$GATE" ] || [ "$CANDIDATE" -nt "$GATE" ]; then GATE="$CANDIDATE"; fi
done
test -n "$GATE" || { echo "Build with -DMMA_ALLOW_TEST_INPUTS=ON first"; exit 1; }

SHIM="$(dirname "$GATE")/libalsa_readi_shim.so"
test -f "$SHIM" || { echo "Missing $SHIM"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

HOME="$WORK" bash Tools/setup_alsa_fixture.sh "$WORK/fixture" >/dev/null

# `multi` over one file-backed slave: a PCM with exactly four channels, which
# is what hardware answers and what the plugin-only fixture devices cannot.
cat >> "$WORK/.asoundrc" <<CONF
pcm.mma_quad {
  type multi
  slaves.a { pcm { type file; slave.pcm "null"; file "/dev/null"; infile "$WORK/fixture/tone440.raw"; format "raw" } channels 4 }
  bindings { 0 { slave a; channel 0 } 1 { slave a; channel 1 } 2 { slave a; channel 2 } 3 { slave a; channel 3 } }
}
CONF

HOME="$WORK" \
MMA_SHIM_EXCLUSIVE=1 \
MMA_SIM_REALTIME=1 \
LD_PRELOAD="$PWD/$SHIM" \
  "./$GATE" mma_quad

echo
echo "ALSA busy probe gate passed."
