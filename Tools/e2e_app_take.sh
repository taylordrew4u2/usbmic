#!/usr/bin/env bash
# The end-to-end gate: the REAL app, headless, records a real take through the
# real ALSA backend against the virtual microphones from setup_alsa_fixture.sh,
# and the files it leaves are checked by verify_take.py.
#
#   Tools/e2e_app_take.sh [seconds-to-record] [extra verify_take.py args]
#
# Needs Xvfb, xdotool, ImageMagick. Every step waits for evidence of the state
# it expects (window up, take folder created, stop timestamp written) rather
# than a fixed sleep, so a slow start-up cannot turn a stop click into a start.
# Exit status is verify_take.py's.
set -euo pipefail

SECONDS_TO_RECORD="${1:-15}"
DISPLAY_NUM="${MMA_DISPLAY:-:99}"
APP=""
for CANDIDATE in build/SobStage_artefacts/Release/SobStage build/SobStage_artefacts/SobStage \
                 build-app/SobStage_artefacts/Release/SobStage build-app/SobStage_artefacts/SobStage; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$APP" ] || [ "$CANDIDATE" -nt "$APP" ]; then APP="$CANDIDATE"; fi
done
test -n "$APP" || { echo "Build the app first"; exit 1; }
echo "App: $APP (built $(date -r "$APP" '+%Y-%m-%d %H:%M:%S'))"

bash Tools/setup_alsa_fixture.sh >/dev/null
RECORDINGS="$HOME/RECORDINGS"
mkdir -p "$RECORDINGS"
BEFORE=$(ls -1 "$RECORDINGS" 2>/dev/null | sort | tail -1 || true)

cleanup() { kill "${APP_PID:-0}" 2>/dev/null || true; sleep 1; pkill Xvfb 2>/dev/null || true; }
trap cleanup EXIT

pkill Xvfb 2>/dev/null || true; sleep 1
Xvfb "$DISPLAY_NUM" -screen 0 1280x1200x24 >/dev/null 2>&1 &
sleep 2
DISPLAY="$DISPLAY_NUM" nohup "./$APP" >/tmp/mma-e2e-app.log 2>&1 &
APP_PID=$!

# 1. The window is up.
for i in $(seq 1 60); do
  if DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null 2>&1; then break; fi
  sleep 1
done
DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null || { echo "FAIL: window never appeared"; exit 1; }
sleep 4  # devices enumerate and streams open after the window shows

click() { DISPLAY="$DISPLAY_NUM" xdotool mousemove "$1" "$2" click 1; }

# A launch after an interrupted take shows the Recovered card first, and a
# card swallows every click behind it by design. Its Done button sits at
# (467,702) on this display; with no card up the click lands on an inert
# label. Either way the main screen is reachable afterwards.
click 467 702
sleep 1

newest() { ls -1 "$RECORDINGS" 2>/dev/null | sort | tail -1; }

# 2. Record. The button is at (1023,617) on a 1180-wide window centred on this
#    display. A take is proven started by its folder appearing.
TAKE=""
for attempt in 1 2 3; do
  click 1023 617
  for i in $(seq 1 10); do
    sleep 1
    CUR=$(newest)
    if [ -n "$CUR" ] && [ "$CUR" != "$BEFORE" ]; then TAKE="$CUR"; break; fi
  done
  [ -n "$TAKE" ] && break
  echo "record press $attempt did not start a take; trying again"
done
[ -n "$TAKE" ] || { echo "FAIL: no take folder appeared in $RECORDINGS"; exit 1; }
echo "take started: $TAKE"

sleep "$SECONDS_TO_RECORD"
DISPLAY="$DISPLAY_NUM" import -window root /tmp/mma-e2e-recording.png

# 3. Stop. A mid-take card, if one is up, swallows clicks by design: Escape is
#    its "keep recording", and the Keep button sits where the card puts it.
DISPLAY="$DISPLAY_NUM" xdotool key Escape
sleep 1
click 1023 617
STOPPED=""
for i in $(seq 1 20); do
  sleep 1
  if python3 -c "import json,sys; sys.exit(0 if json.load(open('$RECORDINGS/$TAKE/session.json')).get('stopTimestamp') else 1)" 2>/dev/null; then
    STOPPED=yes; break
  fi
done
DISPLAY="$DISPLAY_NUM" import -window root /tmp/mma-e2e-saved.png
[ -n "$STOPPED" ] || echo "WARN: no stop timestamp after 20 s (the verifier will fail on it)"

cleanup
trap - EXIT

# Two seconds of slack for start-up and finalisation.
python3 Tools/verify_take.py "$RECORDINGS/$TAKE" --seconds "$((SECONDS_TO_RECORD - 3))" \
  --tone mma_mic1-1=440 --tone mma_mic2-1=1000 "${@:2}"
