#!/usr/bin/env bash
# The release checklist's hostile-timing gate, as far as a machine without a
# USB card can take it: the recording card stops answering at Record and at
# Stop, in the REAL app, and the window must keep answering.
#
# A card pulled or wedged mid-call does not fail -- the call never returns.
# Tools/fs_stall_shim.cpp is preloaded into the unmodified app and makes every
# filesystem call that touches the destination folder block for as long as a
# trigger file exists, the way a dead USB mass-storage request blocks.
# MessageThreadStallMeter (test builds only) measures how long the message
# thread -- the thread that draws the window and answers clicks -- went without
# running. That is measured, not inferred from a log line written afterwards.
#
#   Tools/e2e_card_stall.sh
#
# Each card step is allowed to wait out one five-second deadline on the message
# thread and must then give up and say so; the steps after it must not wait
# again. Stalls longer than MMA_MAX_STALL_MS (default 9000: one deadline plus
# scheduling slack on a shared runner) fail the gate. Before the deadlines this
# was a permanent freeze, and with deadlines but no memory of the dead card it
# was one deadline per step -- tens of seconds -- which this gate also catches.
#
# Needs Xvfb, xdotool and a build with -DMMA_ALLOW_TEST_INPUTS=ON.
set -euo pipefail

DISPLAY_NUM="${MMA_DISPLAY:-:99}"
MAX_STALL_MS="${MMA_MAX_STALL_MS:-9000}"

newest() {
  local found=""
  for CANDIDATE in "$@"; do
    [ -e "$CANDIDATE" ] || continue
    if [ -z "$found" ] || [ "$CANDIDATE" -nt "$found" ]; then found="$CANDIDATE"; fi
  done
  echo "$found"
}

APP=$(newest build/SobStage_artefacts/Release/SobStage build/SobStage_artefacts/SobStage \
             build-app/SobStage_artefacts/Release/SobStage build-app/SobStage_artefacts/SobStage)
SHIM=$(newest build/libfs_stall_shim.so build-app/libfs_stall_shim.so)
test -n "$APP" || { echo "Build the app first"; exit 1; }
test -n "$SHIM" || { echo "Build with -DMMA_ALLOW_TEST_INPUTS=ON first (no libfs_stall_shim.so)"; exit 1; }
echo "App: $APP"
echo "Shim: $SHIM"

bash Tools/setup_alsa_fixture.sh >/dev/null
REAL_HOME="$HOME"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/mma-card-stall.XXXXXX")
APP_PID=""
FAILED=0

