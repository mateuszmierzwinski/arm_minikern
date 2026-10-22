#!/usr/bin/env bash
# Boots a Go program that asks the kernel how many CPUs it has and then runs
# one goroutine per CPU simultaneously, and checks both halves of the claim:
#
#   - user space:  Go sees four CPUs, sizes GOMAXPROCS from them, and gets all
#                  four goroutines running at once;
#   - the kernel:  every core actually entered the kernel. That counter is the
#                  part a program cannot claim for itself — on one core the
#                  program above passes too, only more slowly.
#
# It then boots the same program with minikern.smp=off and requires the
# opposite: one CPU reported, and the other cores never started.
set -euo pipefail

image="$1"
rootfs="$2"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

boot() {
    local name="$1" extra="$2" status=0
    log="$tmp/$name.log"
    timeout 180 qemu-system-arm -M orangepi-pc -display none -monitor none \
        -serial "file:$log" -nic user -kernel "$image" -initrd "$rootfs" \
        -append "console=uart0 minikern.halt=poweroff minikern.hangcheck=20 minikern.net=off minikern.netstat=1$extra" \
        || status=$?
    echo "---- $name"
    cat "$log"
    [ "$status" -eq 0 ] || { echo "FAIL[$name]: qemu exited $status (124 = hang)" >&2; exit 1; }
}

expect() {
    grep -qF -- "$1" "$log" || { echo "FAIL: $log lacks: $1" >&2; exit 1; }
}

refuse() {
    ! grep -qF -- "$1" "$log" || { echo "FAIL: $log must not contain: $1" >&2; exit 1; }
}

# Every core named in the per-core line must have been inside the kernel. A
# zero there means a core was started, reported itself, and then did nothing.
expect_cores_busy() {
    local want="$1" line counts n
    line=$(grep -o 'per core.*' "$log" | tail -1) || true
    [ -n "$line" ] || { echo "FAIL: no per-core counter line" >&2; exit 1; }
    counts=${line#per core }
    n=0
    for c in $counts; do
        n=$((n + 1))
        if [ "$n" -le "$want" ] && [ "$c" -eq 0 ]; then
            echo "FAIL: cpu $((n - 1)) never entered the kernel ($line)" >&2
            exit 1
        fi
    done
    [ "$n" -ge "$want" ] || { echo "FAIL: $n cores reported, wanted $want" >&2; exit 1; }
    echo "ok: $want cores each entered the kernel ($line)"
}

boot smp_on ""
expect "smp: 4 of 4 cores up"
expect "cpus 4, gomaxprocs 4"
expect "all 4 goroutines ran at once"
expect "smptest ok"
expect "smp: 4 cores scheduling"
expect "init exited with status 7"
expect_cores_busy 4

# The floating-point state is moved only when a thread uses it, which is
# only worth doing if it really is less often than every switch. The kernel
# reports both numbers so the claim can be checked rather than believed.
line=$(grep -o 'vfp: state moved.*' "$log" | tail -1)
moved=$(echo "$line" | awk '{print $4}')
switches=$(echo "$line" | awk '{print $7}')
echo "$line"
[ "$moved" -gt 0 ] || { echo "FAIL: the lazy path was never taken, so nothing was tested" >&2; exit 1; }
[ "$moved" -lt "$switches" ] ||
    { echo "FAIL: it moved $moved times in $switches switches, which is not lazy" >&2; exit 1; }
echo "ok: the floating-point state moved $moved times in $switches switches"

# The same program on one core: it must still pass, and must be told the truth.
boot smp_off " minikern.smp=off"
expect "smp: not starting the other cores"
expect "cpus 1, gomaxprocs 1"
expect "all 1 goroutines ran at once"
expect "smptest ok"
expect "smp: 1 cores scheduling"
refuse "4 of 4 cores up"
expect_cores_busy 1
echo PASS
