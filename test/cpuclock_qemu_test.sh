#!/usr/bin/env bash
# The CPU clock adapter, driven for real against QEMU's clock block.
#
# The governor's own test drives a **fake** clock, which proves the policy
# and proves nothing about the registers. QEMU does model the H3's clock
# control unit (`allwinner-h3-ccu`), so this drives the real adapter through
# every published operating point and reads PLL_CPUX back: the factor
# arithmetic, the field placement, the clock-source handover and each
# refusal are all checked against a model of the block rather than against a
# stand-in. It is the standard the EMAC driver was first held to.
#
# It is not silicon, and the header says what is still the board's to settle
# (board/h3/cpux.h). But it is what found the bug it now guards: with only N
# varied, the PLL stopped at 768 MHz and **three of the chip's five points —
# 816, 912 and 1008 MHz — had been failing with EINVAL**, unnoticed, because
# nothing had ever run that arithmetic against the register it writes.
set -euo pipefail

image="$1"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

probe() {
    local log="$1" extra="$2" status=0
    timeout 120 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
        -nic none -kernel "$image" \
        -append "console=uart0 minikern.halt=poweroff minikern.net=off minikern.cpufreq=probe $extra" ||
        status=$?
    [ "$status" -eq 0 ] || { cat "$log"; echo "FAIL: qemu exited $status" >&2; exit 1; }
    ! grep -qF PANIC "$log" || { cat "$log"; echo "FAIL: the kernel panicked" >&2; exit 1; }
    grep -qF "cpuclock: probe done" "$log" || { cat "$log"; echo "FAIL: the probe did not finish" >&2; exit 1; }
}

# Every point the adapter is asked for must be taken, and the register must
# read back as the frequency that was asked for — the one check that would
# have caught the factor bug the moment it was written.
check_points() {
    local log="$1" want="$2" seen=0
    while read -r asked got rc; do
        seen=$((seen + 1))
        [ "$rc" = "0" ] || { echo "FAIL: $asked kHz was refused with $rc" >&2; exit 1; }
        [ "$asked" = "$got" ] ||
            { echo "FAIL: asked for $asked kHz, the PLL register reads back $got kHz" >&2; exit 1; }
    done < <(sed -n 's/^cpuclock: asked \([0-9]*\) kHz at [0-9]* mV -> \(-*[0-9]*\), pll ctrl [0-9a-f]* = \([0-9]*\) kHz.*/\1 \3 \2/p' "$log" | tr -d '\r')
    [ "$seen" = "$want" ] || { echo "FAIL: $seen points probed, expected $want" >&2; exit 1; }
    echo "ok: $seen operating points set and read back exactly"
}

# With the supply pin: the whole published table, including the three points
# above 768 MHz that need K as well as N.
probe "$tmp/pin.log" "minikern.vddcpux=PA10"
cat "$tmp/pin.log"
check_points "$tmp/pin.log" 5
for mhz in 480000 816000 1008000; do
    grep -qE "^cpuclock: asked $mhz kHz .* = $mhz kHz, src 2, enabled 1" "$tmp/pin.log" ||
        { echo "FAIL: $mhz kHz did not end on the PLL with it enabled" >&2; exit 1; }
done
echo "ok: the cores were handed back to the PLL at every point"

# The refusals. Each is arithmetic the board could never be asked about, and
# each must come back as an error rather than as a wrong frequency.
for want in "12000 kHz (below the crystal) -> -22" \
            "500000 kHz (not a multiple of 24 MHz) -> -22" \
            "3096000 kHz (no N and K reach it) -> -22"; do
    grep -qF -- "cpuclock: $want" "$tmp/pin.log" || { echo "FAIL: expected refusal: $want" >&2; exit 1; }
done
echo "ok: every out-of-range request was refused"

# Without the pin, the policy offers only what the fixed supply can feed, so
# the adapter is never asked for a point it would have to brown out to reach.
probe "$tmp/nopin.log" ""
check_points "$tmp/nopin.log" 3
grep -q "cpuclock: asked 912000" "$tmp/nopin.log" &&
    { echo "FAIL: a point needing the higher supply was offered without a pin" >&2; exit 1; }
echo "ok: without a supply pin, only the points the fixed supply can feed are attempted"
echo PASS
