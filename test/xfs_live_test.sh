#!/usr/bin/env bash
# xfs_live_test.sh <xfs_probe>: the XFS reader against filesystems made by
# **Linux's own mkfs.xfs**, with the files that went into them to compare
# against — and with xfs_repair as the judge of whether an image is sound.
#
# The images are built with protofiles, so no mounting (and so no root) is
# needed to populate them. Every image is checked with `xfs_repair -n`
# before the reader sees it, and its md5 is checked afterwards: a read-only
# mount that changed a byte would fail here.
#
# The negative half matters as much: an image with a feature the reader does
# not implement, or a dirty log, or a damaged checksum, must be **refused**.
# That refusal is the whole safety argument for pointing this at a volume
# somebody cares about.
set -euo pipefail

probe="$(realpath "$1")"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
cd "$tmp"

for tool in mkfs.xfs xfs_repair xfs_db; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed" >&2; exit 1; }
done

fail() { echo "FAIL: $*" >&2; exit 1; }

# The feature set the reader supports: crc and finobt on, reflink and rmapbt
# off, classic inodes (no nrext64), no sparse inodes.
MINIMAL="-m crc=1,finobt=1,reflink=0,rmapbt=0,bigtime=0,inobtcount=0 -i sparse=0,size=512,nrext64=0"

# make_image <image> <protofile> [extra mkfs args...]
make_image() {
    local img="$1" proto="$2"
    shift 2
    truncate -s 512M "$img"
    # shellcheck disable=SC2086
    mkfs.xfs -q -f $MINIMAL -n ftype=1 -b size=4096 "$@" -p "$proto" "$img" ||
        fail "mkfs.xfs refused to make $img"
    xfs_repair -n "$img" >/dev/null 2>&1 || fail "mkfs.xfs made an image xfs_repair dislikes"
}

# ── the content: a few files, a block-format directory, a leaf-format one ──
printf 'hello from a real XFS filesystem\n' >hello.txt
head -c 200000 /dev/urandom >big.bin
head -c 3 /dev/urandom >tiny.bin
{
    echo DUMMY; echo "0 0"; echo "d--755 0 0"
    echo "hello.txt ---644 0 0 $tmp/hello.txt"
    echo "big.bin ---644 0 0 $tmp/big.bin"
    echo "tiny.bin ---644 0 0 $tmp/tiny.bin"
    echo "sub d--755 0 0"
    echo "nested.txt ---644 0 0 $tmp/hello.txt"
    echo "deeper d--755 0 0"
    echo "bottom.txt ---644 0 0 $tmp/hello.txt"
    echo '$'
    echo '$'
    echo "bigdir d--755 0 0"
    for i in $(seq 1 40); do echo "file-$i.txt ---644 0 0 $tmp/hello.txt"; done
    echo '$'
    echo "hugedir d--755 0 0"
    for i in $(seq 1 500); do echo "entry-$i ---644 0 0 $tmp/tiny.bin"; done
    echo '$'
    echo '$'
} >proto
make_image fs.img proto
before="$(md5sum fs.img | cut -d' ' -f1)"

echo "---- the geometry the reader sees"
"$probe" fs.img info | tee info
grep -q "blocksize 4096" info || fail "the reader read the wrong block size"
grep -q "inodesize 512" info || fail "the reader read the wrong inode size"
grep -q "ftype 1" info || fail "the reader missed the ftype feature"

echo "---- the root directory"
"$probe" fs.img ls / >root.txt
cat root.txt
for name in . .. hello.txt big.bin tiny.bin sub bigdir hugedir; do
    grep -qE " $name\$" root.txt || fail "/ is missing $name"
done
[ "$(wc -l <root.txt)" -eq 8 ] || fail "/ has $(wc -l <root.txt) entries, want 8"
grep -qE "^dir [0-9]+ sub$" root.txt || fail "sub is not reported as a directory"
grep -qE "^file [0-9]+ hello.txt$" root.txt || fail "hello.txt is not reported as a file"

echo "---- a shortform directory, nested"
"$probe" fs.img ls /sub >sub.txt
grep -qE " nested.txt\$" sub.txt || fail "/sub is missing nested.txt"
grep -qE " deeper\$" sub.txt || fail "/sub is missing deeper"
"$probe" fs.img ls /sub/deeper | grep -qE " bottom.txt\$" || fail "/sub/deeper is missing bottom.txt"

echo "---- a block-format directory (one block, with hash entries at its end)"
"$probe" fs.img ls /bigdir >bigdir.txt
[ "$(wc -l <bigdir.txt)" -eq 42 ] || fail "/bigdir has $(wc -l <bigdir.txt) entries, want 42"
grep -qE " file-1.txt\$" bigdir.txt || fail "/bigdir is missing file-1.txt"
grep -qE " file-40.txt\$" bigdir.txt || fail "/bigdir is missing file-40.txt"

echo "---- a leaf-format directory (several data blocks)"
"$probe" fs.img ls /hugedir >hugedir.txt
[ "$(wc -l <hugedir.txt)" -eq 502 ] || fail "/hugedir has $(wc -l <hugedir.txt) entries, want 502"
for i in 1 250 500; do
    grep -qE " entry-$i\$" hugedir.txt || fail "/hugedir is missing entry-$i"
