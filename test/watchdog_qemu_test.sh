#!/usr/bin/env bash
# The watchdog, in both directions — which is the only way to test one.
#
#   - A kernel that stops ticking must be rebooted. `minikern.selftest=wedge`
#     masks interrupts and spins, which is what a deadlock looks like and
#     what nothing else here can report: no print, no panic, no answer to any
#     question. The boot banner appearing again is the reboot.
#   - A kernel that is fine must be left alone. That is the half that decides
#     whether a watchdog stays switched on in practice, so it is tested too:
#     a normal boot with the watchdog running must reach its power-off.
set -euo pipefail

image="$1"
rootfs="$2"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

# ── a wedged kernel is rebooted ────────────────────────────────────────────
log="$tmp/wedge.log"
# It never powers off: it reboots and wedges again, so the timeout is the
# way out and its status is expected rather than a failure.
timeout 20 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
    -nic user -kernel "$image" \
    -append "console=uart0 minikern.halt=poweroff minikern.net=off minikern.watchdog=2 minikern.selftest=wedge" \
    >/dev/null 2>&1 || true
echo "---- a wedged kernel, with the watchdog running"
head -20 "$log"
boots=$(grep -c "minikern 1.0.0" "$log" || true)
echo "boot banners seen: $boots"
grep -q "watchdog: the board reboots if the kernel stops ticking" "$log" ||
    { echo "FAIL: the watchdog did not start" >&2; exit 1; }
grep -q "wedging the kernel" "$log" || { echo "FAIL: the kernel did not wedge" >&2; exit 1; }
[ "$boots" -ge 2 ] ||
    { echo "FAIL: the board booted $boots time(s): a wedged kernel was not rebooted" >&2; exit 1; }
echo "ok: a wedged kernel was rebooted $((boots - 1)) time(s) in 20 s"

# ── a healthy kernel is left alone ─────────────────────────────────────────
log="$tmp/healthy.log"
status=0
timeout 60 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
    -nic user -kernel "$image" -initrd "$rootfs" \
    -append "console=uart0 minikern.halt=poweroff minikern.net=off minikern.watchdog=2" \
    >/dev/null 2>&1 || status=$?
echo "---- a healthy kernel, with the watchdog running"
cat "$log"
[ "$status" -eq 0 ] || { echo "FAIL: qemu exited $status: the watchdog rebooted a healthy board" >&2; exit 1; }
boots=$(grep -c "minikern 1.0.0" "$log" || true)
[ "$boots" -eq 1 ] || { echo "FAIL: the board booted $boots times when nothing was wrong" >&2; exit 1; }
grep -q "init exited with status 7" "$log" || { echo "FAIL: init did not finish" >&2; exit 1; }
grep -q "minikern: powering off" "$log" || { echo "FAIL: the board did not power off" >&2; exit 1; }
echo "ok: a healthy kernel ran to its power-off with the watchdog on"
echo PASS
