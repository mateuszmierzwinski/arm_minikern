"""Host tools the kernel build shells out to. One place, read by the toolchain and the image rule."""

# Fedora `gcc-arm-linux-gnu` + `binutils-arm-linux-gnu`: a bare cross compiler,
# no libc — which is exactly what a freestanding kernel wants.
CROSS_PREFIX = "/usr/bin/arm-linux-gnu-"

# Fedora `uboot-tools`: wraps the raw image for U-Boot's (and QEMU's) bootm.
MKIMAGE = "/usr/bin/mkimage"

# Where the cross gcc keeps its own freestanding headers (stdint.h, stddef.h, stdarg.h).
CROSS_GCC_INCLUDE_ROOT = "/usr/lib/gcc/arm-linux-gnueabi"

# Fedora `dtc`: compiles dts/*.dts for QEMU's -dtb (the U-Boot boot path).
DTC = "/usr/bin/dtc"

# TinyGo 0.39 (Fedora `tinygo`) supports Go 1.19-1.25 only, and the monorepo's
# own SDK is newer, so it is pointed at a host Go of that range explicitly.
TINYGO = "/usr/bin/tinygo"
TINYGO_GOROOT = "/home/mmierzwinski/go/go_current"

# U-Boot for the H3, used **only** to test the real boot chain (BROM → SPL →
# U-Boot → bootm) from an SD-card image, which the `-kernel` path never
# exercises. It is a per-machine host tool like the ones above, kept in the
# monorepo's gitignored `tools/` directory (the same convention Rule 29 uses
# for the security scanners), because **Fedora 43 no longer ships an armv7
# U-Boot at all** — `uboot-images-armv7` is gone, and only the armv8 package
# remains. `scripts/build_uboot.sh` builds it; the SD-card targets are
# `manual`, so a machine without it still builds and tests everything else.
UBOOT_SUNXI_SPL = "/home/mmierzwinski/ExtVolume/Dropbox/Monorepo/tools/uboot/orangepi_pc/u-boot-sunxi-with-spl.bin"
