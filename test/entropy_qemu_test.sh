#!/usr/bin/env bash
# Two boots of the same image must not produce the same random bytes.
#
# The kernel's generator is a good sequence from a seed that used to be the
# timer and the chip's serial number — and QEMU gives every machine the same
# serial number and starts the timer at zero, so two boots produced the
# **same** bytes. That is fine for the hash seeds Go asks for and not fine
# for a key, which matters now that this tree verifies signatures.
#
# What makes them differ is harvesting the times at which interrupts arrive.
# What this test can show is that mechanism working; it cannot show that the
# bytes are unpredictable, and does not claim to — on real hardware that
# jitter is physical, under an emulator it follows the host's scheduler
# (kernel/entropy.h).
set -euo pipefail

image="$1"
rootfs="$2"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

boot() {
    local log="$1" status=0
    timeout 120 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
        -nic user -kernel "$image" -initrd "$rootfs" \
        -append "console=uart0 minikern.halt=poweroff" || status=$?
    [ "$status" -eq 0 ] || { cat "$log"; echo "FAIL: qemu exited $status" >&2; exit 1; }
    # The console ends its lines with a carriage return, which would
    # otherwise count as a seventeenth byte.
    sed -n 's/^random: //p' "$log" | head -1 | tr -d '\r' 
}

first="$(boot "$tmp/a.log")"
second="$(boot "$tmp/b.log")"
echo "first boot:  $first"
echo "second boot: $second"

[ -n "$first" ] && [ -n "$second" ] || { echo "FAIL: a boot printed no random bytes" >&2; exit 1; }
[ "${#first}" -eq 32 ] || { echo "FAIL: expected sixteen bytes, got \"$first\"" >&2; exit 1; }
[ "$first" != "$second" ] ||
    { echo "FAIL: two boots produced the same bytes, so nothing unpredictable reached the pool" >&2; exit 1; }
echo "ok: two boots of one image produced different bytes"
echo PASS
