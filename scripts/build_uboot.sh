#!/usr/bin/env bash
# Builds U-Boot for an H3 board, as a per-machine host tool.
#
# It exists because **Fedora 43 ships no armv7 U-Boot**: `uboot-images-armv7`
# is gone from the repositories and only `uboot-images-armv8` remains, which
# is no use for a 32-bit Cortex-A7. Installing one is not an option either —
# packages need root, which an agent does not have (master Rule 27.7).
#
# What it produces is used by one thing: `sdboot_qemu_test`, which boots the
# real chain (BROM → SPL → U-Boot → bootm with a device tree) from an SD-card
# image. Nothing in the kernel depends on it, and no U-Boot code enters this
# repository — the output lands in the monorepo's gitignored `tools/`, the
# same per-machine convention the security scanners use.
#
# Three things have to be turned off for it to build on a modern Fedora, and
# none of them matter to a board:
#   TOOLS_LIBCRYPTO / TOOLS_KWBIMAGE  Fedora 43's openssl-devel no longer has
#                                     <openssl/engine.h>, and U-Boot's host-side
#                                     signing tools include it unconditionally.
#   TOOLS_MKEFICAPSULE                wants gnutls, for EFI capsules.
# And `binman`, which assembles the final file, needs `pkg_resources` —
# removed in setuptools 81 — so the file is assembled here instead: it is the
# SPL padded to 32 KiB (the sector the SPL looks for U-Boot at,
# SYS_MMCSD_RAW_MODE_U_BOOT_SECTOR = 0x40) followed by u-boot.img.
#
# Usage: build_uboot.sh [board-defconfig] [source-directory] [output-directory]
#   board-defconfig   orangepi_pc (the default, what QEMU emulates) or
#                     nanopi_neo (the real board this kernel is for)
#
# The output directory defaults to `tools/uboot/<board>` at the top of
# whatever repository this script is in. It is found with git rather than by
# counting parent directories, because this project is developed inside a
# monorepo and published as a repository of its own, where the same relative
# climb lands in two different places — the mistake that once made an
# exported test script compile nothing and exit 0 (CLAUDE.md §10).
set -euo pipefail

board="${1:-orangepi_pc}"
src="${2:-$HOME/ubootsrc}"
version="2025.07"
here="$(cd "$(dirname "$0")" && pwd)"
root="$(git -C "$here" rev-parse --show-toplevel 2>/dev/null || echo "$here/..")"
out="${3:-$root/tools/uboot}/$board"

command -v arm-linux-gnu-gcc >/dev/null || { echo "arm-linux-gnu-gcc is missing" >&2; exit 1; }
for t in bison flex bc; do
    command -v "$t" >/dev/null || { echo "$t is missing" >&2; exit 1; }
done

# swig is needed by binman's pylibfdt and is not installable without root, so
# it is extracted from its package into tools/uboot/ when absent.
swigdir="$(dirname "$out")"
if ! command -v swig >/dev/null && [ ! -x "$swigdir/usr/bin/swig" ]; then
    echo "==> extracting swig (no root needed)"
    (cd "$swigdir" && dnf download swig >/dev/null && rpm2cpio swig*.rpm | cpio -idm --quiet)
fi
if [ -x "$swigdir/usr/bin/swig" ]; then
    export PATH="$swigdir/usr/bin:$PATH"
    export SWIG_LIB="$(echo "$swigdir"/usr/share/swig/*)"
fi

mkdir -p "$src" "$out"
cd "$src"
tarball="u-boot-$version.tar.bz2"
[ -f "$tarball" ] || { echo "==> downloading $tarball"; curl -sSfO "https://ftp.denx.de/pub/u-boot/$tarball"; }
[ -d "u-boot-$version" ] || tar xf "$tarball"
cd "u-boot-$version"

echo "==> configuring ${board}_defconfig"
make CROSS_COMPILE=arm-linux-gnu- "${board}_defconfig" >/dev/null
./scripts/config --disable TOOLS_LIBCRYPTO --disable TOOLS_KWBIMAGE --disable TOOLS_MKEFICAPSULE
make CROSS_COMPILE=arm-linux-gnu- olddefconfig >/dev/null

echo "==> building"
# binman fails on modern setuptools; everything before it is what is needed.
make CROSS_COMPILE=arm-linux-gnu- -j"$(nproc)" >/dev/null 2>&1 || true
[ -f spl/sunxi-spl.bin ] && [ -f u-boot.img ] ||
    { echo "build produced no SPL or u-boot.img — see the build output" >&2; exit 1; }

# The layout binman would have written: SPL padded to SPL's load sector, then
# u-boot.img. 0x8000 is SYS_MMCSD_RAW_MODE_U_BOOT_SECTOR * 512.
(dd if=spl/sunxi-spl.bin bs=32768 conv=sync status=none; cat u-boot.img) > "$out/u-boot-sunxi-with-spl.bin"
echo "==> $out/u-boot-sunxi-with-spl.bin ($(stat -c%s "$out/u-boot-sunxi-with-spl.bin") bytes)"
