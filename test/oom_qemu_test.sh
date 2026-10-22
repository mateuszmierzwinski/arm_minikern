#!/usr/bin/env bash
# What the kernel does when there is no page left.
#
# The policy has two halves and this checks both: memory is handed out
# optimistically, so a program may map far more than the board has; and the
# process that finally cannot be given a page is **killed** — with SIGKILL,
# which it cannot catch and cannot mistake for a bad pointer of its own.
#
# A segmentation fault here would be the wrong answer, and a plausible one:
# that is what the kernel used to send, and a Go program turned it into
# "invalid memory address" with the real cause nowhere in sight.
set -euo pipefail

image="$1"
rootfs="$2"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
log="$tmp/oom.log"

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

status=0
timeout 120 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
    -nic user -kernel "$image" -initrd "$rootfs" \
    -append "console=uart0 minikern.halt=poweroff minikern.net=off" || status=$?
echo "---- console (the tail of it)"
tail -12 "$log"
[ "$status" -eq 0 ] || { echo "FAIL: qemu exited $status (124 = hang)" >&2; exit 1; }

expect() {
    grep -qF -- "$1" "$log" || { echo "FAIL: the console lacks: $1" >&2; exit 1; }
}

# It really did get a long way first: optimistic hand-out is the other half
# of the policy, and a kernel that refused early would never print this.
grep -q "oomtest: 256 MiB resident" "$log" ||
    { echo "FAIL: it did not get as far as 256 MiB, so nothing was handed out optimistically" >&2; exit 1; }

expect "out of memory at"
expect "killed"
expect "minikern: init killed by signal 9"
grep -q "SIGSEGV\|segmentation" "$log" &&
    { echo "FAIL: it was reported as a segmentation fault, which hides the cause" >&2; exit 1; }
echo "ok: the board handed out what it had and then killed the process that asked for more"
echo PASS
