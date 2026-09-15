#!/usr/bin/env bash
# §0.1 from the monitoring side, on Linux: headphones that stop taking audio in
# the middle of a take must not take the take with them.
#
# sim_capture_mac and sim_capture_win each make this claim on their own
# platform. Linux could not make it at all: nothing had ever run the playback
# half of AlsaBackend's worker loop, because the existing harness passes no
# output device. snd_pcm_writei, the recovered -EPIPE path that counts
# outputGlitches, and the -ENODEV path that gives up and reports were all
# unreachable from any test.
#
# The shim that already replaces snd_pcm_readi for one named PCM does the same
# for snd_pcm_writei under MMA_SHIM_STREAM=playback, so the output dies exactly
# as a pulled headphone cable makes it die while every other call in the path
# stays the shipping code.
#
#   Tools/alsa_monitor_loss.sh
#
# The control is not optional: it is what says the failure case proved something
# about the loss rather than about the harness.
set -euo pipefail

GATE=""
for CANDIDATE in build/alsa_monitor_loss build-app/alsa_monitor_loss; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$GATE" ] || [ "$CANDIDATE" -nt "$GATE" ]; then GATE="$CANDIDATE"; fi
done
test -n "$GATE" || { echo "Build with -DMMA_ALLOW_TEST_INPUTS=ON first"; exit 1; }

SHIM="$(dirname "$GATE")/libalsa_readi_shim.so"
test -f "$SHIM" || { echo "Missing $SHIM"; exit 1; }

bash Tools/setup_alsa_fixture.sh >/dev/null

echo "=== Control: a take with the headphones left alone ==="
"./$GATE"

echo
echo "=== The headphones stop taking audio part way through the take ==="
# Two seconds of real writing before the output goes, so this is monitoring lost
# MID-take rather than headphones that never worked: an output that failed from
# the first write would pass a weaker test while proving much less.
MMA_SHIM_MODE=dead \
MMA_SHIM_STREAM=playback \
MMA_SHIM_DEVICE=mma_out \
MMA_SHIM_FAIL_AFTER_MS=2000 \
LD_PRELOAD="$PWD/$SHIM" \
  "./$GATE"

echo
echo "ALSA monitor loss gate passed."
