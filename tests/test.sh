#!/bin/bash
# Host test of the disk generator (macOS): build tests/hosttest, generate the whole disk from a
# 32 MB flash dump, then let macOS check it: fsck_msdos (read-only), mount it read-only, compare
# FLASH.BIN with the dump.
#   tests/test.sh <32 MB flash dump>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DUMP="${1:?usage: tests/test.sh <32 MB flash dump>}"
OUT="$(mktemp -d)"
trap 'hdiutil detach "$DEV" >/dev/null 2>&1 || true; rm -rf "$OUT"' EXIT
cc -std=c99 -O2 -Wall -Wextra -I "$HERE/../src" -o "$OUT/hosttest" "$HERE/hosttest.c" "$HERE/../src/fatimg.c"
"$OUT/hosttest" "$DUMP" "$OUT/disk.img"

DEV=$(hdiutil attach -readonly -nomount -imagekey diskimage-class=CRawDiskImage "$OUT/disk.img" | awk 'NR==1{print $1}')
echo "attached as $DEV"; diskutil list "$DEV" | sed -n '3,$p'
echo "--- fsck_msdos -n ${DEV}s1"
fsck_msdos -n "${DEV}s1"
MNT="$OUT/mnt"; mkdir -p "$MNT"
diskutil mount readOnly -mountPoint "$MNT" "${DEV}s1"
ls -la "$MNT"
cat "$MNT/INFO.TXT"
cmp "$MNT/FLASH.BIN" "$DUMP" && echo "FLASH.BIN == dump: OK"
diskutil unmount "$MNT"