stop_app() {
  if [ -n "$APP_PID" ]; then kill -9 "$APP_PID" 2>/dev/null || true; APP_PID=""; fi
  pkill Xvfb 2>/dev/null || true
  sleep 1
}
cleanup() { rm -f "$WORK"/*.dead; stop_app; }
trap cleanup EXIT

fail() { echo "  FAIL  $*"; FAILED=1; }
pass() { echo "  PASS  $*"; }

click() {
  local win geo x y
  win=$(DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage | head -1)
  geo=$(DISPLAY="$DISPLAY_NUM" xdotool getwindowgeometry --shell "$win")
  x=$(echo "$geo" | sed -n 's/^X=//p')
  y=$(echo "$geo" | sed -n 's/^Y=//p')
  DISPLAY="$DISPLAY_NUM" xdotool mousemove $(( x + $1 )) $(( y + $2 )) click 1
}

# Each scenario gets its own profile, so a remembered destination, a recovery
# card or a first-run prompt from one cannot change what the next one sees.
launch() {
  local name="$1"
  SCENARIO_HOME="$WORK/$name"
  CARD="$SCENARIO_HOME/RECORDINGS"
  DEAD="$WORK/$name.dead"
  METER="$WORK/$name.stalls"
  LOG="$SCENARIO_HOME/.config/SobStage/log.txt"
  mkdir -p "$CARD"
  cp "$REAL_HOME/.asoundrc" "$SCENARIO_HOME/"
  rm -f "$DEAD" "$METER" "$METER.now"

  pkill Xvfb 2>/dev/null || true; sleep 1
  Xvfb "$DISPLAY_NUM" -screen 0 1280x1200x24 >/dev/null 2>&1 &
  sleep 2

  HOME="$SCENARIO_HOME" DISPLAY="$DISPLAY_NUM" \
  MMA_STALL_PREFIX="$CARD" MMA_STALL_TRIGGER="$DEAD" MMA_STALL_METER_FILE="$METER" \
  LD_PRELOAD="$PWD/$SHIM" \
    nohup "./$APP" >"$WORK/$name.stdout" 2>&1 &
  APP_PID=$!

  for _ in {1..60}; do
    if DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null 2>&1; then break; fi
    sleep 1
  done
  DISPLAY="$DISPLAY_NUM" xdotool search --name SobStage >/dev/null \
    || { fail "$name: window never appeared"; return 1; }
  sleep 5  # devices enumerate; the destination's detached checks finish
}

# The card goes away. Stalls counted from here on are the ones this gate is
# about; whatever start-up cost before it is some other gate's business.
kill_card() {
  STALLS_BEFORE=$( (grep -c '^STALL' "$METER" 2>/dev/null) || true)
  STALLS_BEFORE=${STALLS_BEFORE:-0}
  touch "$DEAD"
}

worst_stall_since_card_died() {
  local worst=0 now=0
  if [ -f "$METER" ]; then
    worst=$(tail -n +"$(( STALLS_BEFORE + 1 ))" "$METER" | awk '/^STALL/ { if ($2 > w) w = $2 } END { print w + 0 }')
  fi
  # A window that froze and has not come back has no finished STALL line yet.
  [ -f "$METER.now" ] && now=$(cat "$METER.now" 2>/dev/null || echo 0)
  now=${now:-0}
  [ "$now" -gt "$worst" ] && worst=$now
  echo "$worst"
}

wait_for_log() {  # pattern, seconds
  for _ in $(seq 1 "$2"); do
    grep -q "$1" "$LOG" 2>/dev/null && return 0
    sleep 1
  done
  return 1
}

check_responsive() {  # scenario name
  local worst
  worst=$(worst_stall_since_card_died)
  if [ "$worst" -le "$MAX_STALL_MS" ]; then
    pass "$1: longest the window stopped answering after the card died: ${worst} ms (limit ${MAX_STALL_MS})"
  else
    fail "$1: the window stopped answering for ${worst} ms after the card died (limit ${MAX_STALL_MS})"
  fi
}

press_record() {
  # The first press on a fresh profile raises "Where does this recording go?";
  # its Start button is at (733,540). With no card up that click lands on
  # nothing. Coordinates as in e2e_app_take.sh.
  click 973 178
  sleep 1
  click 733 540
}

dismiss_cards() {
  for DONE_Y in 263 293 312 332; do click 417 "$DONE_Y"; done
  sleep 1
}

# -------------------------------------------------------------------------
echo
echo "=== The card stops answering during a take, and then Stop is pressed ==="
launch stop
dismiss_cards
press_record

TAKE=""
for _ in {1..15}; do
  sleep 1
  TAKE=$(find "$CARD" -mindepth 1 -maxdepth 1 -type d | head -1)
  [ -n "$TAKE" ] && break
done
if [ -z "$TAKE" ]; then
  fail "stop: no take started, so the Stop path was never reached"
else
  pass "stop: take started in $(basename "$TAKE")"
  sleep 3

  kill_card
  sleep 1
  DISPLAY="$DISPLAY_NUM" xdotool key Escape
  click 973 178

  if wait_for_log "stopped answering" 40; then
    pass "stop: the app said the card stopped answering"
  else
    fail "stop: nothing in the log about the card after 40 s"
  fi

  # What the user is told has to be true. After a card that stopped
  # answering, nothing can be called saved -- these lines used to appear
  # straight after the card's own failure message.
  AFTER_DEATH=$(awk '/stopped answering/ { seen = 1 } seen' "$LOG")
  if echo "$AFTER_DEATH" | grep -q "Saved to\|audio itself is saved"; then
    fail "stop: claimed the take was saved after the card stopped answering:"
    echo "$AFTER_DEATH" | grep "Saved to\|audio itself is saved" | sed 's/^/        /'
  else
    pass "stop: nothing was called saved after the card stopped answering"
  fi

  # Still dead. The window has to be answering NOW, not merely have answered
  # once on the way out.
  sleep 3
  check_responsive stop
  if [ "$(cat "$METER.now" 2>/dev/null || echo 99999)" -lt 1000 ]; then
    pass "stop: window answering while the card is still dead"
  else
    fail "stop: window not answering while the card is still dead"
  fi

  # The card comes back. The abandoned writer finishes on its own worker and
  # the app carries on.
  rm -f "$DEAD"
  sleep 5
  if kill -0 "$APP_PID" 2>/dev/null; then
    pass "stop: app still running after the card came back"
  else
    fail "stop: app exited after the card came back"
  fi
fi
[ "$FAILED" = "0" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app

# -------------------------------------------------------------------------
echo
echo "=== The card stops answering just as Record is pressed ==="
BEFORE_FAIL=$FAILED
launch start
dismiss_cards
kill_card
press_record

if wait_for_log "stopped answering" 30; then
  pass "start: the app said the card stopped answering"
else
  fail "start: nothing in the log about the card after 30 s"
fi

sleep 2
check_responsive start

# One failure, one reason. A vaguer second message used to follow the first
# and became the last word on screen.
if [ "$(grep -c '\[failed\] Recording:' "$LOG" 2>/dev/null || echo 0)" = "1" ]; then
  pass "start: exactly one reason given"
else
  fail "start: more than one failure message for one refused take:"
  grep '\[failed\] Recording:' "$LOG" | sed 's/^/        /'
fi

if grep -q "Recording started" "$LOG" 2>/dev/null; then
  fail "start: a take was reported as started on a card that never answered"
else
  pass "start: no take was claimed"
fi

rm -f "$DEAD"
sleep 3
if kill -0 "$APP_PID" 2>/dev/null; then
  pass "start: app still running after the card came back"
else
  fail "start: app exited after the card came back"
fi
[ "$FAILED" = "$BEFORE_FAIL" ] || { echo "--- log ---"; tail -30 "$LOG" 2>/dev/null || true; }
stop_app

echo
if [ "$FAILED" = "0" ]; then
  echo "Card-stall gate passed."
else
  echo "Card-stall gate FAILED."
  exit 1
fi
