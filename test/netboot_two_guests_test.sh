#!/usr/bin/env bash
# Discovery between two machines, with nothing else on the segment.
#
# Every other netboot test runs through the emulator's address translation,
# which answers probes for addresses nobody owns and invents hosts
# (CLAUDE.md §10) — so "it found the server" has always meant "it found the
# host". Here two guests share an L2 segment of their own
# (`-nic socket,mcast=`): one runs the server, built from the same packages
# the host's server is, and the other netboots from it. Nothing on the host
# takes part in the conversation.
#
# There is also no DHCP server on such a segment, which is why both guests
# are given their addresses on the command line.
set -euo pipefail

image="$1"
server_rootfs="$2"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
server_log="$tmp/server.log"
client_log="$tmp/client.log"
# The segment is a socket pair rather than a multicast group: QEMU's
# multicast backend needs a host route for the group, and two guests joined
# to one with nothing routing it never see each other's frames (which is
# what the first version of this test spent 250 probes proving). A listen
# and a connect is the same thing a cable between two boards is, and needs
# nothing of the host but a port.
segment_port="${MKNB_SEGMENT_PORT:-11834}"

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

guest() {
    local log="$1" nic="$2" append="$3" initrd="${4:-}"
    local args=(-M orangepi-pc -display none -monitor none -serial "file:$log"
                -nic "$nic" -kernel "$image" -append "$append")
    [ -n "$initrd" ] && args+=(-initrd "$initrd")
    timeout 90 qemu-system-arm "${args[@]}" >/dev/null 2>&1 || true
}

# The server boots first and stays up: an init that exits powers its board
# off, which would take the server away mid-transfer.
# Two different hardware addresses, because QEMU gives every machine the
# same chip serial number and the address is normally derived from it: two
# guests sharing one address cannot talk to each other at all.
guest "$server_log" "socket,listen=127.0.0.1:$segment_port" \
    "console=uart0 minikern.ip=10.9.0.1/24 minikern.mac=02:00:00:00:09:01" "$server_rootfs" &
server_pid=$!
trap 'kill $server_pid 2>/dev/null || true; wait $server_pid 2>/dev/null || true' EXIT

for _ in $(seq 1 100); do
    grep -q "netbootsrv: listening" "$server_log" 2>/dev/null && break
    sleep 0.2
done
grep -q "netbootsrv: listening" "$server_log" ||
    { echo "---- server"; cat "$server_log"; echo "FAIL: the serving guest never listened" >&2; exit 1; }

# The client has no initrd at all: whatever it runs, it got over the wire.
guest "$client_log" "socket,connect=127.0.0.1:$segment_port" \
    "console=uart0 minikern.halt=poweroff minikern.ip=10.9.0.2/24 minikern.mac=02:00:00:00:09:02 minikern.netboot=10.9.0.1"

echo "---- the serving guest"
cat "$server_log"
echo "---- the client guest"
cat "$client_log"

expect() {
    grep -qF -- "$1" "$client_log" || { echo "FAIL: the client lacks: $1" >&2; exit 1; }
}

expect "net: eth0 10.9.0.2/24"
expect "configured, no dhcp"
expect "netboot: server at 10.9.0.1 offers"
expect "minikern: started /init as pid 1"
# This is the point of the test: with no initrd, the only way a program ran
# is that it arrived from the other guest.
expect "hello from gc Go on minikern"
expect "minikern: powering off"
grep -q "10.0.2" "$client_log" &&
    { echo "FAIL: the client talked to the emulator's network, not the other guest" >&2; exit 1; }
grep -q "served" "$server_log" || grep -q "offer" "$server_log" ||
    { echo "FAIL: the serving guest does not report having served anything" >&2; exit 1; }
echo "ok: one board served another its root filesystem, with nothing else on the segment"
echo PASS
