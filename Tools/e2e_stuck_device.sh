#!/usr/bin/env bash
# §0.1 begins before the first take: an app that never opens cannot record.
#
# One microphone whose open never completes used to hold the WHOLE app closed.
# snd_pcm_open is a blocking open and it ran on the message thread, so the
# window never appeared -- no error, no way in, and no way to record with the
# microphones that were working perfectly well beside it. Reproduced with the
# main thread parked in fifo_open/wait_for_partner inside openat, two threads
# alive, and nothing on screen after twenty seconds.
#
# The wedged device here is a FIFO with no writer: a device that exists, that
# ALSA will happily be pointed at, and whose open cannot finish. SND_PCM_NONBLOCK
# does not save the probe -- that flag governs the PCM's data semantics, not the
# open of whatever backs the device -- which is why the enumeration probe wedged
# in exactly the same place.
#
#   Tools/e2e_stuck_device.sh
#
# Passes when the app opens anyway, says which microphone it gave up on, and
# records a real take from the healthy one.
set -euo pipefail

DISPLAY_NUM="${MMA_DISPLAY:-:99}"
# Measured, not picked: with a wedged device attached the window takes about
# 24 seconds to appear on an idle machine -- the bounded probe and the bounded
# open, in sequence, doing exactly what §0.1 asks of them. Against the 45 this
# used to allow, that is under 2x margin on a shared CI runner, and the gate
# duly failed once with no evidence of why. The wait costs nothing when it
# passes, because the loop ends the moment the window appears; the only thing a
# larger ceiling changes is how long a genuine hang takes to be reported.
WINDOW_DEADLINE="${MMA_WINDOW_DEADLINE:-120}"
STUCK_FIFO="${TMPDIR:-/tmp}/mma-stuck-device"

APP=""
for CANDIDATE in build/SobStage_artefacts/Release/SobStage build/SobStage_artefacts/SobStage \
                 build-app/SobStage_artefacts/Release/SobStage build-app/SobStage_artefacts/SobStage; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$APP" ] || [ "$CANDIDATE" -nt "$APP" ]; then APP="$CANDIDATE"; fi
done
test -n "$APP" || { echo "Build the app first"; exit 1; }
echo "App: $APP"

bash Tools/setup_alsa_fixture.sh >/dev/null
ASOUNDRC="$HOME/.asoundrc"
cp "$ASOUNDRC" "$ASOUNDRC.stuck-device-backup"

rm -f "$STUCK_FIFO"
mkfifo "$STUCK_FIFO"

# mma_mic2 becomes the device that never finishes opening. mma_mic1 stays
# healthy: the point is not that the app survives, it is that the working
# microphone still records.
python3 - "$ASOUNDRC" "$STUCK_FIFO" <<'PY'
import re, sys
path, fifo = sys.argv[1], sys.argv[2]
s = open(path).read()
s = re.sub(r'^pcm\.mma_mic2 .*$',
           'pcm.mma_mic2 { type file; slave.pcm "null"; file "/dev/null"; infile "%s"; format "raw" }' % fifo,
           s, flags=re.M)
open(path, 'w').write(s)
PY

cleanup() {
  kill "${APP_PID:-0}" 2>/dev/null || true
  sleep 1
  pkill Xvfb 2>/dev/null || true
  rm -f "$STUCK_FIFO"
  # Always put the machine's ALSA config back, including on a failure: this
  # script rewrites it, and leaving a wedged device behind would break every
  # later run on the same box.
  [ -f "$ASOUNDRC.stuck-device-backup" ] && mv "$ASOUNDRC.stuck-device-backup" "$ASOUNDRC"
}
trap cleanup EXIT

pkill Xvfb 2>/dev/null || true; sleep 1
Xvfb "$DISPLAY_NUM" -screen 0 1280x1200x24 >/dev/null 2>&1 &
sleep 2
LOG="$HOME/.config/SobStage/log.txt"
LOG_LINES_BEFORE=0
[ -f "$LOG" ] && LOG_LINES_BEFORE=$(wc -l < "$LOG")

DISPLAY="$DISPLAY_NUM" nohup "./$APP" >/tmp/mma-e2e-stuck.log 2>&1 &
APP_PID=$!

WAIT_BEGAN=$(date +%s)
WAITED_TICKS=0

WINDOW_APPEARED=0

for _ in $(seq 1 "$WINDOW_DEADLINE"); do
  if DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null 2>&1; then
    WINDOW_APPEARED=1
    break
  fi
  WAITED_TICKS=$((WAITED_TICKS + 1))
  sleep 1
done

