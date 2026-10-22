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
