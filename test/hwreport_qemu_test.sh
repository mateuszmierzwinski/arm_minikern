#!/usr/bin/env bash
# The hardware report, and the part of it QEMU can already judge.
#
# `minikern.hwreport=1` prints every value this kernel took from the
# datasheet beside what the datasheet says it should be, so that powering a
# board up produces one artefact a reader can check (board/h3/hwreport.h).
# This test does two things with it.
#
# It keeps the report honest: a report that quietly loses a line is worse
# than no report, because its absence is what a reader would have to notice.
#
# And it **checks the EMAC's clock and reset bits**, which had been written
# off along with the PHY-select register as "unverified on silicon". They are
# not the same: the gates and resets live in the clock block, which QEMU
# models, so the bits the driver sets can be read back and compared with the
# datasheet here and now. Only the system-control register that selects the
# internal PHY has no model, and it is the one thing left in this area that
# a board has to settle.
set -euo pipefail

image="$1"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
log="$tmp/console.log"

command -v qemu-system-arm >/dev/null || { echo "qemu-system-arm is not installed" >&2; exit 1; }

status=0
timeout 120 qemu-system-arm -M orangepi-pc -display none -monitor none -serial "file:$log" \
    -nic user -kernel "$image" \
    -append "console=uart0 minikern.halt=poweroff minikern.hwreport=1" || status=$?
grep '^hw:' "$log" || true
[ "$status" -eq 0 ] || { cat "$log"; echo "FAIL: qemu exited $status" >&2; exit 1; }

fail() { echo "FAIL: $1" >&2; exit 1; }

# The report's own structure.
# Matched as phrases rather than as "hw: <phrase>": two of the lines carry
# rule-off dashes between the prefix and the words.
for want in "hardware report" "hw: midr " "hw: cntfrq " "hw: sid " "hw: ccu pll_cpux" \
            "hw: syscon ephy_clk" "hw: ths sample 0" "hw: ths sample 7" "end of report" \
            "what only a board can settle"; do
    grep -qF -- "$want" "$log" || fail "the report lacks: $want"
done
echo "ok: the report is complete"

# The clock and reset bits the EMAC driver sets, read back out of the block
# that models them. The driver having run is a precondition: the network is
# up in this boot, which is what sets them.
grep -qF "net: eth0" "$log" || fail "the network did not come up, so the gates mean nothing"

bit_of() { sed -n "s/^hw: ccu $1 *\([0-9a-f]*\).*/\1/p" "$log" | tail -1 | tr -d '\r'; }
check_bit() {
    local reg="$1" bit="$2" what="$3" value
    value="$(bit_of "$reg")"
    [ -n "$value" ] || fail "the report has no $reg line"
    (( (0x$value >> bit) & 1 )) || fail "$what: $reg reads $value, bit $bit is clear"
    echo "ok: $what is set ($reg = $value, bit $bit)"
}
check_bit "bus_gate0" 17 "the EMAC's clock gate"
check_bit "bus_rst0" 17 "the EMAC's reset release"
check_bit "bus_gate4" 0 "the internal PHY's clock gate"
check_bit "bus_rst2" 2 "the internal PHY's reset release"

# And the sensor, which is the opposite case: no model at all, so the only
# correct behaviour is to refuse what it reads.
grep -qF "refused by the driver" "$log" ||
    fail "the sensor read as something believable, which QEMU cannot produce"
echo "ok: the unmodelled sensor's reading is refused rather than believed"
echo PASS