done
# Every entry's inode must be distinct: a misparsed entry usually repeats one.
[ "$(awk '{print $2}' hugedir.txt | sort -u | wc -l)" -eq 502 ] ||
    fail "/hugedir has repeated inode numbers"

echo "---- reading files back byte for byte"
"$probe" fs.img cat /hello.txt out.txt && cmp out.txt hello.txt || fail "hello.txt differs"
"$probe" fs.img cat /big.bin out.bin && cmp out.bin big.bin || fail "big.bin differs"
"$probe" fs.img cat /tiny.bin out.tiny && cmp out.tiny tiny.bin || fail "tiny.bin differs"
"$probe" fs.img cat /sub/deeper/bottom.txt out.deep && cmp out.deep hello.txt || fail "bottom.txt differs"

echo "---- stat"
"$probe" fs.img stat /big.bin | tee stat.txt
grep -q "size 200000" stat.txt || fail "big.bin has the wrong size"
grep -q "format 2" stat.txt || fail "big.bin should be in extents format"
grep -q "mode 100644" stat.txt || fail "big.bin has the wrong mode"

echo "---- what is not there"
! "$probe" fs.img ls /nothing 2>/dev/null || fail "a missing path was found"
! "$probe" fs.img cat /sub out.x 2>/dev/null || fail "a directory was read as a file"
! "$probe" fs.img ls /hello.txt 2>/dev/null || fail "a file was listed as a directory"

echo "---- the reader changed nothing"
[ "$(md5sum fs.img | cut -d' ' -f1)" = "$before" ] || fail "a read-only mount changed the image"
xfs_repair -n fs.img >/dev/null 2>&1 || fail "the image is no longer sound"

# ── the refusals ───────────────────────────────────────────────────────────
echo "---- a filesystem with reflink is refused"
truncate -s 512M reflink.img
mkfs.xfs -q -f -m crc=1,finobt=1,reflink=1 -b size=4096 reflink.img
! "$probe" reflink.img info 2>err.txt || fail "an image with reflink was mounted"
grep -q "unsupported read-only features" err.txt || cat err.txt

echo "---- a filesystem with 64-bit extent counts is refused"
truncate -s 512M nrext.img
mkfs.xfs -q -f -m crc=1,finobt=1,reflink=0,rmapbt=0 -i nrext64=1 -b size=4096 nrext.img
! "$probe" nrext.img info 2>/dev/null || fail "an image with nrext64 was mounted"

echo "---- a v4 filesystem is refused"
if truncate -s 512M v4.img && mkfs.xfs -q -f -m crc=0 -b size=4096 v4.img 2>/dev/null; then
    ! "$probe" v4.img info 2>/dev/null || fail "a v4 image was mounted"
else
    echo "(this mkfs.xfs cannot make v4 images any more: skipped)"
fi

echo "---- a damaged superblock checksum is refused"
cp fs.img bad_sb.img
printf '\xff\xff\xff\xff' | dd of=bad_sb.img bs=1 seek=224 conv=notrunc status=none
cmp -s fs.img bad_sb.img && fail "the checksum we meant to damage was already ffffffff"
! "$probe" bad_sb.img info 2>/dev/null || fail "an image with a bad superblock crc was mounted"

echo "---- a damaged directory block is refused when it is read"
cp fs.img bad_dir.img
# bigdir's single directory block: find it, then flip a byte inside it.
blk="$(xfs_db -r -c "inode $(grep -E ' bigdir$' root.txt | awk '{print $2}')" -c bmap bad_dir.img |
    awk 'NR==1 {print $5}')"
[ -n "$blk" ] || fail "could not find bigdir's block"
printf '\xff' | dd of=bad_dir.img bs=1 seek=$((blk * 4096 + 100)) conv=notrunc status=none
# A corruption that corrupted nothing would make this case vacuous.
cmp -s fs.img bad_dir.img && fail "the byte we meant to flip was already 0xff"
"$probe" bad_dir.img info >/dev/null || fail "the mount should still work"
! "$probe" bad_dir.img ls /bigdir 2>/dev/null || fail "a corrupt directory block was read as data"

echo "---- a dirty log is refused"
cp fs.img dirty.img
logstart="$(xfs_db -r -c "sb 0" -c "p logstart" dirty.img | awk '{print $3}')"
# A record header of the same cycle, after the unmount record: the newest
# record is then not an unmount, which is exactly what a crash leaves.
printf '\xfe\xed\xba\xbe\x00\x00\x00\x01\x00\x00\x00\x02\x00\x00\x02\x00' |
    dd of=dirty.img bs=1 seek=$((logstart * 4096 + 1024)) conv=notrunc status=none
cmp -s fs.img dirty.img && fail "the fake log record changed nothing"
! "$probe" dirty.img info 2>err2.txt || fail "an image with a dirty log was mounted"
grep -q "log is dirty" err2.txt || { cat err2.txt; fail "the refusal did not name the log"; }

echo PASS
