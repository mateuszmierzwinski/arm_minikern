#!/usr/bin/env bash
# xfs_qemu_test.sh <kernel> <rootfs> <nbdserver>: the whole storage path.
#
# A Go program on minikern formats an XFS filesystem on a volume that is a
# file on a server — over this kernel's own TCP, its own NBD client and its
# own buffer cache — then asks the kernel to mount it and reads it back.
# Afterwards the host runs **xfs_repair** on that same file.
#
# That last step is the point. The claim is not that the guest printed PASS;
# it is that a filesystem this board wrote over the network is one Linux
# calls valid, and that the kernel can read what it wrote.
set -euo pipefail

image="$1"
rootfs="$2"
server="$(realpath "$3")"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
vols="$tmp/volumes"
vol="$vols/vol.img"
log="$tmp/console.log"
port=$((31000 + RANDOM % 15000))
pid=0

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }
command -v xfs_repair >/dev/null || { echo "xfs_repair is not installed" >&2; exit 1; }

host_ip="$(ip -4 -o addr show scope global 2>/dev/null | awk 'NR==1{split($4,a,"/"); print a[1]}')"
if [ -z "$host_ip" ]; then
    echo "SKIP: this machine has no routable address, so the guest cannot reach a server on it" >&2
    exit 0
fi

