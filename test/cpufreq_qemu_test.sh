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

run() {
    status=0
    timeout 180 qemu-system-arm -M orangepi-pc -display none -monitor none \
        -serial "file:$log" -nic user -kernel "$image" -initrd "$rootfs" \
        -append "console=uart0 minikern.halt=poweroff minikern.hangcheck=60 minikern.net=off minikern.netstat=1 minikern.cpufreq=test${2:-}$1" \
        || status=$?
    cat "$log"
    [ "$status" -eq 0 ] || { echo "FAIL: qemu exited $status (124 = hang)" >&2; exit 1; }
}

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
# ── the operating points the supply gates ──────────────────────────────────
#
# The points above 816 MHz need the higher VDD_CPUX, and which pin switches
# that is a board fact given on the command line. Without it those points
# must not be offered at all — asking for a frequency the core cannot be fed
# at is the one mistake here that does not announce itself.
grep -q "1008000 kHz" "$log" &&
    { echo "FAIL: a point needing the higher supply was used with no pin to raise it" >&2; exit 1; }
echo "ok: without a supply pin, only the points the fixed supply can feed were used"

log="$tmp/cpufreq-vdd.log"
run " minikern.vddcpux=PA10"
expect "cpufreq: VDD_CPUX switches on PA10"
expect "1008000 kHz at 1300 mV"
expect "freqtest ok"
echo "ok: with a supply pin, the whole published table is reached"

# ── a sensor whose reading cannot be believed ──────────────────────────────
#
# A sensor that cannot be *read* was always treated as the worst case. A
# sensor that answers with an impossible number was not, and that is the
# dangerous one: this chip's conversion turns the sample from an absent or
# unclocked block into -264 °C, and a governor that believes it goes to the
# top of the table and stays there with nothing watching the temperature.
#
# The pair is what makes it a proof. A reading that is cold but **possible**
# must still boost, or "it stayed at the bottom" would say nothing about
# rejecting the impossible one.
log="$tmp/cpufreq-cold.log"
run " minikern.vddcpux=PA10" "-coldsensor"
expect "1008000 kHz at 1300 mV"
grep -q "rejected as impossible" "$log" &&
    { echo "FAIL: 20 °C was rejected, and it is a temperature a board really reaches" >&2; exit 1; }
echo "ok: a cold but possible reading still reaches the top of the table"

log="$tmp/cpufreq-bad.log"
run " minikern.vddcpux=PA10" "-badsensor"
expect "cpufreq: critical temperature,"
expect "rejected as impossible"
# Under a full load, with the supply pin present, and still at the lowest
# point: the governor refused to act on the reading rather than trusting it.
grep -qE "cpufreq: 480000 kHz \(point 1 of 5\)" "$log" ||
    { echo "FAIL: it did not end at the lowest point" >&2; exit 1; }
for faster in 648000 816000 912000 1008000; do
    grep -q "cpufreq: $faster kHz at" "$log" &&
        { echo "FAIL: it sped up to $faster kHz on a reading no die could produce" >&2; exit 1; }
done
echo "ok: an impossible reading is refused, and the governor assumes the worst instead"

echo PASS
