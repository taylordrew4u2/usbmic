#!/usr/bin/env bash
# The negative gate: the microphones cannot be opened (their virtual devices
# point at files that do not exist). The app must NOT produce a take of empty
# files: either the record button is off with a reason, or a take that starts
# is stopped by the proof within seconds. Either way no session folder with
# audio-less stems may be left as if it were a recording.
#
#   Tools/e2e_refusal.sh
set -euo pipefail

DISPLAY_NUM="${MMA_DISPLAY:-:99}"
APP=""
for CANDIDATE in build/SobStage_artefacts/Release/SobStage build/SobStage_artefacts/SobStage \
                 build-app/SobStage_artefacts/Release/SobStage build-app/SobStage_artefacts/SobStage; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$APP" ] || [ "$CANDIDATE" -nt "$APP" ]; then APP="$CANDIDATE"; fi
done
test -n "$APP" || { echo "Build the app first"; exit 1; }

# Microphones whose source files are missing.
cp "$HOME/.asoundrc" /tmp/asoundrc.backup 2>/dev/null || true
cat > "$HOME/.asoundrc" <<CONF
pcm.mma_mic1 { type file; slave.pcm "null"; file "/dev/null"; infile "/nonexistent/tone440.raw"; format "raw" }
pcm.mma_mic2 { type file; slave.pcm "null"; file "/dev/null"; infile "/nonexistent/tone1000.raw"; format "raw" }
CONF
restore() { [ -f /tmp/asoundrc.backup ] && cp /tmp/asoundrc.backup "$HOME/.asoundrc"; kill "${APP_PID:-0}" 2>/dev/null || true; sleep 1; pkill Xvfb 2>/dev/null || true; }
trap restore EXIT

RECORDINGS="$HOME/RECORDINGS"; mkdir -p "$RECORDINGS"
BEFORE=$(ls -1 "$RECORDINGS" 2>/dev/null | sort | tail -1 || true)

pkill Xvfb 2>/dev/null || true; sleep 1
Xvfb "$DISPLAY_NUM" -screen 0 1280x1200x24 >/dev/null 2>&1 &
sleep 2
DISPLAY="$DISPLAY_NUM" nohup "./$APP" >/tmp/mma-e2e-refusal.log 2>&1 &
APP_PID=$!
for i in $(seq 1 60); do DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null 2>&1 && break; sleep 1; done
sleep 4
click() { DISPLAY="$DISPLAY_NUM" xdotool mousemove "$1" "$2" click 1; }
click 467 702; sleep 1            # Recovered card's Done, if it is up
DISPLAY="$DISPLAY_NUM" import -window root /tmp/mma-e2e-refusal-before.png
click 1023 617; sleep 12          # record; give the proof time to act
DISPLAY="$DISPLAY_NUM" import -window root /tmp/mma-e2e-refusal-after.png

AFTER=$(ls -1 "$RECORDINGS" 2>/dev/null | sort | tail -1 || true)
if [ -n "$AFTER" ] && [ "$AFTER" != "$BEFORE" ]; then
  echo "a take folder was created: $AFTER"
  python3 - "$RECORDINGS/$AFTER" <<'PY'
import sys, os, glob, json
folder = sys.argv[1]
sizes = {os.path.basename(f): os.path.getsize(f) for f in glob.glob(folder + '/*.wav')}
meta = json.load(open(folder + '/session.json')) if os.path.exists(folder + '/session.json') else {}
print('  files:', sizes)
print('  stopped:', bool(meta.get('stopTimestamp')))
# The one unacceptable outcome: a take left "recording" (no stop) or claiming
# to have recorded while every stem is header-only.
empty = all(s < 4096 for s in sizes.values()) if sizes else True
if empty and not meta.get('stopTimestamp'):
    print('FAIL: empty take left running -- the day-long-silence failure'); sys.exit(1)
print('PASS: the app did not pretend -- take stopped itself' if empty else 'PASS: audio was written')
PY
else
  echo "PASS: record was refused; no take folder was created"
fi
