#!/usr/bin/env bash
# Creates file-backed ALSA capture devices carrying known tones, so the real
# Linux audio path can be exercised on a machine with no sound hardware -- a
# CI runner, or a container. Writes ~/.asoundrc; back yours up first if you
# have one you care about.
set -euo pipefail

FIXTURE_DIR="${1:-${TMPDIR:-/tmp}/mma-alsa-fixture}"
mkdir -p "$FIXTURE_DIR"

# Float32, because that is the first format AlsaBackend negotiates. ALSA's file
# plugin cannot refuse a format the way real hardware does -- it just replays
# the bytes -- so the fixture has to speak whatever the backend asks for, or the
# capture reads correct-length garbage.
python3 - "$FIXTURE_DIR" <<'PY'
import math, struct, sys
out = sys.argv[1]
# 30 seconds each, so a capture never runs dry mid-test.
for name, freq in (("tone440", 440.0), ("tone1000", 1000.0)):
    with open("%s/%s.raw" % (out, name), "wb") as f:
        f.write(b"".join(struct.pack("<f", 0.4 * math.sin(2 * math.pi * freq * i / 48000.0))
                         for i in range(48000 * 30)))
PY

cat > "$HOME/.asoundrc" <<CONF
# Written by Tools/setup_alsa_fixture.sh -- virtual mics for testing.
pcm.mma_mic1 { type file; slave.pcm "null"; file "/dev/null"; infile "$FIXTURE_DIR/tone440.raw"; format "raw" }
pcm.mma_mic2 { type file; slave.pcm "null"; file "/dev/null"; infile "$FIXTURE_DIR/tone1000.raw"; format "raw" }
# /dev/null, not a file in the fixture directory. The monitor bus writes to
# this device continuously for as long as the app is open, and nothing ever
# truncates it -- pointed at a real file it reached 2.7 GB in a few minutes of
# screenshotting and filled the disk, which presents as "Room for 2m 04s of
# feelings" on the main screen rather than as a full disk.
pcm.mma_out  { type file; slave.pcm "null"; file "/dev/null"; format "raw" }
CONF

echo "ALSA fixture ready in $FIXTURE_DIR (mma_mic1 = 440 Hz, mma_mic2 = 1000 Hz)"
