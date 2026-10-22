#!/usr/bin/env bash
# Boots a minikern image on QEMU's orangepi-pc — the same Allwinner H3 the
# NanoPi NEO carries. The serial console is this terminal; Ctrl-A X quits.
#
#   bazel run //:qemu [-- extra qemu args]
#   MINIKERN_CMDLINE="minikern.halt=poweroff" bazel run ...:qemu
#
# The machine only accepts its own 1 GiB of RAM; the kernel clamps to the
# NEO's 512 MiB itself. -nic user is QEMU's built-in NAT (DHCP 10.0.2.15,
# gateway 10.0.2.2) wired to the H3's EMAC.
set -euo pipefail

image="$1"
shift
exec qemu-system-arm \
    -M orangepi-pc \
    -nographic \
    -nic user \
    -kernel "$image" \
    -append "${MINIKERN_CMDLINE:-console=uart0}" \
    "$@"
