#!/usr/bin/env bash
# net_qemu_test.sh <kernel>: the P1 milestone. Boots the kernel on QEMU's
# user-mode network (slirp) and checks, from the console and from the host:
#   - the EMAC and its PHY came up,
#   - DHCP produced slirp's lease (10.0.2.15/24, gateway 10.0.2.2),
#   - the gateway answered our pings,
#   - a UDP datagram sent from the host to the guest's echo service (port 7,
#     reached through hostfwd) comes back unchanged.
set -euo pipefail

image="$1"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
log="$tmp/console.log"
port=$((20000 + RANDOM % 20000))

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
    -nic "user,hostfwd=udp:127.0.0.1:$port-:7" -kernel "$image" -append "console=uart0" &
qemu=$!
trap 'kill $qemu 2>/dev/null || true; wait $qemu 2>/dev/null || true' EXIT

fail() {
    cat "$log"
    echo "FAIL: $1" >&2
    exit 1
}

wait_for() {
    for _ in $(seq 1 100); do
        grep -qF -- "$1" "$log" 2>/dev/null && return 0
        sleep 0.1
    done
    fail "console never showed: $1"
}

wait_for "link up 100M full duplex"
wait_for "net: eth0 10.0.2.15/24 gw 10.0.2.2"
wait_for "net: 10.0.2.2 answered ping seq 3"

payload="minikern-echo-$port"
exec 3<>"/dev/udp/127.0.0.1/$port"
printf '%s' "$payload" >&3
reply=""
read -r -t 5 -N "${#payload}" reply <&3 || true
exec 3<&-
[ "$reply" = "$payload" ] || fail "UDP echo returned '$reply', want '$payload'"
cat "$log"
echo "UDP echo through hostfwd: ok"
! grep -qF "PANIC" "$log" || fail "the kernel panicked"
echo PASS
