#!/usr/bin/env bash
# Boots the kernel under QEMU with minikern.halt=poweroff, once per way a boot
# loader can hand over (ATAGs; device tree + initrd), and checks each boot
# log. PSCI SYSTEM_OFF ends QEMU on its own; the timeout only catches a hang.
set -euo pipefail

image="$1"
dtb="$2"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
initrd="$tmp/initrd"
printf 'minikern initrd\n' >"$initrd"   # 16 bytes

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

# boot <name> <extra cmdline> <qemu args...>: runs one boot into $tmp/<name>.log.
boot() {
    local name="$1" extra="$2" status=0
    shift 2
    log="$tmp/$name.log"
    timeout 20 qemu-system-arm -M orangepi-pc -display none -monitor none \
        -serial "file:$log" -nic user -kernel "$image" \
        -append "console=uart0 minikern.halt=poweroff$extra" "$@" || status=$?
    echo "---- $name"
    cat "$log"
    if [ "$status" -ne 0 ]; then
        echo "FAIL[$name]: qemu exited $status (124 = hang, no power-off)" >&2
        exit 1
    fi
}

expect() {
    if ! grep -qF -- "$1" "$log"; then
        echo "FAIL: $log lacks: $1" >&2
        exit 1
    fi
}

expect_common() {
    expect "minikern "
    expect "entered in hyp mode"
    expect "mem: 512 MiB at 40000000"
    expect 'cmdline: "console=uart0 minikern.halt=poweroff"'
    expect "minikern: boot complete"
    expect "minikern: powering off"
}

boot atags ""
expect_common
expect "boot: info atags at 40000100"

boot atags_initrd "" -initrd "$initrd"
expect_common
expect "boot: info atags at 40000100"
expect "initrd: 16 bytes at "

boot fdt "" -dtb "$dtb" -initrd "$initrd"
expect_common
expect "boot: info fdt at "
expect "initrd: 16 bytes at "
# Each deliberate fault must reach the vectors, be reported and panic.
selftest() {
    boot "selftest_$1" " minikern.selftest=$1"
    expect "$2 at pc "
    expect "in kernel mode"
    expect "PANIC: unhandled $2"
}
selftest undef "undefined instruction"
selftest dabt "data abort"
selftest pabt "prefetch abort"

# The DMA window's cacheable alias is unmapped when the window is created, so
# that no physical page is reachable under two cacheabilities. Reading it by
# its linear address must abort — the half of that fix QEMU can judge.
selftest dma-alias "data abort"
expect "reading the retired dma alias at 0xc"
echo PASS
