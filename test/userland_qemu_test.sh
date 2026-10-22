#!/usr/bin/env bash
# userland_qemu_test.sh <kernel> <initrd> <expected line>...
#
# Boots the kernel under QEMU with the initrd as the root filesystem and
# minikern.halt=poweroff, so the machine powers off when /init ends. Passes
# when QEMU exits by itself, every expected line appears in the console
# log, and the kernel neither panicked nor killed init with a signal.
set -euo pipefail

image="$1"
initrd="$2"
shift 2
log="${TEST_TMPDIR:-$(mktemp -d)}/console.log"

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

status=0
timeout 120 qemu-system-arm -M orangepi-pc -display none -monitor none \
    -serial "file:$log" -nic user -kernel "$image" -initrd "$initrd" \
    -append "console=uart0 minikern.halt=poweroff ${MINIKERN_EXTRA_CMDLINE:-}" || status=$?
cat "$log"

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

[ "$status" -eq 0 ] || fail "qemu exited $status (124 = hang, no power-off)"
for want in "$@"; do
    grep -qF -- "$want" "$log" || fail "console lacks: $want"
done
! grep -qF "PANIC" "$log" || fail "the kernel panicked"
! grep -qF "killed by signal" "$log" || fail "init was killed by a signal"
grep -qF "minikern: powering off" "$log" || fail "no orderly power-off"
echo PASS
