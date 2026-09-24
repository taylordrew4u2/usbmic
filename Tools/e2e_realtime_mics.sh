#!/usr/bin/env bash
# The microphone simulator: the real app records from virtual microphones that
# run on real, independent clocks -- one 150 ppm fast, one 150 ppm slow -- the
# way two USB microphones' crystals disagree.
#
# The plain fixture cannot say anything about drift or about lost audio. Its
# microphones are ALSA `file` devices on the clockless `null` slave, so they
# deliver audio as fast as the file can be read: every fixture take overflows,
# and the dropped-sound card fires on every take whatever the app does. Here
# Tools/alsa_readi_shim.cpp paces every PCM to its own clock (MMA_SIM_REALTIME,
# MMA_SIM_PPM), so a take that loses audio is a real finding.
#
#   Tools/e2e_realtime_mics.sh [seconds]     # default 75
#
# Checks, on the take the real app writes:
#   - verify_take.py: each stem holds its own microphone's tone, the mix holds
#     both, durations and headers are right
#   - nothing was dropped: no overflow, no writer drops, no backend drops
#   - with 60 s or more, the app's own drift measurement is in session.json and
#     agrees with the clocks it was given: opposite signs, about 300 ppm apart
set -euo pipefail

SECONDS_TO_RECORD="${1:-75}"
PPM_A="${MMA_SIM_PPM_A:-150}"
PPM_B="${MMA_SIM_PPM_B:--150}"

SHIM=""
for CANDIDATE in build/libalsa_readi_shim.so build-app/libalsa_readi_shim.so; do
  [ -e "$CANDIDATE" ] || continue
  if [ -z "$SHIM" ] || [ "$CANDIDATE" -nt "$SHIM" ]; then SHIM="$CANDIDATE"; fi
done
test -n "$SHIM" || { echo "Build with -DMMA_ALLOW_TEST_INPUTS=ON first (no libalsa_readi_shim.so)"; exit 1; }

export MMA_SIM_REALTIME=1
export MMA_SIM_PPM="mma_mic1=${PPM_A},mma_mic2=${PPM_B},mma_out=0"
export MMA_APP_LD_PRELOAD="$PWD/$SHIM"
echo "Microphone clocks: $MMA_SIM_PPM"

# The app monitors from launch, and §5.4's buffer ladder steps up on three
# ring-loss events inside thirty seconds. On a machine whose scheduling jitter
# is wider than the 64-sample cushion that happens while monitoring, before
# the take -- given the time. Forty-five seconds is one ladder window plus the
# reopen, so a take on such a machine starts at the size the machine needs.
export MMA_SETTLE_SECONDS="${MMA_SETTLE_SECONDS:-45}"

# The fixture's tone files are 30 s long and the `file` plugin loops them, so a
# longer take is fine; verify_take.py only needs each tone present.
bash Tools/e2e_app_take.sh "$SECONDS_TO_RECORD"

TAKE="$HOME/RECORDINGS/$(find "$HOME/RECORDINGS" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; | LC_ALL=C sort | tail -1)"
echo
echo "=== Real-time clocks: $(basename "$TAKE") ==="

python3 - "$TAKE/session.json" "$SECONDS_TO_RECORD" "$PPM_A" "$PPM_B" <<'PY'
import json, sys
path, seconds, ppm_a, ppm_b = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
j = json.load(open(path))
failed = 0

print("  buffer at take: %s samples; ladder steps before it: %s" % (
    j.get("bufferSizeSamples"),
    ["%s->%s at %.0fs" % (c.get("oldBufferSize"), c.get("newBufferSize"), c.get("timestampSeconds", 0))
     for c in j.get("bufferChanges", [])] or "none"))

def check(ok, what):
    global failed
    print(("  PASS  " if ok else "  FAIL  ") + what)
    failed += 0 if ok else 1

lost = [d for d in j.get("dropouts", [])
        if d.get("description", "").startswith("Dropped")
        or "could not keep up" in d.get("description", "")]
check(not lost, "nothing was dropped on real-time clocks"
      + ("" if not lost else ": " + "; ".join(d["description"] for d in lost)))

if seconds >= 65:
    drift = {}
    for entry in j.get("driftLog", []):
        drift[entry.get("deviceUsbId") or entry.get("usbId") or entry.get("device")] = entry.get("driftPpm", entry.get("ppm"))
    print("  app measured:", drift)
    a, b = drift.get("mma_mic1"), drift.get("mma_mic2")
    check(a is not None and b is not None, "the app measured both microphones' drift")
    if a is not None and b is not None:
        want = abs(ppm_a - ppm_b)
        got = abs(a - b)
        check(abs(got - want) <= 0.25 * want,
              "measured spread %.0f ppm against the %.0f ppm the clocks were given" % (got, want))
        check((a > 0) != (b > 0), "the fast and the slow microphone drift in opposite directions")
else:
    print("  (drift is only recorded after 60 s of measurement; run with 65 s or more to check it)")

sys.exit(1 if failed else 0)
PY
echo "Real-time microphone gate passed."