cleanup() {
    [ "$pid" -ne 0 ] && kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# A 512 MiB volume of rubbish, so nothing the guest reads back could be
# left over from a filesystem somebody else made.
mkdir -p "$vols"
truncate -s 512M "$vol"
head -c 2097152 /dev/urandom | dd of="$vol" conv=notrunc status=none

"$server" -listen "$host_ip:$port" -volumes "$vols" >"$tmp/server.log" 2>&1 &
pid=$!
for _ in $(seq 1 50); do
    grep -q "listening on" "$tmp/server.log" && break
    sleep 0.1
done
grep -q "listening on" "$tmp/server.log" || { cat "$tmp/server.log"; fail "the server did not start"; }

timeout 300 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
    -nic user -kernel "$image" -initrd "$rootfs" \
    -append "console=uart0 minikern.halt=poweroff minikern.netstat=1 minikern.nbd=$host_ip:$port/vol" ||
    { echo "---- console"; cat "$log"; echo "---- server"; cat "$tmp/server.log"; fail "qemu did not power off"; }

echo "---- console"
cat "$log"
echo "---- server"
cat "$tmp/server.log"

expect() {
    grep -qF -- "$1" "$log" || fail "the console lacks: $1"
}

# ── what the guest did ─────────────────────────────────────────────────────
expect "nbd: 131072 blocks of 4096 bytes (512 MiB)"
expect "PASS format"
expect "formatted: 131072 blocks, 2 groups, root inode 64"
expect "xfs: 131072 blocks of 4096 bytes, 2 groups, 512-byte inodes, root 64"
expect "xfs: mounted at /mnt"
expect "PASS mount"
expect "PASS list"
expect "PASS read-only"
expect "PASS mount-twice"
expect "PASS bad-fstype"
expect "ALL XFS PASS"
expect "init exited with status 0"

# ── and what Linux makes of it ─────────────────────────────────────────────
echo "---- xfs_repair on the filesystem the board wrote"
if ! xfs_repair -n "$vol" >"$tmp/repair.log" 2>&1; then
    cat "$tmp/repair.log"
    fail "xfs_repair rejected the filesystem the board wrote"
fi
cat "$tmp/repair.log"
grep -vE '^(Phase [0-9]|        - |No modify flag|Cannot get host|Repair may fail|the image and)' \
    "$tmp/repair.log" >"$tmp/complaints.log" || true
if [ -s "$tmp/complaints.log" ]; then
    fail "xfs_repair had something to say: $(head -1 "$tmp/complaints.log")"
fi

echo "---- and the geometry is the one mkfs.xfs would have made"
command -v xfs_db >/dev/null && xfs_db -r -c "sb 0" -c "p agcount rootino fdblocks" "$vol" |
    tee "$tmp/db.log"
grep -q "agcount = 2" "$tmp/db.log" || fail "agcount"
grep -q "rootino = 64" "$tmp/db.log" || fail "rootino"
grep -q "fdblocks = 114672" "$tmp/db.log" || fail "fdblocks"

grep -q "served \"vol\"" "$tmp/server.log" || fail "the server did not log the session"

# ── phase two: writing into a filesystem the host made ─────────────────────
#
# Creating a file needs the log, so a volume the board formatted has nowhere
# to write. mkfs.xfs puts a file there instead, and the board writes into
# the blocks it already has — the one kind of write that changes no
# metadata, and so the one this kernel allows.
command -v mkfs.xfs >/dev/null || { echo "mkfs.xfs is not installed" >&2; exit 1; }

echo "---- phase two: a filesystem with a file in it"
# Deliberately **not** a multiple of the block size: the last block then has
# room in it, and growing a file into room it already owns is a metadata
# change (its recorded size) without an allocation — which is the write the
# log makes possible.
head -c 262000 /dev/urandom >"$tmp/data.bin"
{
    echo DUMMY
    echo "0 0"
    echo "d--755 0 0"
    echo "data.bin ---644 0 0 $tmp/data.bin"
    echo '$'
} >"$tmp/proto"
truncate -s 512M "$vol"
mkfs.xfs -q -f -m crc=1,finobt=1,reflink=0,rmapbt=0,bigtime=0,inobtcount=0 \
    -i sparse=0,size=512,nrext64=0 -n ftype=1 -b size=4096 -p "$tmp/proto" "$vol" ||
    fail "mkfs.xfs could not make the second volume"
xfs_repair -n "$vol" >/dev/null 2>&1 || fail "mkfs.xfs made an image xfs_repair dislikes"

# How many 0x7e bytes the volume holds before the board writes 40000 of
# them: the file is random, so a few are there by chance.
pattern_before="$(tr -dc '\176' <"$vol" | wc -c)"
# What the filesystem holds before the board touches it, so that creating a
# file and removing it again can be shown to have left no trace.
ifree_before="$(xfs_db -r -c "sb 0" -c "print ifree" "$vol" | awk '{print $3}')"
fdblocks_before="$(xfs_db -r -c "sb 0" -c "print fdblocks" "$vol" | awk '{print $3}')"

log="$tmp/console2.log"
timeout 300 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
    -nic user -kernel "$image" -initrd "$rootfs" \
    -append "console=uart0 minikern.halt=poweroff minikern.hangcheck=40 minikern.nbd=$host_ip:$port/vol minikern.env.MINIKERN_XFS_MODE=write" ||
    { echo "---- console"; cat "$log"; fail "qemu did not power off in phase two"; }

echo "---- console (phase two)"
cat "$log"
expect "xfs: mounted at /mnt, read-write"
expect "PASS mount-rw"
expect "PASS read-existing"
expect "PASS write-data"
expect "PASS read-back"
expect "PASS create-file"
expect "PASS list-after-create"
expect "PASS create-twice"
expect "PASS remove-file"
expect "PASS remove-twice"
expect "PASS write-into-a-new-file"
expect "PASS remove-a-file-with-blocks"
expect "PASS grow-into-own-block"
expect "PASS refuse-grow"
expect "ALL XFS PASS"
expect "init exited with status 0"

echo "---- xfs_repair after the board wrote into the file"
if ! xfs_repair -n "$vol" >"$tmp/repair2.log" 2>&1; then
    cat "$tmp/repair2.log"
    fail "xfs_repair rejected the filesystem after the board wrote to it"
fi
grep -vE '^(Phase [0-9]|        - |No modify flag|Cannot get host|Repair may fail|the image and)' \
    "$tmp/repair2.log" >"$tmp/complaints2.log" || true
if [ -s "$tmp/complaints2.log" ]; then
    cat "$tmp/repair2.log"
    fail "xfs_repair complained after the board's write"
fi

echo "---- and the board's bytes really reached the image"
pattern_after="$(tr -dc '\176' <"$vol" | wc -c)"
echo "0x7e bytes in the volume: $pattern_before before, $pattern_after after"
# At least the 40000 the board wrote must be there. The increase is slightly
# less than 40000 because the random bytes it overwrote included a few 0x7e
# of their own, which is why the test counts the total rather than the
# difference.
[ "$pattern_after" -ge 40000 ] || fail "the board's 40000 bytes are not in the volume"
[ "$pattern_after" -gt "$pattern_before" ] || fail "the volume did not change at all"

# ── the log, and the inode the board rewrote ───────────────────────────────
#
# Two judges neither of which is ours: xfs_logprint reads the records the
# board wrote, and xfs_db reads the size it recorded.
echo "---- xfs_logprint reads what the board wrote to the log"
if ! xfs_logprint -t "$vol" >"$tmp/logprint.log" 2>&1; then
    cat "$tmp/logprint.log"
    fail "xfs_logprint could not read the log"
fi
cat "$tmp/logprint.log"
grep -qiE 'bad|corrupt|invalid|error' "$tmp/logprint.log" &&
    fail "xfs_logprint found something wrong with the log"
# <CLEAN> is xfs_logprint's own recognition of the unmount record the board
# wrote at the head: it is the judge saying the log needs no recovery.
grep -q "state: <CLEAN>" "$tmp/logprint.log" ||
    fail "xfs_logprint does not consider the log clean"

echo "---- and the transaction record the board wrote before it"
if ! xfs_logprint -t -i -s 0 "$vol" >"$tmp/logdump.log" 2>&1; then
    cat "$tmp/logdump.log"
    fail "xfs_logprint could not read the transaction the board wrote"
fi
cat "$tmp/logdump.log"
# The transaction, read back by a tool that is not ours: one item, an inode
# core, and the fields of the inode the board actually changed.
grep -q "#items:1" "$tmp/logdump.log" || fail "the record does not hold one item"
grep -q "INODE: #regs:2" "$tmp/logdump.log" || fail "the inode item is not two regions"
grep -q "CORE inode:" "$tmp/logdump.log" || fail "the item does not carry an inode core"
grep -qE 'magic:IN .*format:2' "$tmp/logdump.log" ||
    fail "the logged core is not an extents-format inode"
# 262100 = 0x3ffd4, the size the board grew the file to.
grep -q "size:0x3ffd4" "$tmp/logdump.log" ||
    fail "the logged core does not carry the size the board recorded"
log_bb=$(xfs_db -r -c "sb 0" -c "print logstart" "$vol" | awk '{print $3}')
log_daddr=$(xfs_logprint -t "$vol" | sed -n 's/.*daddr: \([0-9]*\).*/\1/p')
echo "the log starts at fsb $log_bb, daddr $log_daddr; its first three blocks:"
dd if="$vol" bs=512 skip="$log_daddr" count=3 2>/dev/null | hexdump -C | head -36

echo "---- and the file the board created and then removed is gone"
# The board created it, listed it, and removed it again, so what the host
# must see is an absence — and a free-inode count back where it started.
xfs_db -r -c "path /made-on-the-board" "$vol" >"$tmp/created.log" 2>&1 &&
    fail "the file the board removed is still in the filesystem"
cat "$tmp/created.log"
ifree_after="$(xfs_db -r -c "sb 0" -c "print ifree" "$vol" | awk '{print $3}')"
echo "free inodes: $ifree_before before the board ran, $ifree_after after"
[ "$ifree_after" = "$ifree_before" ] ||
    fail "the inode the board used was not given back ($ifree_before -> $ifree_after)"
fdblocks_after="$(xfs_db -r -c "sb 0" -c "print fdblocks" "$vol" | awk '{print $3}')"
echo "free blocks: $fdblocks_before before the board ran, $fdblocks_after after"
[ "$fdblocks_after" = "$fdblocks_before" ] ||
    fail "the blocks the board used were not given back ($fdblocks_before -> $fdblocks_after)"

echo "---- and xfs_db agrees about the size the board recorded"
xfs_db -r -c "path /data.bin" -c "print core.size" "$vol" >"$tmp/size.log" 2>&1 ||
    fail "xfs_db could not read the file's inode"
cat "$tmp/size.log"
grep -q "core.size = 262100" "$tmp/size.log" ||
    fail "the size the board grew the file to is not on the disk"

echo PASS
