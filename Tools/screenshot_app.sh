#!/usr/bin/env bash
# Render the app headless and capture it, so UI changes can be looked at rather
# than reasoned about. Requires Xvfb and ImageMagick's `import`:
#
#   apt-get install -y xvfb imagemagick xdotool
#
# Usage:  Tools/screenshot_app.sh out.png [seconds-to-settle]
#
# Pair it with Tools/setup_alsa_fixture.sh to get virtual microphones, so the
# channel strips render with devices present instead of the empty-rig state.
set -euo pipefail

OUT="${1:-app.png}"
SETTLE="${2:-15}"
DISPLAY_NUM="${MMA_DISPLAY:-:99}"
APP="build-app/MultiMicAggregator_artefacts/Release/Multi-Mic Aggregator"

test -x "$APP" || { echo "Build first: cmake -B build-app -DMMA_BUILD_APP=ON && cmake --build build-app"; exit 1; }

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
DISPLAY="$DISPLAY_NUM" import -window root "$OUT"

kill "$APP_PID" 2>/dev/null || true
pkill Xvfb 2>/dev/null || true

echo "wrote $OUT"
