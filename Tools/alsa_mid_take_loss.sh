#!/usr/bin/env bash
# §0.1's hardest promise, on Linux: a microphone unplugged in the middle of a
# take must not take the take with it.
#
# Two earlier attempts at this failed, and the reason was always the fixture,
# never the code. The ALSA `file` plugin free-runs at thousands of times real
# time and LOOPS its infile, so it ignores truncation and cannot be made to
# disappear -- killing the FIFO behind it and truncating it both proved only
# that the plugin does not care. sim_capture_mac reached the event on the macOS
# path through the virtual HAL. Linux, the platform this actually ships on, had
# nothing.
#
# It does not need a virtual HAL. libasound is a shared library, so snd_pcm_readi
# can be replaced for ONE named PCM -- returning -ENODEV, which snd_pcm_recover
# cannot recover, exactly as a pulled cable does -- while every other call in the
# path stays the shipping code reading a real device.
#
#   Tools/alsa_mid_take_loss.sh
#
# The control is not optional: it is what says the failure case proved something
# about the unplug rather than about the harness.
set -euo pipefail

GATE=""
for CANDIDATE in build/alsa_mid_take_loss build-app/alsa_mid_take_loss; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$GATE" ] || [ "$CANDIDATE" -nt "$GATE" ]; then GATE="$CANDIDATE"; fi
done
test -n "$GATE" || { echo "Build with -DMMA_ALLOW_TEST_INPUTS=ON first"; exit 1; }

SHIM="$(dirname "$GATE")/libalsa_readi_shim.so"
test -f "$SHIM" || { echo "Missing $SHIM"; exit 1; }

bash Tools/setup_alsa_fixture.sh >/dev/null

echo "=== Control: two healthy microphones, nothing unplugged ==="
"./$GATE"

echo
echo "=== One microphone unplugged part way through the take ==="
# Two seconds of real reading before the device goes, so this is a device lost
# MID-take rather than one that was never there: a mic that failed from the
# first read would pass a weaker test while proving much less.
MMA_SHIM_MODE=dead \
MMA_SHIM_DEVICE=mma_mic2 \
MMA_SHIM_FAIL_AFTER_MS=2000 \
LD_PRELOAD="$PWD/$SHIM" \
  "./$GATE"

echo
echo "ALSA mid-take loss gate passed."
