#!/usr/bin/env bash
# nbd_qemu_test.sh <kernel> <rootfs> <nbdserver>: the whole block path, end
# to end — a Go program on minikern reading and writing a volume that is a
# file on a server, over this kernel's own TCP and its own NBD client.
#
# The proof is at both ends. The host writes a known pattern into the image
# before the boot and the guest checks it; the guest writes its own pattern
# and the host checks that afterwards, byte for byte with cmp. A test that
# only read the console would pass on a kernel that printed PASS and did
# nothing.
#
# The server is bound to the host's address on the real network, because
# slirp forwards guest UDP to 10.0.2.2 but not guest TCP (CLAUDE.md §10).
set -euo pipefail

image="$1"
rootfs="$2"
server="$(realpath "$3")"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
vols="$tmp/volumes"
img="$vols/vol.img"
log="$tmp/console.log"
port=$((31000 + RANDOM % 15000))
pid=0

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

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

# ── the volume, with a pattern the guest will check ─────────────────────────
mkdir -p "$vols"
dd if=/dev/urandom of="$img" bs=1M count=4 status=none
printf 'MINIKERN-NBD-VOLUME-V1' | dd of="$img" bs=1 seek=0 conv=notrunc status=none
head -c 65536 /dev/zero | tr '\000' '\245' | dd of="$img" bs=1 seek=1048576 conv=notrunc status=none
cp "$img" "$tmp/before.raw"

"$server" -listen "$host_ip:$port" -volumes "$vols" >"$tmp/server.log" 2>&1 &
pid=$!
for _ in $(seq 1 50); do
    grep -q "listening on" "$tmp/server.log" && break
    sleep 0.1
done
grep -q "listening on" "$tmp/server.log" || { cat "$tmp/server.log"; fail "the server did not start"; }
echo "serving $img at $host_ip:$port"

timeout 90 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
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

# ── what the guest reported ────────────────────────────────────────────────
expect "nbd: connecting to $host_ip:$port"
expect "nbd: 1024 blocks of 4096 bytes (4 MiB)"
expect "volume: 4194304 bytes"
expect "PASS header"
expect "PASS pattern"
expect "PASS cached-reread"
expect "PASS unaligned-span"
expect "PASS write"
expect "PASS read-back"
expect "PASS past-the-end"
expect "PASS sync"
expect "ALL NBD PASS"
expect "init exited with status 0"
# The cache must have served the second read of the pattern.
grep -qE "nbd: cache [1-9][0-9]* hits" "$log" || fail "the buffer cache served nothing"

# ── what actually reached the file ─────────────────────────────────────────
echo "---- verifying the image on disk"
dd if="$img" of="$tmp/written.bin" bs=1 skip=2097152 count=131072 status=none
head -c 131072 /dev/zero | tr '\000' '\134' >"$tmp/written.expected"
cmp "$tmp/written.bin" "$tmp/written.expected" || fail "the guest's 128 KiB write is not in the image"

dd if="$img" of="$tmp/mark.bin" bs=1 skip=3145728 count=19 status=none
printf 'written-by-minikern' >"$tmp/mark.expected"
cmp "$tmp/mark.bin" "$tmp/mark.expected" || fail "the guest's mark is not in the image"

# Everything the guest did not write must be exactly as it was.
for range in "0 2097152" "2228224 917504"; do
    set -- $range
    dd if="$img" of="$tmp/untouched.bin" bs=1 skip="$1" count="$2" status=none
    dd if="$tmp/before.raw" of="$tmp/untouched.expected" bs=1 skip="$1" count="$2" status=none
    cmp "$tmp/untouched.bin" "$tmp/untouched.expected" ||
        fail "the guest changed bytes it never wrote, at offset $1"
done

grep -q "served \"vol\"" "$tmp/server.log" || fail "the server did not log the session"
echo PASS
