#!/usr/bin/env bash
# The real app walks every one of its own screens, the way a person would, on
# the virtual microphones from setup_alsa_fixture.sh. Source/UI/UiWalker.h does
# the walking from inside the process -- it finds each button, tick box, picker
# and slider in the live window rather than clicking fixed pixels -- and writes
# every check to a report this script reads.
#
# Five launches, each on a fresh profile of its own:
#   1. The full walk: Settings, Help, Cameras, every control moved and put
#      back, a microphone renamed, diagnostics exported twice, a real take
#      recorded through a mid-take buffer change and a mid-take bit depth and
#      rate change (its session.json must still match MIX.wav), the
#      saved-take card, a
#      second take whose mid-take buffer change must be applied at Stop, every
#      remaining button, and a clean quit.
#   2. A crash: record, then SIGKILL the app mid-take.
#   3. The next launch after that crash: the interrupted take must be offered
#      back before anything else, and the full walk must pass again.
#   4. A microphone dies mid-take: the alert card must come up, Keep recording
#      must keep the take going, and the take must still stop and save. The
#      microphone then comes back, and Stop must reopen it: the second take
#      has to carry its tone, not silence.
#   5. A take whose files never grow: the app's own stop, its red card on top,
#      and the saved-take card only after OK.
#
#   Tools/e2e_ui_walk.sh
#
# Needs a build configured with -DMMA_ALLOW_TEST_INPUTS=ON, and Xvfb.
set -euo pipefail

DISPLAY_NUM="${MMA_DISPLAY:-:99}"
APP=""
for CANDIDATE in build/SobStage_artefacts/Release/SobStage build/SobStage_artefacts/SobStage \
                 build-app/SobStage_artefacts/Release/SobStage build-app/SobStage_artefacts/SobStage; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$APP" ] || [ "$CANDIDATE" -nt "$APP" ]; then APP="$CANDIDATE"; fi
done
test -n "$APP" || { echo "Build the app first"; exit 1; }
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"
echo "App: $APP"

bash Tools/setup_alsa_fixture.sh >/dev/null
ASOUNDRC="$HOME/.asoundrc"

# The fixture's microphones hand over audio as fast as memory copies go. The
# shim paces each one to its own clock, so the walk's takes run in real time
# the way a person's would (see e2e_realtime_mics.sh).
SHIM=""
for CANDIDATE in build-app/libalsa_readi_shim.so build/libalsa_readi_shim.so; do
  [ -f "$CANDIDATE" ] && { SHIM="$PWD/$CANDIDATE"; break; }
done
test -n "$SHIM" || { echo "Build with -DMMA_ALLOW_TEST_INPUTS=ON first (no libalsa_readi_shim.so)"; exit 1; }

WORK="$(mktemp -d)"
APP_PID=""
cleanup() {
  [ -n "$APP_PID" ] && kill -9 "$APP_PID" 2>/dev/null || true
  pkill Xvfb 2>/dev/null || true
}
trap cleanup EXIT

pkill Xvfb 2>/dev/null || true; sleep 1
Xvfb "$DISPLAY_NUM" -screen 0 1600x1200x24 >/dev/null 2>&1 &
sleep 2

FAILED=0

# A profile of its own: settings, recordings and the Desktop the diagnostics
# zip lands on, with the fixture's ALSA configuration carried over.
fresh_home() {
  local home="$WORK/$1"
  mkdir -p "$home/Desktop"
  cp "$ASOUNDRC" "$home/.asoundrc"
  echo "$home"
}

SNAPSHOTS="${MMA_UI_WALK_SNAPSHOT_DIR:-/tmp/mma-ui-walk-snapshots}"
rm -rf "$SNAPSHOTS"; mkdir -p "$SNAPSHOTS"

# launch <home> <report> [extra env...]; sets APP_PID
launch() {
  local home="$1" report="$2"; shift 2
  env HOME="$home" DISPLAY="$DISPLAY_NUM" MMA_UI_WALK_REPORT="$report" \
    MMA_UI_WALK_SNAPSHOT_DIR="$SNAPSHOTS" \
    LD_PRELOAD="$SHIM" MMA_SIM_REALTIME=1 MMA_SIM_PPM="mma_mic1=150,mma_mic2=-150,mma_out=0" "$@" \
    "$APP" >"$report.log" 2>&1 &
  APP_PID=$!
}

# wait_for_exit <seconds>; returns the app's exit status, or 124 on timeout
wait_for_exit() {
  local limit="$1"
  for ((i = 0; i < limit; i++)); do
    if ! kill -0 "$APP_PID" 2>/dev/null; then
      local status=0
      wait "$APP_PID" || status=$?
      APP_PID=""
      return "$status"
    fi
    sleep 1
  done
  return 124
}

# judge <label> <report> <exit status>
judge() {
  local label="$1" report="$2" status="$3"
  echo "---- $label ----"
  cat "$report" 2>/dev/null || echo "(no report written)"
  if ! grep -q "^UI-WALK DONE .*failures=0$" "$report" 2>/dev/null; then
    echo "FAIL: $label: the walk did not finish cleanly"
    FAILED=1
  fi
  if [ "$status" -ne 0 ]; then
    echo "FAIL: $label: the app exited with status $status after the walk (log: $report.log)"
    tail -20 "$report.log" || true
    FAILED=1
  fi
}

