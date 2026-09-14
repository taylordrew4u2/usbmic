#!/usr/bin/env bash
# Launching with no display must SAY so, not segfault.
#
# JUCE's centreWithSize goes through getParentOrMainMonitorBounds, which
# dereferences getPrimaryDisplay() without checking it. With an empty display
# list that is a null read, so building the main window killed the app on
# launch: exit 139 and not a word, in the log or anywhere else. Over SSH, or
# with a broken display configuration, that is all a person got.
#
#   Tools/e2e_headless.sh
set -euo pipefail

APP=""
for CANDIDATE in build/SobStage_artefacts/Release/SobStage build/SobStage_artefacts/SobStage \
                 build-app/SobStage_artefacts/Release/SobStage build-app/SobStage_artefacts/SobStage; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$APP" ] || [ "$CANDIDATE" -nt "$APP" ]; then APP="$CANDIDATE"; fi
done
test -n "$APP" || { echo "Build the app first"; exit 1; }

OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT

set +e
env -u DISPLAY "./$APP" >"$OUT" 2>&1
STATUS=$?
set -e

# 139 is the shell's rendering of SIGSEGV, and 11 is the raw signal: either one
# means the app died rather than declined.
if [ "$STATUS" -eq 139 ] || [ "$STATUS" -eq 11 ]; then
  echo "FAIL: the app segfaulted on a display-less launch (exit $STATUS) instead of saying why."
  head -5 "$OUT"
  exit 1
fi

if [ "$STATUS" -eq 0 ]; then
  echo "FAIL: the app reported success on a display-less launch (exit 0)."
  echo "      It cannot have shown a recording screen; saying it did is worse than failing."
  exit 1
fi

if ! grep -qi "needs a display" "$OUT"; then
  echo "FAIL: the app exited $STATUS on a display-less launch without explaining why."
  head -5 "$OUT"
  exit 1
fi

echo "PASS: a display-less launch was refused with a reason (exit $STATUS)"
