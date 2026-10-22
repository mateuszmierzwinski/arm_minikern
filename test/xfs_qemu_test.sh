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

timeout 120 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
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
echo PASS
