#!/usr/bin/env bash
# net_sockets_qemu_test.sh <kernel> <rootfs> <netecho>: Go's net package on
# minikern, over both protocols and in both directions.
#
# The guest's nettest init checks what it can reach by itself (UDP and TCP
# over loopback, including a complete HTTP exchange) and then waits for the
# host. The host sends a UDP datagram and opens a TCP connection to the
# guest's forwarded ports, and runs an echo service the guest dials out to.
#
# That last one needs an address the guest's traffic can reach through
# slirp's NAT, which is the host's own address on the real network: slirp
# does not forward guest TCP to 10.0.2.2. When the machine has no such
# address the guest reports that one check as SKIP, and the rest still run.
set -euo pipefail

image="$1"
rootfs="$2"
netecho="$3"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
log="$tmp/console.log"
ready="$tmp/netecho.ready"
udp_port=47101
tcp_port=47102
guest_udp=$((31000 + RANDOM % 15000))
guest_tcp=$((31000 + RANDOM % 15000))

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

# The host's address on the real network, if it has one.
host_ip="$(ip -4 -o addr show scope global 2>/dev/null | awk 'NR==1{split($4,a,"/"); print a[1]}')"
tcp_echo=""
if [ -n "$host_ip" ]; then
    tcp_echo="$host_ip:$tcp_port"
    echo "the guest will dial $tcp_echo for the outbound TCP check"
else
    echo "no routable host address: the outbound TCP check will be skipped"
fi

"$netecho" -tcp "$tcp_echo" -udp "127.0.0.1:$udp_port" -ready-file "$ready" >"$tmp/netecho.log" 2>&1 &
echo_pid=$!
qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
    -nic "user,hostfwd=udp:127.0.0.1:$guest_udp-:9000,hostfwd=tcp:127.0.0.1:$guest_tcp-:9001" \
    -kernel "$image" -initrd "$rootfs" \
    -append "console=uart0 minikern.halt=poweroff minikern.env.MINIKERN_TCP_ECHO=$tcp_echo \
minikern.env.MINIKERN_UDP_ECHO=10.0.2.2:$udp_port" &
qemu=$!
trap 'kill $echo_pid $qemu 2>/dev/null || true; wait 2>/dev/null || true' EXIT

fail() {
    echo "---- console"; cat "$log" 2>/dev/null || true
    echo "---- netecho"; cat "$tmp/netecho.log" 2>/dev/null || true
    echo "FAIL: $1" >&2
    exit 1
}

wait_for() {
    for _ in $(seq 1 300); do
        grep -qF -- "$1" "$log" 2>/dev/null && return 0
        kill -0 $qemu 2>/dev/null || fail "qemu exited before: $1"
        sleep 0.2
    done
    fail "console never showed: $1"
}

for _ in $(seq 1 50); do
    [ -f "$ready" ] && break
    sleep 0.1
done
[ -f "$ready" ] || fail "netecho did not start: $(cat "$tmp/netecho.log" 2>/dev/null)"

# What the guest proves on its own, before it waits for us.
wait_for "PASS udp-loopback"
wait_for "PASS udp-client"
wait_for "PASS tcp-http-loopback"
if [ -n "$tcp_echo" ]; then
    wait_for "PASS tcp-client"
fi

# The host's turn: a datagram, then a connection.
wait_for "READY udp-server"
exec 3<>"/dev/udp/127.0.0.1/$guest_udp"
printf 'shout' >&3
udp_reply=""
read -r -t 15 -N 5 udp_reply <&3 || true
exec 3<&-
[ "$udp_reply" = "SHOUT" ] || fail "the guest's UDP server replied '$udp_reply', want 'SHOUT'"

wait_for "READY tcp-server"
exec 4<>"/dev/tcp/127.0.0.1/$guest_tcp"
printf 'knock' >&4
tcp_reply=""
read -r -t 15 -N 5 tcp_reply <&4 || true
exec 4<&-
[ "$tcp_reply" = "KNOCK" ] || fail "the guest's TCP server replied '$tcp_reply', want 'KNOCK'"

wait_for "ALL NET PASS"
wait_for "init exited with status 0"
cat "$log"
! grep -qF "PANIC" "$log" || fail "the kernel panicked"
! grep -qF "FAIL " "$log" || fail "the guest reported a failed check"
echo "host <-> guest, UDP and TCP, both ways: ok"
echo PASS
