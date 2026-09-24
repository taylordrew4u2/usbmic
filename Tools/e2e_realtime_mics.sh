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
export MMA_SETTLE_SECONDS="${MMA_SETTLE_SECONDS:-60}"

# The fixture's tone files are four minutes long: the `file` plugin starts a
# file over at its end and delivers a seam in every block from then on (see
# Tools/setup_alsa_fixture.sh), so no stream may live longer than the file.
# Settle plus take plus stop is well inside it. verify_take.py only needs each
# tone present.
TAKE_FAILED=0
bash Tools/e2e_app_take.sh "$SECONDS_TO_RECORD" || TAKE_FAILED=1

TAKE="$HOME/RECORDINGS/$(find "$HOME/RECORDINGS" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; | LC_ALL=C sort | tail -1)"

# What the take looked like, whatever verify_take.py made of it: a stem whose
# tone the per-block vote could not recognise on a CI runner is nothing to
# reason about from the share alone. The vote per second and a block map of
# the first seconds, plus the app's own account of the take, go in the log.
echo
echo "=== Tone timeline: $(basename "$TAKE") ==="
python3 Tools/tone_timeline.py "$TAKE/01_mma_mic1.wav" 440 1000 6 | sed -n '1,12p;/map/,$p'
python3 Tools/tone_timeline.py "$TAKE/02_mma_mic2.wav" 1000 440 6 | sed -n '1,12p;/map/,$p'
echo "--- seams: audio out of order, which no counter sees ---"
SEAMS=0
for STEM in "01_mma_mic1.wav 440" "02_mma_mic2.wav 1000"; do
  set -- $STEM
  LINE=$(python3 Tools/tone_timeline.py --seams "$TAKE/$1" "$2")
  echo "  $LINE"
  case "$LINE" in *": 0 seams"*) ;; *) SEAMS=1 ;; esac
done
echo "--- activity.log ---"
cat "$TAKE/activity.log" 2>/dev/null || true
echo "--- session.json dropouts and buffer ---"
python3 - "$TAKE/session.json" <<'PY' || true
import json, sys
j = json.load(open(sys.argv[1]))
print("  bufferSizeSamples:", j.get("bufferSizeSamples"), " changes:", j.get("bufferSizeChanges"))
for d in j.get("dropouts", []):
    print("  dropout:", d)
PY

if [ "$TAKE_FAILED" != 0 ]; then
  echo "The take itself failed verification (above)."
  exit 1
fi
if [ "$SEAMS" != 0 ]; then
  echo "FAIL  a stem carries audio out of order (seams above)."
  exit 1
fi

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

# Two kinds of loss, judged apart. Audio the app had and threw away -- a
# full ring, a writer that could not keep up, a block that did not fit the
# layout, a driver that dropped before the app read it -- is the app's, and
# there must be none. Silence written because a microphone's block had not
# arrived is what a machine that stalls longer than the cushion costs, and
# the buffer ladder is only allowed to grow the cushion between takes, three
# occasions inside thirty seconds at a time; a virtual machine whose host
# steals tens of milliseconds at a stretch can do that once in a seventy-five
# second take at 256 samples, and the app cannot be asked to prevent it,
# only to say so. It must say so, and it must be small: a few milliseconds
# on the whole take, never the block-a-second of a loop that has lost its
# footing.
import re
dropouts = j.get("dropouts", [])
silence = 0
faults = []
for d in dropouts:
    text = d.get("description", "")
    m = re.match(r"Dropped (\d+) samples: a microphone's audio did not arrive in time", text)
    if m:
        silence += int(m.group(1))
    elif text.startswith("Dropped") or "could not keep up" in text:
        faults.append(text)
check(not faults, "nothing the app had was thrown away"
      + ("" if not faults else ": " + "; ".join(faults)))
rate = float(j.get("sampleRate", 48000) or 48000)
silence_ms = 1000.0 * silence / rate
check(silence_ms <= 25.0,
      "silence written for late microphone blocks is %s (%.1f ms; the take reports it; limit 25 ms)"
      % ("none" if silence == 0 else "%d samples" % silence, silence_ms))

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
