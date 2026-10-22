#!/usr/bin/env bash
# mkfsxfs_live_test.sh <mkfsxfs> <xfs_probe>: the formatter against Linux's
# own judge.
#
# The point of writing mkfs in Go, in user space, was that `xfs_repair` can
# say whether the result is a real filesystem — so that is the test, and
# **any** output from xfs_repair beyond its phase headings is a failure.
# Then the kernel's own reader mounts what was written, which closes the
# loop: our formatter, Linux's judgement, our reader.
set -euo pipefail

mkfs="$(realpath "$1")"
probe="$(realpath "$2")"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
cd "$tmp"

for tool in xfs_repair xfs_db; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed" >&2; exit 1; }
done

fail() { echo "FAIL: $*" >&2; exit 1; }

# repair_clean <image>: xfs_repair must find nothing to say. Its phase
# headings and the host-geometry notice are expected; a line about a
# structure is not.
repair_clean() {
    local img="$1" out="$tmp/repair.log"
    if ! xfs_repair -n "$img" >"$out" 2>&1; then
        cat "$out"
        fail "xfs_repair rejected $img"
    fi
    grep -vE '^(Phase [0-9]|        - |No modify flag|Cannot get host|Repair may fail|the image and|Note - |Default)' \
        "$out" >"$tmp/complaints.log" || true
    if [ -s "$tmp/complaints.log" ]; then
        cat "$out"
        fail "xfs_repair had something to say about $img: $(head -1 "$tmp/complaints.log")"
    fi
}

echo "---- formatting a 512 MiB volume"
truncate -s 512M vol.img
"$mkfs" -device vol.img | tee mkfs.log
grep -q "agcount=2" mkfs.log || fail "a filesystem needs at least two groups for redundancy"
grep -q "root     =inode 64" mkfs.log || fail "the root inode is not where mkfs.xfs would put it"

echo "---- xfs_repair must find nothing wrong"
repair_clean vol.img

echo "---- the geometry matches what mkfs.xfs makes of the same device"
xfs_db -r -c "sb 0" -c "p agcount agblocks rootino inoalignmt fdblocks icount ifree" vol.img |
    tee geometry.log
grep -q "agcount = 2" geometry.log || fail "agcount"
grep -q "agblocks = 65536" geometry.log || fail "agblocks"
grep -q "rootino = 64" geometry.log || fail "rootino"
grep -q "inoalignmt = 4" geometry.log || fail "inoalignmt"
grep -q "fdblocks = 114672" geometry.log || fail "fdblocks: mkfs.xfs makes 114672 of the same device"
grep -q "icount = 64" geometry.log || fail "icount"
grep -q "ifree = 61" geometry.log || fail "ifree"

echo "---- every secondary superblock is there"
for ag in 0 1; do
    xfs_db -r -c "sb $ag" -c "p magicnum" vol.img | grep -q "0x58465342" ||
        fail "group $ag has no superblock"
done

echo "---- the log is clean, not merely zeroed"
xfs_repair -n vol.img 2>&1 | grep -q "totally zeroed log" &&
    fail "the log holds no record: an unmount record is what makes it clean"

echo "---- the kernel's own reader mounts it"
"$probe" vol.img info | tee probe.log
grep -q "blocks 131072 blocksize 4096 agcount 2" probe.log || fail "the reader read the wrong geometry"
grep -q "root 64" probe.log || fail "the reader found the wrong root"
"$probe" vol.img ls / >root.txt
cat root.txt
[ "$(wc -l <root.txt)" -eq 2 ] || fail "a new filesystem's root has only . and .."
grep -qE '^dir 64 \.$' root.txt || fail "the root is missing ."
grep -qE '^dir 64 \.\.$' root.txt || fail "the root is missing .."
"$probe" vol.img stat / | grep -q "size 6 nlink 2 format 1" ||
    fail "an empty root should be a 6-byte shortform directory with two links"

echo "---- a formatted volume is not formatted again by accident"
if "$mkfs" -device vol.img -quiet 2>err.log; then
    fail "an existing filesystem was overwritten without -force"
fi
grep -q "already holds an XFS filesystem" err.log || { cat err.log; fail "the refusal is unclear"; }
"$mkfs" -device vol.img -force -quiet || fail "-force did not work"
repair_clean vol.img

echo "---- a volume too small to hold a filesystem is refused"
truncate -s 4M tiny.img
if "$mkfs" -device tiny.img -quiet 2>tiny.log; then
    fail "a 4 MiB volume was formatted"
fi
grep -q "too small" tiny.log || { cat tiny.log; fail "the refusal is unclear"; }

echo "---- a larger volume, with more groups"
truncate -s 4G big.img
"$mkfs" -device big.img | tee big.log
grep -q "agcount=4" big.log || fail "a 4 GiB volume should be split into four groups"
repair_clean big.img
"$probe" big.img ls / | grep -qE '^dir [0-9]+ \.$' || fail "the reader cannot read the larger one"

echo PASS
