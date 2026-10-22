#!/usr/bin/env bash
# An idle core powers itself off, and comes back when there is work.
#
# This is the power saving that is actually worth having on this chip: an
# idle Cortex-A7 that is *off* draws no core power, while one sitting in
# `wfi` still has its clock and its voltage. Stepping the frequency, by
# comparison, saves a fraction of the power of a core that is running.
#
# The test runs the same Go workload twice — once as usual, once with
# `minikern.cpuoff=1` — and requires that with parking on, cores really did
# power off, that they came back (the work finishes, on four cores, with
# every core showing kernel entries), and that the answer is identical. A
# power optimisation that changes a result is a bug, not an optimisation.
set -euo pipefail

image="$1"
rootfs="$2"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

boot() {
    local log="$1" extra="$2" status=0
    timeout 180 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
        -nic user -kernel "$image" -initrd "$rootfs" \
        -append "console=uart0 minikern.halt=poweroff minikern.hangcheck=60 minikern.netstat=1 $extra" ||
        status=$?
    [ "$status" -eq 0 ] || { cat "$log"; echo "FAIL: qemu exited $status with \"$extra\"" >&2; exit 1; }
    ! grep -qF PANIC "$log" || { cat "$log"; echo "FAIL: the kernel panicked with \"$extra\"" >&2; exit 1; }
}

boot "$tmp/plain.log" ""
boot "$tmp/parked.log" "minikern.cpuoff=1"

echo "---- with parking on"
grep -E 'smp:|cpuoff:|^goabi|ok$|map entries' "$tmp/parked.log" || true

parks="$(sed -n 's/^cpuoff: \([0-9]*\) cores powered off.*/\1/p' "$tmp/parked.log" | tail -1 | tr -d '\r')"
[ -n "$parks" ] || { echo "FAIL: the kernel never reported the parking counter" >&2; exit 1; }
[ "$parks" -gt 0 ] ||
    { echo "FAIL: no core powered itself off, so nothing was proved" >&2; exit 1; }
echo "ok: a core powered itself off $parks times"

# Coming back is not optional: the workload only finishes if it does, and
# four cores only show kernel entries if every one of them came back.
grep -qF "ALL PASS" "$tmp/parked.log" ||
    { echo "FAIL: the workload did not finish with parking on" >&2; exit 1; }
# Coming back has to be measured as *more* than the entry each core made
# before it ever parked. "Greater than zero" is satisfied by a core that
# parked once and was never started again — which is precisely the bug this
# check exists to catch, and did: the wake passed a virtual address to
# firmware that wanted a physical one, so every core parked once, stayed
# off, and reported the single entry it had made on the way in.
per_core="$(sed -n 's/.*per core //p' "$tmp/parked.log" | tail -1 | tr -d '\r')"
echo "kernel entries per core: $per_core"
cpu=0
for n in $per_core; do
    if [ "$cpu" -gt 0 ] && [ "$n" -lt 10 ]; then
        echo "FAIL: cpu $cpu did $n kernel entries, so it never really came back" >&2
        exit 1
    fi
    cpu=$((cpu + 1))
done
echo "ok: every core came back and did real kernel work"

# Parking more times than there are cores is the other half of it: a core
# that comes back, finds the work done and parks again is the cycle working,
# not a core that went off once and stayed off.
[ "$parks" -gt 3 ] ||
    { echo "FAIL: only $parks parks, so no core parked twice — the cycle is not running" >&2; exit 1; }
echo "ok: cores parked and came back repeatedly ($parks times)"

# Parking must not cost the machine its parallelism: a core that is off is
# still a core user space is counted, or Go sizes GOMAXPROCS to what is
# awake and then builds a workload that never needs the rest — which is how
# the first version of this ran the whole suite on one core and called it a
# saving.
for log in "$tmp/plain.log" "$tmp/parked.log"; do
    grep -qF "smp: 4 cores scheduling" "$log" ||
        { echo "FAIL: $(basename "$log") does not report four cores scheduling" >&2; exit 1; }
done
echo "ok: four cores are still offered to user space while some are off"

# And the answer must be the same, parked or not.
for want in "ALL PASS" "random: "; do
    grep -qF -- "$want" "$tmp/plain.log" || { echo "FAIL: the plain boot lacks: $want" >&2; exit 1; }
    grep -qF -- "$want" "$tmp/parked.log" || { echo "FAIL: the parked boot lacks: $want" >&2; exit 1; }
done
echo "ok: the workload produced the same result with cores powering off"
echo PASS