# 1. The full walk.
HOME1="$(fresh_home walk)"
launch "$HOME1" "$WORK/walk.txt"
STATUS=0; wait_for_exit 420 || STATUS=$?
judge "full walk" "$WORK/walk.txt" "$STATUS"

# The full walk records two takes: the first with a buffer change, a format
# change and renames in the middle of it, the second after Stop has applied
# them. Every one is verified -- checking only the newest would leave the
# first, the one most likely to break, unchecked.
TAKES="$(find "$HOME1/RECORDINGS" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | LC_ALL=C sort)"
TAKE_COUNT="$(printf '%s\n' "$TAKES" | grep -c . || true)"
if [ "$TAKE_COUNT" -lt 2 ]; then
  echo "FAIL: the walk left $TAKE_COUNT take folder(s), expected 2"; FAILED=1
fi
# The first take also has its local backup unticked part way through, so its
# copy is short on purpose and its record must say so.
FIRST_TAKE=1
while IFS= read -r TAKE; do
  [ -n "$TAKE" ] || continue
  echo "---- the walk's take: $TAKE ----"
  EXPECT=()
  if [ "$FIRST_TAKE" = 1 ]; then EXPECT=(--expect-loss "Local backup copy turned off in Settings"); fi
  FIRST_TAKE=0
  # The walk renames microphone 1 "Walker Vox", so its tone is found by that.
  python3 Tools/verify_take.py "$TAKE" --seconds 3 --silent-ok mma_out \
    --tone Walker=440 --tone mma_mic2=1000 --mirror-root "$HOME1/RECORDINGS-MIRROR" ${EXPECT[@]+"${EXPECT[@]}"} || {
    echo "FAIL: the take $TAKE recorded during the walk does not verify"; FAILED=1; }
done <<< "$TAKES"

# 2. A crash in the middle of a take.
HOME2="$(fresh_home crash)"
launch "$HOME2" "$WORK/crash.txt" MMA_UI_WALK_MODE=crash
RECORDING=""
for _ in {1..90}; do
  if grep -q "^RECORDING " "$WORK/crash.txt" 2>/dev/null; then RECORDING=yes; break; fi
  kill -0 "$APP_PID" 2>/dev/null || break
  sleep 1
done
if [ -z "$RECORDING" ]; then
  echo "FAIL: the crash run never started recording"; cat "$WORK/crash.txt" 2>/dev/null || true; FAILED=1
else
  sleep 4
  kill -9 "$APP_PID"; wait "$APP_PID" 2>/dev/null || true; APP_PID=""
  echo "---- killed the app mid-take ----"
  grep "^RECORDING " "$WORK/crash.txt"
fi

# 3. The launch after the crash, on the same profile.
launch "$HOME2" "$WORK/recovered.txt" MMA_UI_WALK_EXPECT_RECOVERED=1
STATUS=0; wait_for_exit 420 || STATUS=$?
judge "launch after a crash" "$WORK/recovered.txt" "$STATUS"

# 4. A microphone dies in the middle of a take. The shim kills mma_mic2 while
#    the walker's trigger file exists: created right after the take starts,
#    removed before Stop, as a mic plugged back into the same port.
HOME4="$(fresh_home fault)"
launch "$HOME4" "$WORK/fault.txt" MMA_UI_WALK_MODE=fault MMA_UI_WALK_FAULT_FILE="$WORK/kill-mic2" \
  MMA_SHIM_MODE=dead MMA_SHIM_DEVICE=mma_mic2 MMA_SHIM_FAIL_WHILE_FILE="$WORK/kill-mic2"
STATUS=0; wait_for_exit 300 || STATUS=$?
judge "a microphone dies mid-take" "$WORK/fault.txt" "$STATUS"

# The take after it. Nothing used to reopen a stream that died while its
# device stayed listed, so this take's mma_mic2 stem was silence.
SECOND="$(sed -n 's/^SECOND-TAKE //p' "$WORK/fault.txt" | tail -1)"
if [ -z "$SECOND" ]; then
  echo "FAIL: the fault run never recorded a second take"; FAILED=1
else
  echo "---- the take after the microphone came back: $SECOND ----"
  python3 Tools/verify_take.py "$SECOND" --seconds 3 --silent-ok mma_out \
    --tone mma_mic1=440 --tone mma_mic2=1000 --mirror-root "$HOME4/RECORDINGS-MIRROR" || {
    echo "FAIL: the take after a microphone died does not carry both microphones"; FAILED=1; }
fi

# 5. A take whose files never grow. The app stops it three seconds in; the red
#    card saying so must stay the card on screen, with the saved-take card
#    after it rather than over it, and the siren only while that card is up.
HOME5="$(fresh_home proof)"
launch "$HOME5" "$WORK/proof.txt" MMA_UI_WALK_MODE=proof
STATUS=0; wait_for_exit 300 || STATUS=$?
judge "the app stops a take that writes nothing" "$WORK/proof.txt" "$STATUS"

if [ "$FAILED" -ne 0 ]; then
  echo "UI walk: FAILED (reports and logs in $WORK)"
  exit 1
fi
rm -rf "$WORK"
echo "UI walk: OK (window snapshots in $SNAPSHOTS)"
