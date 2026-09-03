#!/usr/bin/env bash
# Render the app headless and capture it, so UI changes can be looked at rather
# than reasoned about. Requires Xvfb and ImageMagick's `import`:
#
#   apt-get install -y xvfb imagemagick xdotool
#
# Usage:  Tools/screenshot_app.sh out.png [seconds-to-settle]
#         MMA_CLICK=x,y Tools/screenshot_app.sh out.png   # click first
#
# Pair it with Tools/setup_alsa_fixture.sh to get virtual microphones, so the
# channel strips render with devices present instead of the empty-rig state.
set -euo pipefail

OUT="${1:-app.png}"
SETTLE="${2:-15}"
DISPLAY_NUM="${MMA_DISPLAY:-:99}"
# Multi-config generators (Xcode, Visual Studio) put the product under a
# per-configuration directory; single-config ones -- which is what the build
# command in the README produces -- put it straight in the artefacts folder.
# Both are looked for, so this works from the documented build rather than only
# from the generator the script was first written against.
#
# The README builds into `build`; this script was originally written against
# `build-app`. Both are looked in, and when both exist the NEWEST binary wins
# rather than whichever directory happens to be listed first -- otherwise a
# stale artefact in one directory silently gets photographed while the change
# under test sits unbuilt in the other. That is not hypothetical: it produced a
# screenshot presented as evidence for a change the binary did not contain.
APP=""
for CANDIDATE in \
  "build/SobStage_artefacts/Release/SobStage" \
  "build/SobStage_artefacts/SobStage" \
  "build-app/SobStage_artefacts/Release/SobStage" \
  "build-app/SobStage_artefacts/SobStage"
do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$APP" ] || [ "$CANDIDATE" -nt "$APP" ]; then APP="$CANDIDATE"; fi
done

test -n "$APP" || { echo "Build first: cmake -B build -DMMA_BUILD_APP=ON && cmake --build build"; exit 1; }

# Always say which binary is being photographed and when it was built. A
# screenshot is only evidence if it is of the code under test, and the age is
# what makes a forgotten rebuild obvious instead of invisible.
echo "Capturing: $APP (built $(date -r "$APP" '+%Y-%m-%d %H:%M:%S'))"

# `pkill Xvfb` matches the process name only. Do not reach for `pkill -f` here:
# the pattern would also match the shell running this script, and killing the
# caller is a confusing way to fail.
pkill Xvfb 2>/dev/null || true
sleep 1

Xvfb "$DISPLAY_NUM" -screen 0 1280x900x24 >/dev/null 2>&1 &
sleep 2

DISPLAY="$DISPLAY_NUM" nohup "./$APP" >/tmp/mma-screenshot-app.log 2>&1 &
APP_PID=$!

sleep "$SETTLE"

# Optional: click somewhere before capturing, so screens behind a button --
# Settings, most of all -- can be captured reproducibly instead of by an
# ad-hoc xdotool invocation that is retyped from memory each time.
#   MMA_CLICK=918,678 Tools/screenshot_app.sh settings.png
if [ -n "${MMA_CLICK:-}" ]; then
  DISPLAY="$DISPLAY_NUM" xdotool mousemove "${MMA_CLICK%%,*}" "${MMA_CLICK##*,}" click 1
  sleep 2
fi

DISPLAY="$DISPLAY_NUM" import -window root "$OUT"

kill "$APP_PID" 2>/dev/null || true
pkill Xvfb 2>/dev/null || true

echo "wrote $OUT"
