#!/usr/bin/env bash
# netboot_qemu_test.sh <kernel> <server> <rootfs>: the P5 milestone.
#
# Runs the real MKNB server on the host with the rootfs as its image, boots
# the kernel with no initrd at all and minikern.netboot, and checks that the
# board found the server by sweeping, fetched the archive, verified its
# digest, mounted it and ran the Go init inside it.
#
# The server binds the protocol's fixed port on loopback, so only one of
# these may run at a time on a machine.
set -euo pipefail

image="$1"
server="$2"
rootfs="$3"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
log="$tmp/console.log"
srvlog="$tmp/server.log"
images="$tmp/images"

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

mkdir -p "$images"
cp "$rootfs" "$images/default.tar"
chmod u+w "$images/default.tar"

# The server signs its offers, and the board is told which key to trust.
# The seed is fixed so the public key is too; it is a test, not a secret.
seed="$tmp/seed.hex"
printf '0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20' >"$seed"
"$server" -images "$images" -addr 127.0.0.1:7634 -sign-seed "$seed" >"$srvlog" 2>&1 &
srv=$!
trap 'kill $srv 2>/dev/null || true; wait $srv 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
    grep -q "serving" "$srvlog" && break
    sleep 0.1
done

# The digest of the image the server will offer. Pinning it is what stops
# the first machine to answer the sweep deciding what the board runs, so the
# ordinary path is tested **with** the pin: it must change nothing.
digest="$(sha256sum "$images/default.tar" | cut -d" " -f1)"
# The server prints the public key a board must trust, because deriving it
# by hand is how it ends up wrong.
for _ in $(seq 1 50); do
    grep -q "minikern.netboot.key=" "$srvlog" && break
    sleep 0.1
done
key="$(sed -n 's/.*minikern.netboot.key=\([0-9a-f]*\).*/\1/p' "$srvlog" | head -1)"
[ "${#key}" -eq 64 ] || { cat "$srvlog"; echo "FAIL: the server did not print its public key" >&2; exit 1; }

status=0
timeout 120 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
    -nic user -kernel "$image" \
    -append "console=uart0 minikern.netboot minikern.halt=poweroff minikern.rootfs.sha256=$digest minikern.netboot.key=$key" || status=$?

fail() {
    echo "---- console"; cat "$log" 2>/dev/null || true
    echo "---- server"; cat "$srvlog" 2>/dev/null || true
    echo "FAIL: $1" >&2
    exit 1
}

expect() {
    grep -qF -- "$1" "$log" || fail "console lacks: $1"
}

[ "$status" -eq 0 ] || fail "qemu exited $status (124 = hang)"
expect "netboot: looking for a server on the local subnet (unicast sweep)"
expect "netboot: server at 10.0.2.2 offers"
expect "bytes from 10.0.2.2 in"
expect "minikern: started /init as pid 1"
expect "netbooted init running on gc"
expect 'motd: "hello from the tar root"'
expect "NETBOOT INIT OK"
expect "init exited with status 0"
expect "netboot: accepting only the image whose sha256 is $digest"
expect "netboot: accepting only offers signed by $key"

# And the attack it exists for: an offer of anything else is refused at the
# offer, so nothing is fetched and nothing is mounted. The board is told the
# digest of an image that is not the one on offer, which is indistinguishable
# from an impostor answering the sweep.
wrong="0000000000000000000000000000000000000000000000000000000000000000"
log2="$tmp/console-wrong.log"
status=0
timeout 120 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log2" \
    -nic user -kernel "$image" \
    -append "console=uart0 minikern.netboot minikern.halt=poweroff minikern.rootfs.sha256=$wrong" || status=$?
echo "---- console (a pin that matches nothing on offer)"
cat "$log2"
[ "$status" -eq 0 ] || { echo "FAIL: qemu exited $status with the wrong pin" >&2; exit 1; }
grep -q "netbooted init running" "$log2" &&
    { echo "FAIL: the board ran an image whose digest it was not expecting" >&2; exit 1; }
grep -qE "netboot: (giving up|falling back)" "$log2" ||
    { echo "FAIL: the board did not report refusing every offer" >&2; exit 1; }
echo "ok: an image the board was not expecting was refused at the offer"

# ── and an offer from a machine without the private key ────────────────────
#
# The same server, told to stop signing, is indistinguishable from an
# impostor: it can still answer the sweep, and the board must still refuse
# it. This is the half a pinned digest cannot do — the image here is the
# right one, and it is turned down anyway because of who is offering it.
kill $srv 2>/dev/null || true
wait $srv 2>/dev/null || true
unsigned_log="$tmp/server-unsigned.log"
"$server" -images "$images" -addr 127.0.0.1:7634 >"$unsigned_log" 2>&1 &
srv=$!
for _ in $(seq 1 50); do
    grep -q "serving" "$unsigned_log" && break
    sleep 0.1
done
log3="$tmp/console-unsigned.log"
status=0
timeout 120 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log3" \
    -nic user -kernel "$image" \
    -append "console=uart0 minikern.netboot minikern.halt=poweroff minikern.netboot.key=$key" || status=$?
echo "---- console (a server that cannot sign)"
cat "$log3"
[ "$status" -eq 0 ] || { echo "FAIL: qemu exited $status against an unsigned server" >&2; exit 1; }
grep -q "netbooted init running" "$log3" &&
    { echo "FAIL: the board booted an image from a server that could not sign for it" >&2; exit 1; }
grep -qE "netboot: (giving up|falling back)" "$log3" ||
    { echo "FAIL: the board did not report refusing the unsigned offers" >&2; exit 1; }
echo "ok: offers from a machine without the private key were refused"
! grep -qF "PANIC" "$log" || fail "the kernel panicked"
! grep -qF "falling back" "$log" || fail "netboot failed and fell back to an initrd"
! grep -qF "digest" "$log" || fail "a digest problem was reported"

# The server must have served the whole image and refused nothing.
kill $srv 2>/dev/null || true
wait $srv 2>/dev/null || true
grep -q "0 refused" "$srvlog" || fail "the server refused something: $(grep stopped "$srvlog")"
cat "$log"
grep -E "stopped|offer" "$srvlog"
echo PASS
