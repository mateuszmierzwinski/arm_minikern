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

"$server" -images "$images" -addr 127.0.0.1:7634 >"$srvlog" 2>&1 &
srv=$!
trap 'kill $srv 2>/dev/null || true; wait $srv 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
    grep -q "serving" "$srvlog" && break
    sleep 0.1
done

status=0
timeout 120 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
    -nic user -kernel "$image" -append "console=uart0 minikern.netboot minikern.halt=poweroff" || status=$?

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
