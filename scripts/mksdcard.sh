#!/usr/bin/env bash
# Builds a bootable SD-card image: U-Boot, the kernel and a root filesystem.
#
# This is the real boot chain the NanoPi NEO uses, and the one the `-kernel`
# shortcut never exercises: the BROM reads the SPL from sector 16, the SPL
# loads U-Boot, U-Boot runs `boot.scr` from the first partition, and the
# script `bootm`s the kernel with a ramdisk and **U-Boot's own device tree**.
# Everything the kernel learns about the machine on a real board comes
# through that device tree, which is why this path is worth testing.
#
# The same image boots under QEMU (`-M orangepi-pc -sd <image>`) and on the
# board (`dd` it to a card), so the thing shipped to hardware is the thing
# that was tested. The layout:
#
#   sector 16 (8 KiB)   U-Boot: the SPL, padded, then u-boot.img
#   sector 2048 (1 MiB) partition 1, FAT: minikern.uImage, rootfs.uImage, boot.scr
#
# Usage: mksdcard.sh -u <u-boot-with-spl> -k <kernel.uImage> -o <image>
#                    [-r <rootfs.tar>] [-a "<kernel command line>"] [-s <MiB>]
set -euo pipefail

uboot="" kernel="" rootfs="" out="" append="console=uart0" size=64

while getopts "u:k:r:o:a:s:" opt; do
    case "$opt" in
        u) uboot="$OPTARG" ;;
        k) kernel="$OPTARG" ;;
        r) rootfs="$OPTARG" ;;
        o) out="$OPTARG" ;;
        a) append="$OPTARG" ;;
        s) size="$OPTARG" ;;
        *) echo "usage: $0 -u <u-boot> -k <kernel.uImage> -o <image> [-r <rootfs.tar>] [-a <cmdline>] [-s <MiB>]" >&2; exit 1 ;;
    esac
done

[ -n "$uboot" ] && [ -n "$kernel" ] && [ -n "$out" ] ||
    { echo "usage: $0 -u <u-boot> -k <kernel.uImage> -o <image> [-r <rootfs.tar>] [-a <cmdline>] [-s <MiB>]" >&2; exit 1; }
[ -f "$uboot" ] || { echo "no U-Boot image at $uboot (scripts/build_uboot.sh builds one)" >&2; exit 1; }
[ -f "$kernel" ] || { echo "no kernel at $kernel" >&2; exit 1; }
for t in mkimage sfdisk mkfs.vfat mcopy; do
    command -v "$t" >/dev/null || { echo "$t is not installed" >&2; exit 1; }
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The boot script. `${fdtcontroladdr}` is U-Boot's own device tree, which is
# how a kernel on real hardware is told what the machine is; a sunxi U-Boot
# produces no ATAGs at all.
{
    echo "echo minikern: loading from mmc 0:1"
    echo "load mmc 0:1 0x42000000 minikern.uImage"
    echo "setenv bootargs $append"
    if [ -n "$rootfs" ]; then
        echo "load mmc 0:1 0x48000000 rootfs.uImage"
        echo "bootm 0x42000000 0x48000000 \${fdtcontroladdr}"
    else
        echo "bootm 0x42000000 - \${fdtcontroladdr}"
    fi
} > "$work/boot.cmd"
mkimage -C none -A arm -T script -d "$work/boot.cmd" "$work/boot.scr" >/dev/null

# A ramdisk has to be wrapped for bootm, which is also what makes U-Boot fill
# in /chosen/linux,initrd-start for the kernel to find.
if [ -n "$rootfs" ]; then
    mkimage -A arm -O linux -T ramdisk -C none -n rootfs -d "$rootfs" "$work/rootfs.uImage" >/dev/null
fi

part_mib=$((size - 1))
truncate -s "${size}M" "$out"
printf 'label: dos\nstart=2048, size=%d, type=c, bootable\n' "$((part_mib * 2048))" | sfdisk -q "$out" >/dev/null
dd if="$uboot" of="$out" bs=1024 seek=8 conv=notrunc status=none

truncate -s "${part_mib}M" "$work/part.img"
mkfs.vfat -n MINIKERN "$work/part.img" >/dev/null
mcopy -i "$work/part.img" "$kernel" ::minikern.uImage
mcopy -i "$work/part.img" "$work/boot.scr" ::boot.scr
[ -n "$rootfs" ] && mcopy -i "$work/part.img" "$work/rootfs.uImage" ::rootfs.uImage
dd if="$work/part.img" of="$out" bs=512 seek=2048 conv=notrunc status=none

echo "$out: $(stat -c%s "$out") bytes, U-Boot at 8 KiB, FAT partition at 1 MiB"
