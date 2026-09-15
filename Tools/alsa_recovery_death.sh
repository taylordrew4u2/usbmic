#!/usr/bin/env bash
# §0.1: a microphone whose PCM fails and recovers on every read must be given
# up on, not spun on for the rest of the take.
#
# snd_pcm_recover succeeding says the PCM was put back into a runnable state,
# not that the device works. A PCM in that state reaches neither of the worker
# loop's exits -- the failure is recoverable, and no read returns zero -- so the
# loop used to spin there with the mic written as silence and only a rising
# dropped-frame count to show for it.
#
# The ALSA `file` plugin the fixture is built on cannot express this: it
# free-runs, loops its infile, and never xruns. So one symbol is interposed --
# snd_pcm_readi -- and everything else in the path stays the shipping code,
# against a real libasound.
#
#   Tools/alsa_recovery_death.sh
#
# Two runs, and the control is not optional: a rule that also fired on a healthy
# device would pass the first run while being a worse bug than the one it fixes.
set -euo pipefail

GATE=""
for CANDIDATE in build/alsa_recovery_death build-app/alsa_recovery_death; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$GATE" ] || [ "$CANDIDATE" -nt "$GATE" ]; then GATE="$CANDIDATE"; fi
done
test -n "$GATE" || { echo "Build with -DMMA_ALLOW_TEST_INPUTS=ON first"; exit 1; }

SHIM="$(dirname "$GATE")/libalsa_readi_shim.so"
test -f "$SHIM" || { echo "Missing $SHIM"; exit 1; }

bash Tools/setup_alsa_fixture.sh >/dev/null

echo "=== Control: a healthy fixture microphone ==="
"./$GATE"

echo
echo "=== A PCM that fails and recovers on every read ==="
# A few real reads first, so the counter's reset on success is exercised rather
# than assumed: a rule that only worked from the very first read would pass a
# test that never let one through.
MMA_EXPECT_STREAM_DEATH=1 \
MMA_SHIM_FAIL_AFTER=5 \
LD_PRELOAD="$PWD/$SHIM" \
  "./$GATE"

echo
echo "ALSA recovery gate passed."