# The loop's own answer, not a second question. Asking xdotool again is a race
# the gate lost twice in CI, both times at the 27-second mark that matches how
# long the window actually takes to appear here: the loop saw the window and
# broke, and the re-query a moment later did not, so a passing run was reported
# as "the app never opened its window" -- the one message that could not be
# true, since something had just seen it.
if [ "$WINDOW_APPEARED" -eq 0 ]; then
  echo "FAIL: the app never opened its window with one wedged device attached."
  echo "      This is the bug: a single microphone that will not finish opening"
  echo "      holds the whole app closed."
  echo
  # Said out loud because a failure here once arrived with none of it, and the
  # difference between "the app is wedged" and "the wait ended early" is not
  # something the old message could tell anyone. Guessing between them from
  # timestamps afterwards is not diagnosis.
  echo "      waited ${WAITED_TICKS} of ${WINDOW_DEADLINE} ticks, $(( $(date +%s) - WAIT_BEGAN ))s of wall clock"
  if kill -0 "$APP_PID" 2>/dev/null; then
    echo "      the app process is still alive (pid $APP_PID)"
  else
    wait "$APP_PID" 2>/dev/null || true
    echo "      the app process is GONE -- it exited rather than hanging, so this"
    echo "      is a crash or an early exit, not a blocked open"
  fi
  echo "      main thread stack:"
  cat "/proc/$APP_PID/task/$APP_PID/stack" 2>/dev/null | head -5 || echo "      (unavailable)"
  echo "      last of the app's own output:"
  tail -20 /tmp/mma-e2e-stuck.log 2>/dev/null | sed 's/^/        /' || true
  echo "      last of the activity log:"
  tail -20 "$LOG" 2>/dev/null | sed 's/^/        /' || true
  exit 1
fi
echo "PASS: the window opened with a wedged device attached (after ${WAITED_TICKS}s of the ${WINDOW_DEADLINE}s allowed)"

# Opening is necessary but not sufficient: the app must also SAY which device
# it gave up on and why, rather than presenting a microphone that silently does
# nothing. That sentence goes to the activity log, so it can be asserted without
# reading pixels.
LOG="$HOME/.config/SobStage/log.txt"

# Only the lines THIS run appended: the log persists between runs, and a line
# left by an earlier one would otherwise pass this test without the app having
# said anything at all.
new_log_lines() { tail -n "+$((LOG_LINES_BEFORE + 1))" "$LOG" 2>/dev/null; }

for _ in $(seq 1 20); do
  new_log_lines | grep -q "took too long to connect" && break
  sleep 1
done

if ! new_log_lines | grep -q "mma_mic2.*took too long to connect"; then
  echo "FAIL: the app opened but never said which device it gave up on."
  echo "      A microphone that is present, silent and unexplained is the"
  echo "      failure §0.1 exists to prevent."
  new_log_lines | tail -5 || echo "      (nothing new in $LOG)"
  exit 1
fi
echo "PASS: the wedged device was named, with what to do about it"

# The count has to be the microphones that OPENED. Carrying on without a device
# must not also mean claiming it is live: a count wrong in the user's favour is
# worse than no count, and this read "3 microphones are live" for a rig with a
# dead one until the live/selected distinction was made.
if ! new_log_lines | grep -q "2 microphones are live"; then
  echo "FAIL: the app did not report the number of microphones that actually opened."
  new_log_lines | grep -i "microphone" | tail -5
  exit 1
fi
echo "PASS: the live count is the microphones that opened, not the ones selected"

# The point of all of it: the working microphone still records. A wedged device
# used to close every other stream on its way out, so a rig with one dead
# microphone recorded NOTHING -- the largest loss §0.1 can take, arriving by the
# most ordinary way for a gig to go wrong.
RECORDINGS="$HOME/RECORDINGS"
mkdir -p "$RECORDINGS"
newest() { find "$RECORDINGS" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; 2>/dev/null | LC_ALL=C sort | tail -1; }
BEFORE=$(newest)

click() {
  local win geo x y
  win=$(DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage | head -1)
  geo=$(DISPLAY="$DISPLAY_NUM" xdotool getwindowgeometry --shell "$win")
  x=$(echo "$geo" | sed -n 's/^X=//p'); y=$(echo "$geo" | sed -n 's/^Y=//p')
  DISPLAY="$DISPLAY_NUM" xdotool mousemove $((x + $1)) $((y + $2)) click 1
}

sleep 3
for DONE_Y in 263 293 312 332; do click 417 "$DONE_Y"; done
sleep 1

TAKE=""
for attempt in 1 2 3; do
  click 973 178
  sleep 1
  click 733 540
  for _ in $(seq 1 10); do
    sleep 1
    CUR=$(newest); if [ -n "$CUR" ] && [ "$CUR" != "$BEFORE" ]; then TAKE="$CUR"; break; fi
  done
  [ -n "$TAKE" ] && break
  echo "record press $attempt did not start a take; trying again"
done

if [ -z "$TAKE" ]; then
  DISPLAY="$DISPLAY_NUM" import -window root /tmp/mma-e2e-stuck-no-record.png 2>/dev/null || true
  echo "FAIL: the app would not record at all with one wedged microphone attached."
  echo "      The microphones that DID open are working; refusing the take loses"
  echo "      the whole performance because one cable is bad."
  echo "      (screen: /tmp/mma-e2e-stuck-no-record.png)"
  exit 1
fi
echo "PASS: a take started with a wedged microphone attached ($TAKE)"

sleep 7
DISPLAY="$DISPLAY_NUM" xdotool key Escape
sleep 1
click 973 178
for _ in $(seq 1 20); do
  sleep 1
  python3 -c "import json,sys; sys.exit(0 if json.load(open('$RECORDINGS/$TAKE/session.json')).get('stopTimestamp') else 1)" 2>/dev/null && break
done

python3 Tools/verify_partial_take.py "$RECORDINGS/$TAKE" mic1 mic2

echo "ALL CHECKS PASSED"
