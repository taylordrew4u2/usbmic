#!/usr/bin/env bash
# §14.2 could not fire, and only a test at this level could tell.
#
# "Bus power exhaustion doesn't produce a clean error; it shows up as
# enumeration failures and device drops." BusPowerDetector has always been able
# to count those, SetupAdvisor has always turned the count into "use a powered
# hub", and every unit test of both passed -- because the tests call the
# advisor directly. In the app, Application::noteDeviceDropout() was written,
# wired to the advisor, and called from NOWHERE, so the detector was handed an
# event exactly never and the advice could not be reached however many
# microphones dropped off a shared port.
#
# No unit test can catch that: they all pass with the feeder disconnected. This
# gate drives the real app with two of its three devices unable to open --
# 2 enumeration failures with 3 microphones attached, which is precisely the
# threshold -- and asserts the advice actually arrives.
#
#   Tools/e2e_bus_power.sh
set -euo pipefail

DISPLAY_NUM="${MMA_DISPLAY:-:99}"
FIFO_A="${TMPDIR:-/tmp}/mma-buspower-a"
FIFO_B="${TMPDIR:-/tmp}/mma-buspower-b"

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
cp "$ASOUNDRC" "$ASOUNDRC.bus-power-backup"

rm -f "$FIFO_A" "$FIFO_B"
mkfifo "$FIFO_A" "$FIFO_B"

# Both microphones become devices whose open can never complete. The output
# device still opens, so monitoring comes up and the app stays usable -- the
# point is the ADVICE, not a refusal.
python3 - "$ASOUNDRC" "$FIFO_A" "$FIFO_B" <<'PY'
import re, sys
path, a, b = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path).read()
for name, fifo in (('mma_mic1', a), ('mma_mic2', b)):
    s = re.sub(r'^pcm\.%s .*$' % name,
               'pcm.%s { type file; slave.pcm "null"; file "/dev/null"; infile "%s"; format "raw" }'
               % (name, fifo),
               s, flags=re.M)
open(path, 'w').write(s)
PY

LOG="$HOME/.config/SobStage/log.txt"
LOG_LINES_BEFORE=0
[ -f "$LOG" ] && LOG_LINES_BEFORE=$(wc -l < "$LOG")

cleanup() {
  kill "${APP_PID:-0}" 2>/dev/null || true
  sleep 1
  pkill Xvfb 2>/dev/null || true
  rm -f "$FIFO_A" "$FIFO_B"
  [ -f "$ASOUNDRC.bus-power-backup" ] && mv "$ASOUNDRC.bus-power-backup" "$ASOUNDRC"
}
trap cleanup EXIT

pkill Xvfb 2>/dev/null || true; sleep 1
Xvfb "$DISPLAY_NUM" -screen 0 1280x1200x24 >/dev/null 2>&1 &
sleep 2
DISPLAY="$DISPLAY_NUM" nohup "./$APP" >/tmp/mma-e2e-bus-power.log 2>&1 &
APP_PID=$!

for _ in $(seq 1 90); do
  DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null 2>&1 && break
  sleep 1
done
DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null || { echo "FAIL: window never appeared"; exit 1; }

new_log_lines() { tail -n "+$((LOG_LINES_BEFORE + 1))" "$LOG" 2>/dev/null; }

for _ in $(seq 1 30); do
  new_log_lines | grep -q "USB hub with its own power adapter" && break
  sleep 1
done

if ! new_log_lines | grep -q "USB hub with its own power adapter"; then
  DISPLAY="$DISPLAY_NUM" import -window root /tmp/mma-e2e-bus-power.png 2>/dev/null || true
  echo "FAIL: two microphones failed to open with three attached -- exactly §14.2's"
  echo "      threshold -- and the app never advised a powered hub."
  echo "      Nothing is feeding BusPowerDetector its events."
  echo "      (screen: /tmp/mma-e2e-bus-power.png)"
  new_log_lines | tail -5
  exit 1
fi

echo "PASS: §14.2 advised a powered hub after two devices failed to open"
echo "ALL CHECKS PASSED"
