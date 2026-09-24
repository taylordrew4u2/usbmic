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
#
# Long enough that no stream ever reaches the end of its file. When ALSA's
# file plugin reaches the end of an infile it starts again from the top, but
# from then on every block it delivers has a seam in it -- a phase jump at a
# fixed offset in each period -- for as long as the stream stays open. Read
# straight off the device with nothing else running, the tone is clean to
# 30.00 s and then broken in every block. The fixture's files were thirty
# seconds, so any stream open longer than that fed the app corrupt audio,
# and verify_take.py's per-block tone vote failed on a stem the app had
# recorded faithfully. It hid for a long time because the buffer ladder
# reopens the streams, which starts the files over. The longest gate keeps
# a stream open for about two and a half minutes; these last four.
python3 - "$FIXTURE_DIR" <<'PY'
import math, struct, sys
out = sys.argv[1]
seconds = 240
for name, freq in (("tone440", 440.0), ("tone1000", 1000.0)):
    with open("%s/%s.raw" % (out, name), "wb") as f:
        step = 2 * math.pi * freq / 48000.0
        for start in range(0, 48000 * seconds, 48000):
            f.write(b"".join(struct.pack("<f", 0.4 * math.sin(step * i))
                             for i in range(start, start + 48000)))
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
