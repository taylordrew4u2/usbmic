#!/usr/bin/env bash
# §5.4: monitoring that cannot run at the take's rate must say so, and say the
# rate, before the take rather than after it.
#
# The capability check opened the device and, if the open worked, declared
# exclusive monitoring available at whatever rate it was handed. The open
# proves the device is there; it proves nothing about the rate.
#
# Reaching that code on Linux needs two things no fixture on a CI box has: a
# direct-hardware output name, and a device that refuses a rate. The shim
# supplies both — it redirects an allowlisted hw: name onto the fixture's PCM
# and makes hw_params_test_rate say no to one rate — so the name policy, the
# bounded open, the negotiation and the message are all the shipping code.
#
#   Tools/alsa_rate_refusal.sh
#
# The control is not optional: the accepted-rate run is what says the refusal
# proved something about the rate rather than about the harness.
set -euo pipefail

REFUSED=48000
ACCEPTED=44100

GATE=""
for CANDIDATE in build/alsa_rate_refusal build-app/alsa_rate_refusal; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$GATE" ] || [ "$CANDIDATE" -nt "$GATE" ]; then GATE="$CANDIDATE"; fi
done
test -n "$GATE" || { echo "Build with -DMMA_ALLOW_TEST_INPUTS=ON first"; exit 1; }

SHIM="$(dirname "$GATE")/libalsa_readi_shim.so"
test -f "$SHIM" || { echo "Missing $SHIM"; exit 1; }

bash Tools/setup_alsa_fixture.sh >/dev/null

MMA_SHIM_OPEN_MATCH=plughw:99 \
MMA_SHIM_OPEN_AS=mma_out \
MMA_SHIM_REFUSE_RATE="$REFUSED" \
LD_PRELOAD="$PWD/$SHIM" \
  "./$GATE" "$REFUSED" "$ACCEPTED"

echo
echo "ALSA rate refusal gate passed."
