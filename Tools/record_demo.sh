#!/usr/bin/env bash
# Records the README demo from the real app: the main screen with live meters,
# Record, a few seconds of a take, Stop, and the saved-take card.
#
#   Tools/record_demo.sh [out-dir]      # default: docs/images
#
# Writes demo.mp4 (the video) and demo.gif (the inline README preview). Uses
# the virtual microphones from setup_alsa_fixture.sh, so the meters move
# without hardware -- which is also why the fixture output's warning line is
# visible. Needs Xvfb, xdotool and ffmpeg.
set -euo pipefail

OUT_DIR="${1:-docs/images}"
DISPLAY_NUM="${MMA_DISPLAY:-:99}"
APP=""
for CANDIDATE in build/SobStage_artefacts/Release/SobStage build/SobStage_artefacts/SobStage \
                 build-app/SobStage_artefacts/Release/SobStage build-app/SobStage_artefacts/SobStage; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$APP" ] || [ "$CANDIDATE" -nt "$APP" ]; then APP="$CANDIDATE"; fi
done
test -n "$APP" || { echo "Build the app first"; exit 1; }
echo "Recording: $APP (built $(date -r "$APP" '+%Y-%m-%d %H:%M:%S'))"

bash Tools/setup_alsa_fixture.sh >/dev/null

# A clean profile, so the demo shows what a new user sees rather than this
# machine's remembered destination or a recovery card from an earlier run.
REAL_HOME="$HOME"
DEMO_HOME=$(mktemp -d "${TMPDIR:-/tmp}/mma-demo.XXXXXX")
mkdir -p "$DEMO_HOME/RECORDINGS"
cp "$REAL_HOME/.asoundrc" "$DEMO_HOME/"
# In memory: the take opens its files at the same moment, and the two
# should not queue behind each other on one disk.
RAW="$(mktemp -d /dev/shm/mma-demo.XXXXXX)/raw.mp4"

APP_PID=""
FF_PID=""
cleanup() {
  [ -n "$FF_PID" ] && kill -INT "$FF_PID" 2>/dev/null || true
  [ -n "$APP_PID" ] && kill "$APP_PID" 2>/dev/null || true
  sleep 1
  pkill Xvfb 2>/dev/null || true
}
trap cleanup EXIT

pkill Xvfb 2>/dev/null || true; sleep 1
Xvfb "$DISPLAY_NUM" -screen 0 1280x1200x24 >/dev/null 2>&1 &
sleep 2
HOME="$DEMO_HOME" DISPLAY="$DISPLAY_NUM" nohup "./$APP" >"$DEMO_HOME/app.log" 2>&1 &
APP_PID=$!

for _ in {1..60}; do
  DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null 2>&1 && break
  sleep 1
done
sleep 6  # devices enumerate and the meters come alive
# The visible top-level window: JUCE also creates helper windows with the
# same name, and one of those can be gone by the time it is asked about.
WIN=$(DISPLAY="$DISPLAY_NUM" xdotool search --onlyvisible --name SobStage | tail -1)
test -n "$WIN" || { echo "window never appeared"; exit 1; }

# The size the README screenshots use, so the recording and stop states have
# room to show the take's details under the controls.
DISPLAY="$DISPLAY_NUM" xdotool windowsize "$WIN" 1180 620
sleep 2
eval "$(DISPLAY="$DISPLAY_NUM" xdotool getwindowgeometry --shell "$WIN")"
# Even dimensions: H.264 in yuv420p refuses odd ones.
W=$(( WIDTH / 2 * 2 )); H=$(( HEIGHT / 2 * 2 ))

click() { DISPLAY="$DISPLAY_NUM" xdotool mousemove $(( X + $1 )) $(( Y + $2 )) click 1; }

# Captured losslessly-ish at the lowest CPU cost and at low priority: the
# encoder shares the machine with the audio callback, and a starved callback
# would put a real "sound was dropped" alert into the demo.
nice -n 19 ffmpeg -loglevel error -y -f x11grab -draw_mouse 0 -framerate 15 -video_size "${W}x${H}" \
  -i "${DISPLAY_NUM}.0+${X},${Y}" -c:v libx264 -preset ultrafast -crf 12 -pix_fmt yuv420p "$RAW" &
FF_PID=$!

sleep 3                 # the live main screen
click 973 178           # Record
sleep 2
click 733 540           # "Start recording" on the first-run destination card
# The fixture's ALSA `file` microphones read their file faster than real time,
# so every fixture take overflows and the watchdog rightly raises its
# "sound was dropped" card. Real microphones are paced by their clocks and do
# not. Escape is that card's "Keep recording"; with no card up it does nothing.
sleep 2
DISPLAY="$DISPLAY_NUM" xdotool key Escape
sleep 7                 # the take running: clock, meters, files growing
DISPLAY="$DISPLAY_NUM" xdotool key Escape
sleep 1
click 973 178           # Stop
sleep 5                 # the saved-take card

kill -INT "$FF_PID"; wait "$FF_PID" 2>/dev/null || true; FF_PID=""

mkdir -p "$OUT_DIR"
ffmpeg -loglevel error -y -i "$RAW" -vf "scale=960:-2:flags=lanczos" \
  -c:v libx264 -preset slow -crf 26 -pix_fmt yuv420p -movflags +faststart -an "$OUT_DIR/demo.mp4"
ffmpeg -loglevel error -y -i "$RAW" \
  -vf "fps=10,scale=640:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=96[p];[b][p]paletteuse=dither=bayer:bayer_scale=4" \
  "$OUT_DIR/demo.gif"

ls -lh "$OUT_DIR/demo.mp4" "$OUT_DIR/demo.gif"
