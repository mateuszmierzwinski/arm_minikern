#!/usr/bin/env bash
# The real boot chain, from an SD-card image: BROM → SPL → U-Boot → bootm.
#
# Every other QEMU test here uses `-kernel`, which hands the kernel straight
# to the emulator and skips everything a board actually does. This one builds
# the card image that would be written to a real NanoPi NEO and boots *that*,
# so the path under test is the one the hardware uses: U-Boot reads the SPL
# from sector 16, runs `boot.scr` from the FAT partition, and `bootm`s the
# kernel with a ramdisk and **U-Boot's own device tree** — no ATAGs anywhere,
# which is what a modern sunxi U-Boot gives you.
#
# What only this test can show (both measured, both now in CLAUDE.md §3):
#   * the generic timer runs at **24 MHz**, the real H3 frequency, where the
#     `-kernel` path reports 62.5 MHz — so a kernel that hard-coded either
#     number would be wrong on one path and nobody would notice;
#   * U-Boot's own PSCI implements CPU_ON (all four cores come up) but
#     **refuses SYSTEM_OFF**, so a board booted this way cannot power itself
#     off and the kernel has to idle instead.
#
# That refusal is why this test does not wait for QEMU to exit: it waits for
# the lines it needs and then stops the machine itself.
set -euo pipefail

image="$1"
rootfs="$2"
mksdcard="$3"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
log="$tmp/console.log"
card="$tmp/sd.img"

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }
uboot="${MINIKERN_UBOOT:-}"
[ -n "$uboot" ] && [ -f "$uboot" ] || {
    echo "FAIL: no U-Boot image at \"$uboot\"." >&2
    echo "Fedora 43 ships no armv7 U-Boot; build one with scripts/build_uboot.sh" >&2
    echo "(the path is MINIKERN_UBOOT, from toolchain/paths.bzl)." >&2
    exit 1
}

"$mksdcard" -u "$uboot" -k "$image" -r "$rootfs" -o "$card" \
    -a "console=uart0 minikern.halt=poweroff minikern.netstat=1"

: > "$log"
timeout 180 qemu-system-arm -M orangepi-pc -display none -monitor none \
    -serial "file:$log" -nic user -sd "$card" >/dev/null 2>&1 &
qemu=$!
trap 'kill $qemu 2>/dev/null || true; wait $qemu 2>/dev/null || true' EXIT

# The sentinel is the kernel reaching the end of its own work, not QEMU
# exiting: U-Boot's PSCI refuses to power the machine off.
for _ in $(seq 1 170); do
    grep -q "firmware refused power-off" "$log" 2>/dev/null && break
    kill -0 $qemu 2>/dev/null || break
    sleep 1
done

cat "$log"

fail() { echo "FAIL: $1" >&2; exit 1; }
expect() { grep -qF -- "$1" "$log" || fail "the console lacks: $1"; }

# The chain, stage by stage, so a break says which stage broke.
expect "U-Boot SPL 2025"
expect "Allwinner H3"
expect "minikern: loading from mmc 0:1"
expect "Loading Device Tree"
expect "Starting kernel ..."
# The kernel, and that it got its machine description from the device tree
# U-Boot passed rather than from ATAGs.
expect "minikern 1.0.0"
expect "boot: info fdt at"
grep -q "boot: info atags" "$log" && fail "it read ATAGs, which a sunxi U-Boot does not provide"
expect "mem: 512 MiB at 40000000"
# The real silicon frequency, which only this path reports.
expect "timer 24.000 MHz"
expect "smp: 4 of 4 cores up"
# And a Go program from the ramdisk U-Boot loaded, which proves the whole
# card — partition, FAT, ramdisk wrapper and all — not just the kernel.
expect "ALL PASS"
expect "firmware refused power-off"
! grep -qF "PANIC" "$log" || fail "the kernel panicked"
echo "ok: BROM → SPL → U-Boot → bootm → minikern → a Go program, from one card image"
echo PASS
