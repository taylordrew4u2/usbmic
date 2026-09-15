#!/usr/bin/env bash
# §6.5 on a drive that genuinely runs out of room in the middle of a take.
#
# Both halves of this were unit-tested against fakes, and neither had ever met a
# real ENOSPC. What that hid: when a write failed, SessionWriter set no account
# at all -- only the roll-over past 3.9 GB did -- so the take stopped under the
# card-removal notice, which tells the user the drive "stopped responding" and
# to check that it is plugged in properly. Someone whose card was simply full
# spent the one moment they were still next to the rig re-seating a cable that
# was never loose.
#
#   Tools/e2e_disk_full.sh
#
# A real full filesystem is the only way to reach it: no fake and no unit test
# can make std::filesystem::space report an empty drive. This mounts a small
# tmpfs, leaves a sliver free, and records into it until the writes fail.
set -euo pipefail

GATE=""
for CANDIDATE in build/disk_full_take build-app/disk_full_take; do
  [ -x "$CANDIDATE" ] || continue
  if [ -z "$GATE" ] || [ "$CANDIDATE" -nt "$GATE" ]; then GATE="$CANDIDATE"; fi
done
test -n "$GATE" || { echo "Build with -DMMA_ALLOW_TEST_INPUTS=ON first"; exit 1; }

MOUNT_POINT="${MMA_DISK_FULL_MOUNT:-/tmp/mma-full-drive}"
MOUNTED=0

cleanup() {
  if [ "$MOUNTED" = "1" ]; then
    umount "$MOUNT_POINT" 2>/dev/null || sudo umount "$MOUNT_POINT" 2>/dev/null || true
  fi
}
trap cleanup EXIT

mkdir -p "$MOUNT_POINT"

# 4 MB is comfortably more than the header and first blocks of three files and
# comfortably less than what the fixture produces in a second, so the take gets
# genuinely under way before the drive gives out.
if mount -t tmpfs -o size=4M tmpfs "$MOUNT_POINT" 2>/dev/null; then
  MOUNTED=1
elif sudo -n mount -t tmpfs -o size=4M tmpfs "$MOUNT_POINT" 2>/dev/null; then
  MOUNTED=1
fi

if [ "$MOUNTED" != "1" ]; then
  # Loud rather than quiet. A skip that reads like a pass is how a gate stops
  # being a gate, so this says plainly that nothing was proved.
  echo "SKIPPED: could not mount a small tmpfs at $MOUNT_POINT."
  echo "         Nothing about the full-drive path was checked on this machine."
  echo "         Run as a user who may mount tmpfs to exercise it."
  exit 0
fi

bash Tools/setup_alsa_fixture.sh >/dev/null

# Leave a sliver free, so the take starts, runs, and only then hits the wall --
# a drive that was already full when recording started would pass a weaker test
# while proving much less.
dd if=/dev/zero of="$MOUNT_POINT/ballast" bs=1K count=3400 2>/dev/null

echo "=== A take recorded onto a drive that fills part way through ==="
df -h "$MOUNT_POINT" | tail -1
"./$GATE" "$MOUNT_POINT"

echo
echo "Disk-full gate passed."
