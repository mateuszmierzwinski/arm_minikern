#!/usr/bin/env bash
# The frequency governor, driven by a fake thermal sensor that ramps from
# cold to past critical and back while a Go program holds every core busy.
#
# What this can prove: that load is measured, that the policy is consulted,
# that every decision it can reach is reached, and that the result is
# applied and logged. What it cannot prove is the two register writes
# underneath — QEMU models neither the H3's thermal sensor nor its PLLs — so
# those stay off by default and wait for the board (board/h3/ths.h).
set -euo pipefail

image="$1"
rootfs="$2"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
log="$tmp/cpufreq.log"

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

status=0
timeout 180 qemu-system-arm -M orangepi-pc -display none -monitor none \
    -serial "file:$log" -nic user -kernel "$image" -initrd "$rootfs" \
    -append "console=uart0 minikern.halt=poweroff minikern.hangcheck=60 minikern.net=off minikern.netstat=1 minikern.cpufreq=test" \
    || status=$?
cat "$log"
[ "$status" -eq 0 ] || { echo "FAIL: qemu exited $status (124 = hang)" >&2; exit 1; }

expect() {
    grep -qF -- "$1" "$log" || { echo "FAIL: the log lacks: $1" >&2; exit 1; }
}

expect "cpufreq: test governor started"
expect "freqtest ok"
expect "init exited with status 7"

# Every decision the policy can make, each one reached by the ramp.
expect "(load,"                 # a busy CPU is allowed to speed up
expect "(throttling,"           # the ramp crossed the throttle point
# The critical reading is reported even though there is no frequency left
# to give up, which is the whole point of reporting it.
expect "cpufreq: critical temperature,"

# The frequency really moved: more than one distinct point appears.
points=$(grep -o 'cpufreq: [0-9]* kHz at' "$log" | sort -u | wc -l)
[ "$points" -ge 2 ] || { echo "FAIL: only $points operating point(s) were applied" >&2; exit 1; }
echo "ok: $points distinct operating points were applied"

# It must come back down the other side of the ramp, not sit at the bottom:
# the last decision of a cooling CPU under load is to speed up again.
tail -40 "$log" | grep -qF "(load," || { echo "FAIL: it never recovered as the sensor cooled" >&2; exit 1; }
echo PASS
