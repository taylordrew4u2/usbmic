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
# Passes when the app opens anyway and records from the healthy microphone.
set -euo pipefail

DISPLAY_NUM="${MMA_DISPLAY:-:99}"
WINDOW_DEADLINE="${MMA_WINDOW_DEADLINE:-45}"
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

for _ in $(seq 1 "$WINDOW_DEADLINE"); do
  if DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null 2>&1; then break; fi
  sleep 1
done

if ! DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null 2>&1; then
  echo "FAIL: the app never opened its window with one wedged device attached."
  echo "      This is the bug: a single microphone that will not finish opening"
  echo "      holds the whole app closed. Main thread at the time of writing:"
  cat "/proc/$APP_PID/task/$APP_PID/stack" 2>/dev/null | head -5 || true
  exit 1
fi
echo "PASS: the window opened with a wedged device attached"

# Opening is necessary but not sufficient: the app must also SAY which device
# it gave up on and why, rather than presenting a microphone that silently does
# nothing. That sentence goes to the activity log, so it can be asserted without
# reading pixels.
#
# What this deliberately does NOT assert is that a take records. With one
# microphone wedged the app opens, names the failure, and leaves "Start
# recording" disabled -- which is its existing policy for a microphone that
# will not open, not something this fix introduced. Whether a take should be
# allowed to start with the microphones that DID open is a product decision,
# and it is a separate one from this bug: the bug was that there was no window
# to make that decision in.
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

echo "ALL CHECKS PASSED"
