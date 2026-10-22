load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load("@rules_shell//shell:sh_binary.bzl", "sh_binary")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("//:version.bzl", "MINIKERN_VERSION")
load("@rules_pkg//pkg:tar.bzl", "pkg_tar")
load(":minikern.bzl", "build_stamp", "device_tree", "h3_init", "kernel_image", "minikern_rootfs", "stamp_flag")
load("//toolchain:paths.bzl", "UBOOT_SUNXI_SPL")

# minikern — a from-scratch kernel for the NanoPi NEO (Allwinner H3).
#
#   bazel build //:minikern      # .elf .bin .uImage
#   bazel run   //:qemu          # boot it in QEMU
#   bazel test  //:all           # host unit tests
#   bazel test  //:boot_qemu_test   # boot test (manual)
#
# Portable code (src/lib, src/core) builds for both the host (tests) and the
# H3. Arch and board code builds only for the H3, so a host wildcard build
# skips it. See CLAUDE.md §5.

package(default_visibility = ["//:__subpackages__"])

exports_files(["userland/rootfs/etc/motd"])

_H3 = ["//platforms:allwinner_h3"]

# ── portable: host-tested, linked into the kernel unchanged ─────────────────

cc_library(
    name = "kfmt",
    srcs = ["src/lib/kfmt.c"],
    hdrs = ["src/lib/kfmt.h"],
    strip_include_prefix = "src",
)

# The frequency policy: pure, so it is host-tested, and separate from the
# board adapters it decides for (which QEMU cannot judge at all).
cc_library(
    name = "caldate",
    srcs = ["src/core/caldate.c"],
    hdrs = ["src/core/caldate.h"],
    strip_include_prefix = "src",
)

cc_library(
    name = "gpio",
    srcs = ["src/core/gpio.c"],
    hdrs = ["src/core/gpio.h"],
    strip_include_prefix = "src",
)

cc_library(
    name = "cpufreq",
    srcs = ["src/core/cpufreq.c"],
    hdrs = ["src/core/cpufreq.h"],
    strip_include_prefix = "src",
)

cc_library(
    name = "crc32c",
    srcs = ["src/lib/crc32c.c"],
    hdrs = ["src/lib/crc32c.h"],
    strip_include_prefix = "src",
)

cc_library(
    name = "ed25519",
    srcs = ["src/lib/ed25519.c"],
    hdrs = ["src/lib/ed25519.h"],
    strip_include_prefix = "src",
    deps = [
        ":fe25519",
        ":sha512",
    ],
)

cc_library(
    name = "fe25519",
    srcs = ["src/lib/fe25519.c"],
    hdrs = ["src/lib/fe25519.h"],
    strip_include_prefix = "src",
)

cc_library(
    name = "sha512",
    srcs = ["src/lib/sha512.c"],
    hdrs = ["src/lib/sha512.h"],
    strip_include_prefix = "src",
)

cc_library(
    name = "sha256",
    srcs = ["src/lib/sha256.c"],
    hdrs = ["src/lib/sha256.h"],
    strip_include_prefix = "src",
)

cc_library(
    name = "div64",
    srcs = ["src/lib/div64.c"],
    hdrs = ["src/lib/div64.h"],
    strip_include_prefix = "src",
)

cc_library(
    name = "net",
    srcs = glob(["src/core/net/*.c"]),
    hdrs = [
        "src/core/net/net.h",
        "src/core/net/net_internal.h",  # shared by the parts; net.h, netboot.h and tcp.h are the public ones
        "src/core/net/netboot.h",
        "src/core/net/tcp.h",
        "src/core/net/tcp_internal.h",
    ],
    strip_include_prefix = "src",
    deps = [
        ":core",
        ":ed25519",
        ":kfmt",
        ":sha256",
    ],
)

# The storage stack below any filesystem: the block-device port, a buffer
# cache over it, and the NBD client that reaches a volume on the network.
# Pure, so the host tests drive it against a RAM device and a fake server,
# and the live test drives it against qemu-nbd (CLAUDE.md §11).
# The XFS reader: pure, so the host tests drive it against images made by
# Linux's own mkfs.xfs and judged by xfs_repair (CLAUDE.md §13).
cc_library(
    name = "xfs",
    srcs = glob(["src/core/xfs/*.c"]),
    hdrs = [
        "src/core/xfs/xfs.h",
        "src/core/xfs/xfs_format.h",
        "src/core/xfs/xfs_internal.h",
    ],
    strip_include_prefix = "src",
    deps = [
        ":block",
        ":core",
        ":crc32c",
    ],
)

cc_library(
    name = "block",
    srcs = [
        "src/core/bcache.c",
        "src/core/nbd.c",
    ],
    hdrs = [
        "src/core/bcache.h",
        "src/core/blockdev.h",
        "src/core/nbd.h",
    ],
    strip_include_prefix = "src",
    deps = [":core"],
)

cc_library(
    name = "core",
    srcs = [
        "src/core/bootinfo.c",
        "src/core/cmdline.c",
        "src/core/console.c",
        "src/core/elf.c",
        "src/core/fdt.c",
        "src/core/kheap.c",
        "src/core/pmm.c",
        "src/core/tar.c",
        "src/core/ustack.c",
        "src/core/vma.c",
    ],
    hdrs = [
        "src/core/bootinfo.h",
        "src/core/cmdline.h",
        "src/core/console.h",
        "src/core/elf.h",
        "src/core/errno.h",
        "src/core/fdt.h",
        "src/core/kheap.h",
        "src/core/pmm.h",
        "src/core/ports.h",
        "src/core/tar.h",
        "src/core/ustack.h",
        "src/core/vma.h",
    ],
    strip_include_prefix = "src",
    deps = [":kfmt"],
)

# The user translation-table encoding: ARM-specific, but pure, so host-tested.
cc_library(
    name = "pgtable",
    srcs = ["src/arch/arm/pgtable.c"],
    hdrs = ["src/arch/arm/pgtable.h"],
    strip_include_prefix = "src",
    deps = [":core"],
)

# ── H3 only ─────────────────────────────────────────────────────────────────

cc_library(
    name = "kmem",
    srcs = [
        "src/lib/aeabi_div.S",
        "src/lib/kmem.c",
    ],
    target_compatible_with = _H3,
    deps = [":div64"],
    alwayslink = True,
)

cc_library(
    name = "arch_arm",
    srcs = [
        "src/arch/arm/cpu.c",
        "src/arch/arm/gic.c",
        "src/arch/arm/mmu.c",
        "src/arch/arm/psci.c",
        "src/arch/arm/smp.c",
        "src/arch/arm/start.S",
        "src/arch/arm/timer.c",
        "src/arch/arm/trap.c",
        "src/arch/arm/vectors.S",
    ],
    strip_include_prefix = "src",
    target_compatible_with = _H3,
    deps = [
        ":arch_hdrs",
        ":core",
        ":kernel_hdrs",
        ":pgtable",
    ],
    alwayslink = True,  # nothing references _start: it is the ELF entry
)

# The arch interfaces, apart from their implementations, so the kernel
# layer and the arch layer can include each other's headers.
cc_library(
    name = "arch_hdrs",
    hdrs = [
        "src/arch/arm/cpu.h",
        "src/arch/arm/gic.h",
        "src/arch/arm/layout.h",
        "src/arch/arm/mmu.h",
        "src/arch/arm/psci.h",
        "src/arch/arm/smp.h",
        "src/arch/arm/spinlock.h",
        "src/arch/arm/timer.h",
        "src/arch/arm/trap.h",
    ],
    strip_include_prefix = "src",
    target_compatible_with = _H3,
    deps = [
        ":core",
        ":pgtable",
    ],
)

# Board-independent kernel services; they use the arch interfaces above.
cc_library(
    name = "kernel_hdrs",
    hdrs = glob(["src/kernel/*.h"]),
    strip_include_prefix = "src",
    target_compatible_with = _H3,
    deps = [
        ":arch_hdrs",
        ":block",
        ":core",
        ":net",
        ":pgtable",
        ":xfs",
    ],
)

cc_library(
    name = "kernel",
    srcs = glob(["src/kernel/*.c"]),
    strip_include_prefix = "src",
    target_compatible_with = _H3,
    deps = [
        ":arch_arm",
        ":block",
        ":core",
        ":cpufreq",
        ":gpio",
        ":kernel_hdrs",
        ":kfmt",
        ":xfs",
    ],
    alwayslink = True,
)

# The build number, from Bazel's stamp. Off by default, so an ordinary
# build reports 0 rather than a number that means nothing (minikern.bzl).
stamp_flag(
    name = "stamp",
    build_setting_default = False,
)

build_stamp(name = "build_stamp")

cc_library(
    name = "board_h3",
    srcs = [
        ":build_stamp",
        "src/board/h3/cpux.c",
        "src/board/h3/emac.c",
        "src/board/h3/main.c",
        "src/board/h3/pio.c",
        "src/board/h3/rtc.c",
        "src/board/h3/sid.c",
        "src/board/h3/ths.c",
        "src/board/h3/wdt.c",
        "src/board/h3/uart.c",
    ],
    hdrs = [
        "src/board/h3/cpux.h",
        "src/board/h3/emac.h",
        "src/board/h3/h3.h",
        "src/board/h3/pio.h",
        "src/board/h3/rtc.h",
        "src/board/h3/sid.h",
        "src/board/h3/ths.h",
        "src/board/h3/wdt.h",
        "src/board/h3/uart.h",
        "src/board/h3/version.h",
    ],
    local_defines = ['MINIKERN_VERSION=\\"%s\\"' % MINIKERN_VERSION],
    strip_include_prefix = "src",
    target_compatible_with = _H3,
    deps = [
        ":arch_arm",
        ":caldate",
        ":core",
        ":cpufreq",
        ":gpio",
        ":kernel",
        ":kernel_hdrs",
    ],
    alwayslink = True,
)

cc_binary(
    name = "kernel_elf",
    additional_linker_inputs = ["src/arch/arm/link.ld"],
    linkopts = [
        "-T",
        "$(location src/arch/arm/link.ld)",
    ],
    target_compatible_with = _H3,
    deps = [
        ":board_h3",
        ":kmem",
    ],
)

# Needs the host cross compiler and mkimage (CLAUDE.md §4), so it is kept out
# of wildcard builds, like every target here that shells out to host tools.
kernel_image(
    name = "minikern",
    kernel = ":kernel_elf",
    tags = ["manual"],
)

# The NEO's device tree, so QEMU boots through the path real U-Boot takes.
device_tree(
    name = "nanopi_neo_dtb",
    src = "dts/nanopi-neo.dts",
    tags = ["manual"],
)

sh_binary(
    name = "qemu",
    srcs = ["scripts/qemu.sh"],
    args = ["$(rootpath :minikern.uImage)"],
    data = [":minikern.uImage"],
    tags = ["manual"],
)

# ── tests ───────────────────────────────────────────────────────────────────

cc_library(
    name = "ktest",
    testonly = True,
    hdrs = [
        "test/fdt_build.h",
        "test/ktest.h",
        "test/net_frames.h",
    ],
    deps = [":net"],
    strip_include_prefix = "test",
)

cc_test(
    name = "kfmt_test",
    size = "small",
    srcs = ["test/kfmt_test.c"],
    deps = [
        ":kfmt",
        ":ktest",
    ],
)

cc_test(
    name = "cmdline_test",
    size = "small",
    srcs = ["test/cmdline_test.c"],
    deps = [
        ":core",
        ":ktest",
    ],
)

cc_test(
    name = "bootinfo_test",
    size = "small",
    srcs = ["test/bootinfo_test.c"],
    deps = [
        ":core",
        ":ktest",
    ],
)

cc_test(
    name = "ed25519_test",
    size = "small",
    srcs = [
        "test/ed25519_test.c",
        "test/ed_vectors.h",
    ],
    deps = [
        ":ed25519",
        ":ktest",
    ],
)

cc_test(
    name = "fe25519_test",
    size = "small",
    srcs = [
        "test/fe25519_test.c",
        "test/fe_vectors.h",
    ],
    deps = [
        ":fe25519",
        ":ktest",
    ],
)

cc_test(
    name = "sha512_test",
    size = "small",
    srcs = ["test/sha512_test.c"],
    deps = [
        ":ktest",
        ":sha512",
    ],
)

cc_test(
    name = "caldate_test",
    size = "small",
    srcs = ["test/caldate_test.c"],
    deps = [
        ":caldate",
        ":ktest",
    ],
)

cc_test(
    name = "gpio_test",
    size = "small",
    srcs = ["test/gpio_test.c"],
    deps = [
        ":gpio",
        ":ktest",
    ],
)

cc_test(
    name = "cpufreq_test",
    size = "small",
    srcs = ["test/cpufreq_test.c"],
    deps = [
        ":cpufreq",
        ":ktest",
    ],
)

cc_test(
    name = "crc32c_test",
    size = "small",
    srcs = ["test/crc32c_test.c"],
    deps = [
        ":crc32c",
        ":ktest",
    ],
)

cc_test(
    name = "sha256_test",
    size = "small",
    srcs = ["test/sha256_test.c"],
    deps = [
        ":ktest",
        ":sha256",
    ],
)

cc_test(
    name = "div64_test",
    size = "small",
    srcs = ["test/div64_test.c"],
    deps = [
        ":div64",
        ":ktest",
    ],
)

cc_test(
    name = "fdt_test",
    size = "small",
    srcs = ["test/fdt_test.c"],
    deps = [
        ":core",
        ":ktest",
    ],
)

[cc_test(
    name = t,
    size = "small",
    srcs = ["test/%s.c" % t],
    deps = [
        ":core",
        ":ktest",
    ],
) for t in [
    "elf_test",
    "kheap_test",
    "pmm_test",
    "ustack_test",
    "vma_test",
]]

cc_test(
    name = "bcache_test",
    size = "small",
    srcs = ["test/bcache_test.c"],
    deps = [
        ":block",
        ":ktest",
    ],
)

cc_test(
    name = "nbd_test",
    size = "small",
    srcs = ["test/nbd_test.c"],
    deps = [
        ":block",
        ":ktest",
    ],
)

cc_test(
    name = "net_test",
    size = "small",
    srcs = ["test/net_test.c"],
    deps = [
        ":ktest",
        ":net",
    ],
)

# The same test under clang's ASan and UBSan: the stack's input is hostile by
# definition. Needs host clang + compiler-rt, hence manual.
sh_test(
    name = "net_sanitized_test",
    size = "small",
    srcs = ["test/net_sanitized_test.sh"],
    args = ["$(rootpaths :net_sanitized_srcs)"],
    data = [":net_sanitized_srcs"],
    tags = ["manual"],
)

filegroup(
    name = "net_sanitized_srcs",
    srcs = glob([
        "src/core/net/*.c",
        "src/core/net/*.h",
    ]) + [
        "src/core/errno.h",
        "src/core/ports.h",
        "src/lib/ed25519.c",
        "src/lib/ed25519.h",
        "src/lib/fe25519.c",
        "src/lib/fe25519.h",
        "src/lib/kfmt.c",
        "src/lib/kfmt.h",
        "src/lib/sha256.c",
        "src/lib/sha256.h",
        "src/lib/sha512.c",
        "src/lib/sha512.h",
        "test/ktest.h",
        "test/net_frames.h",
        "test/net_test.c",
        "test/netboot_test.c",
        "test/offer_vector.h",
        "test/tcp_hostile_test.c",
        "test/tcp_test.c",
    ],
)

# The kernel's NBD client as a host binary, so it can be driven against
# qemu-nbd — a server nobody here wrote (test/net_nbd_live_test.sh).
cc_binary(
    name = "nbd_probe",
    testonly = True,
    srcs = ["test/nbd_probe.c"],
    tags = ["manual"],
    deps = [":block"],
)

sh_test(
    name = "net_nbd_live_test",
    size = "small",
    srcs = ["test/net_nbd_live_test.sh"],
    args = ["$(rootpath :nbd_probe)"],
    data = [":nbd_probe"],
    tags = ["manual"],
)

# Our NBD server, driven by reference clients (nbdinfo, nbdcopy, qemu-io)
# and then by our own: the other half of net_nbd_live_test.
sh_test(
    name = "nbd_server_live_test",
    size = "small",
    srcs = ["test/nbd_server_live_test.sh"],
    args = [
        "$(rootpath //server/cmd/nbdserver)",
        "$(rootpath :nbd_probe)",
    ],
    data = [
        ":nbd_probe",
        "//server/cmd/nbdserver",
    ],
    tags = ["manual"],
)

# The XFS reader against images made by Linux's mkfs.xfs, judged by
# xfs_repair: the only standard worth holding a filesystem reader to.
cc_binary(
    name = "xfs_probe",
    testonly = True,
    srcs = ["test/xfs_probe.c"],
    tags = ["manual"],
    deps = [":xfs"],
)

sh_test(
    name = "xfs_live_test",
    size = "medium",
    srcs = ["test/xfs_live_test.sh"],
    args = ["$(rootpath :xfs_probe)"],
    data = [":xfs_probe"],
    tags = ["manual"],
)

cc_test(
    name = "xfs_test",
    size = "small",
    srcs = ["test/xfs_test.c"],
    deps = [
        ":crc32c",
        ":ktest",
        ":xfs",
    ],
)

# The XFS reader under ASan and UBSan: it parses somebody's filesystem, so
# an out-of-bounds read that happens not to crash must still fail a test.
sh_test(
    name = "xfs_sanitized_test",
    size = "small",
    srcs = ["test/net_sanitized_test.sh"],
    args = ["$(rootpaths :xfs_sanitized_srcs)"],
    data = [":xfs_sanitized_srcs"],
    tags = ["manual"],
)

filegroup(
    name = "xfs_sanitized_srcs",
    srcs = glob(["src/core/xfs/*.c"]) + [
        "src/core/bcache.c",
        "src/core/console.c",
        "src/core/bcache.h",
        "src/core/blockdev.h",
        "src/core/console.h",
        "src/core/errno.h",
        "src/core/ports.h",
        "src/core/xfs/xfs.h",
        "src/core/xfs/xfs_format.h",
        "src/core/xfs/xfs_internal.h",
        "src/lib/crc32c.c",
        "src/lib/crc32c.h",
        "src/lib/kfmt.c",
        "src/lib/kfmt.h",
        "test/ktest.h",
        "test/xfs_test.c",
    ],
)

# P9.3: the Go formatter, judged by xfs_repair and then read by the kernel's
# own reader — our formatter, Linux's judgement, our reader.
sh_test(
    name = "mkfsxfs_live_test",
    size = "medium",
    srcs = ["test/mkfsxfs_live_test.sh"],
    args = [
        "$(rootpath //userland/cmd/mkfsxfs:mkfsxfs_host)",
        "$(rootpath :xfs_probe)",
    ],
    data = [
        ":xfs_probe",
        "//userland/cmd/mkfsxfs:mkfsxfs_host",
    ],
    tags = ["manual"],
)

cc_test(
    name = "netboot_test",
    size = "small",
    srcs = [
        "test/netboot_test.c",
        "test/offer_vector.h",
    ],
    deps = [
        ":ktest",
        ":net",
        ":sha256",
    ],
)

cc_test(
    name = "tcp_hostile_test",
    size = "small",
    srcs = ["test/tcp_hostile_test.c"],
    deps = [
        ":ktest",
        ":net",
    ],
)

cc_test(
    name = "tcp_test",
    size = "small",
    srcs = ["test/tcp_test.c"],
    deps = [
        ":ktest",
        ":net",
    ],
)

cc_test(
    name = "pgtable_test",
    size = "small",
    srcs = ["test/pgtable_test.c"],
    deps = [
        ":ktest",
        ":pgtable",
    ],
)

cc_test(
    name = "tar_test",
    size = "small",
    srcs = ["test/tar_test.c"],
    args = [
        "$(rootpath test/testdata/ustar.tar)",
        "$(rootpath test/testdata/pax.tar)",
    ],
    data = [
        "test/testdata/pax.tar",
        "test/testdata/ustar.tar",
    ],
    deps = [
        ":core",
        ":ktest",
    ],
)

# A two-syscall user program: the first thing to run in user mode.
cc_binary(
    name = "hello_user",
    srcs = ["test/user/hello.S"],
    tags = ["manual"],
    target_compatible_with = _H3,
)

h3_init(
    name = "hello_init",
    src = ":hello_user",
    tags = ["manual"],
)

pkg_tar(
    name = "hello_rootfs",
    srcs = [":hello_init"],
    mode = "0755",
    tags = ["manual"],
)

# Root filesystems with a Go /init: the ABI suite (gc), and the demo built by
# each compiler. Each also carries userland/rootfs (/etc/motd).
minikern_rootfs(
    name = "goabi_rootfs",
    init = "//userland/cmd/goabi",
)

minikern_rootfs(
    name = "hello_gc_rootfs",
    init = "//userland/cmd/hello",
)

minikern_rootfs(
    name = "netboot_rootfs",
    init = "//userland/cmd/netbootinit",
)

# The board that serves a netboot image carries that image in its own root
# filesystem: there is nothing else on the segment to get it from.
minikern_rootfs(
    name = "netbootsrv_rootfs",
    extra = {":hello_gc_rootfs": "srv/default.tar"},
    init = "//server/cmd/mknbserver_board",
)

minikern_rootfs(
    name = "oomtest_rootfs",
    init = "//userland/cmd/oomtest",
)

minikern_rootfs(
    name = "freqtest_rootfs",
    init = "//userland/cmd/freqtest",
)

minikern_rootfs(
    name = "exectest_rootfs",
    init = "//userland/cmd/exectest",
)

minikern_rootfs(
    name = "smptest_rootfs",
    init = "//userland/cmd/smptest",
)

minikern_rootfs(
    name = "xfstest_rootfs",
    init = "//userland/cmd/xfstest",
)

minikern_rootfs(
    name = "nbdtest_rootfs",
    init = "//userland/cmd/nbdtest",
)

minikern_rootfs(
    name = "nettest_rootfs",
    init = "//userland/cmd/nettest",
)

minikern_rootfs(
    name = "hostile_rootfs",
    init = "//userland/cmd/hostile",
)

minikern_rootfs(
    name = "hello_tinygo_rootfs",
    init = "//userland/cmd/hello:hello_tinygo",
)

# Userland under QEMU: each boots the kernel with one root filesystem and
# checks what /init printed and how it ended.
sh_test(
    name = "user_asm_qemu_test",
    size = "small",
    srcs = ["test/userland_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :hello_rootfs)",
        "'hello from user space'",
        "'init exited with status 42'",
    ],
    data = [
        ":hello_rootfs",
        ":minikern.uImage",
    ],
    tags = ["manual"],
)

sh_test(
    name = "tinygo_qemu_test",
    size = "small",
    srcs = ["test/userland_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :hello_tinygo_rootfs)",
        "'hello from tinygo Go on minikern, pid 1'",
        "'this line went to stderr'",
        "'slept true'",
        "'map entries: 1000'",
        "'init exited with status 7'",
    ],
    data = [
        ":hello_tinygo_rootfs",
        ":minikern.uImage",
    ],
    tags = ["manual"],
)

sh_test(
    name = "go_hello_qemu_test",
    size = "small",
    srcs = ["test/userland_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :hello_gc_rootfs)",
        "'hello from gc Go on minikern, pid 1'",
        "'this line went to stderr'",
        "'slept true'",
        "'map entries: 1000'",
        "'init exited with status 7'",
    ],
    data = [
        ":hello_gc_rootfs",
        ":minikern.uImage",
    ],
    tags = ["manual"],
)

# P8.1: fork and exec, through Go's os/exec — a child runs, its output and
# its exit status come back, a pipe carries bytes into it, and the
# environment and arguments reach the new image.
sh_test(
    name = "exec_qemu_test",
    size = "medium",
    srcs = ["test/userland_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :exectest_rootfs)",
        "'ok   a pipe reports end of file when its writer closes'",
        "'ok   exec runs a child and its output comes back'",
        "'exit status reaches the parent'",
        "'ok   a pipe carries bytes to the child'",
        "'ok   the environment reaches the new image'",
        "'ok   every argument reaches the new image'",
        "'ok   a process with threads can replace its own image'",
        "'ok   execing something that is not there fails cleanly'",
        "'exectest ok'",
        "'init exited with status 7'",
    ],
    data = [
        ":exectest_rootfs",
        ":minikern.uImage",
    ],
    tags = ["manual"],
)

# P0.9: the real boot chain from an SD-card image — the one a board uses, and
# the one `-kernel` skips. Needs a per-machine U-Boot (scripts/build_uboot.sh),
# whose path comes from toolchain/paths.bzl.
sh_test(
    name = "sdboot_qemu_test",
    size = "large",
    srcs = ["test/sdboot_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :goabi_rootfs)",
        "$(rootpath scripts/mksdcard.sh)",
    ],
    data = [
        ":goabi_rootfs",
        ":minikern.uImage",
        "scripts/mksdcard.sh",
    ],
    env = {"MINIKERN_UBOOT": UBOOT_SUNXI_SPL},
    tags = ["manual"],
)

# P11.4: an idle core powers itself off, and comes back when work appears —
# which on this chip saves more than stepping the clock does.
sh_test(
    name = "cpuoff_qemu_test",
    size = "large",
    srcs = ["test/cpuoff_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :goabi_rootfs)",
    ],
    data = [
        ":goabi_rootfs",
        ":minikern.uImage",
    ],
    tags = ["manual"],
)

# Two boots of one image must not produce the same random bytes: the
# mechanism that makes them differ is harvesting interrupt timing.
sh_test(
    name = "entropy_qemu_test",
    size = "medium",
    srcs = ["test/entropy_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :goabi_rootfs)",
    ],
    data = [
        ":goabi_rootfs",
        ":minikern.uImage",
    ],
    tags = ["manual"],
)

# P5.6: discovery between two guests on a segment of their own, so that
# "it found the server" stops meaning "it found the host".
sh_test(
    name = "netboot_two_guests_test",
    size = "medium",
    srcs = ["test/netboot_two_guests_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :netbootsrv_rootfs)",
    ],
    data = [
        ":minikern.uImage",
        ":netbootsrv_rootfs",
    ],
    tags = ["manual"],
)

# The out-of-memory policy: hand out optimistically, then kill the process
# that cannot be given a page — with SIGKILL, not a segmentation fault.
sh_test(
    name = "oom_qemu_test",
    size = "medium",
    srcs = ["test/oom_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :oomtest_rootfs)",
    ],
    data = [
        ":minikern.uImage",
        ":oomtest_rootfs",
    ],
    tags = ["manual"],
)

# P8.11: the watchdog, in both directions — a wedged kernel is rebooted, and
# a healthy one is left alone. QEMU models this device, so unlike the thermal
# sensor and the PLLs it is tested rather than hoped for.
sh_test(
    name = "watchdog_qemu_test",
    size = "medium",
    srcs = ["test/watchdog_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :hello_gc_rootfs)",
    ],
    data = [
        ":hello_gc_rootfs",
        ":minikern.uImage",
    ],
    tags = ["manual"],
)

# P11: the frequency governor, as far as anything but the board can judge
# it. A fake sensor ramps from cold to past critical while a Go program
# holds every core busy, and every decision the policy can make must appear
# in the log: up under load, down when it is too warm to rise, and straight
# to the bottom when it is critical. The two register writes underneath are
# the part only the NEO can confirm (board/h3/ths.h).
sh_test(
    name = "cpufreq_qemu_test",
    size = "medium",
    srcs = ["test/cpufreq_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :freqtest_rootfs)",
    ],
    data = [
        ":freqtest_rootfs",
        ":minikern.uImage",
    ],
    tags = ["manual"],
)

# P10: four cores, from both sides — Go sees them and uses them, and the
# kernel's own per-core counters show every one of them being used.
sh_test(
    name = "smp_qemu_test",
    size = "medium",
    srcs = ["test/smp_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :smptest_rootfs)",
    ],
    data = [
        ":minikern.uImage",
        ":smptest_rootfs",
    ],
    tags = ["manual"],
)

# The gc runtime's whole Linux surface: threads, futexes, timers through
# epoll, async preemption by signal, the GC, a nil-deref panic, files.
sh_test(
    name = "go_abi_qemu_test",
    size = "medium",
    srcs = ["test/userland_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :goabi_rootfs)",
        "'PASS preemption'",
        "'PASS nil-deref-panic'",
        "'PASS files'",
        "'PASS tmpfs'",
        "'PASS clock-set'",
        "'ALL PASS'",
        "'init exited with status 0'",
    ],
    data = [
        ":goabi_rootfs",
        ":minikern.uImage",
    ],
    tags = ["manual"],
)

# The syscall boundary under attack (CVE classes: kernel-pointer confusion,
# length overflow, address-space escape, resource exhaustion).
sh_test(
    name = "cve_syscall_qemu_test",
    size = "medium",
    srcs = ["test/userland_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :hostile_rootfs)",
        "'ALL HOSTILE CASES REFUSED'",
        "'init exited with status 0'",
    ],
    data = [
        ":hostile_rootfs",
        ":minikern.uImage",
    ],
    tags = ["manual"],
)

# The P1 milestone: EMAC + PHY up, a DHCP lease, the gateway answering
# pings, and a host datagram echoed by the guest's UDP port 7.
sh_test(
    name = "net_qemu_test",
    size = "small",
    srcs = ["test/net_qemu_test.sh"],
    args = ["$(rootpath :minikern.uImage)"],
    data = [":minikern.uImage"],
    tags = ["manual"],
)

# Go's net package on minikern: UDP as client, as server, and over
# loopback, checked from the host as well as from inside the guest.
sh_test(
    name = "net_sockets_qemu_test",
    size = "medium",
    srcs = ["test/net_sockets_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :nettest_rootfs)",
        "$(rootpath //test/tools/netecho)",
        "$(rootpath //test/tools/dnsecho)",
    ],
    data = [
        ":minikern.uImage",
        ":nettest_rootfs",
        "//test/tools/dnsecho",
        "//test/tools/netecho",
    ],
    tags = ["manual"],
)

# The P5 milestone: the board finds the server by sweeping the subnet,
# fetches its root filesystem, verifies the digest and runs a Go init from
# it, with no initrd involved.
sh_test(
    name = "netboot_qemu_test",
    size = "medium",
    srcs = ["test/netboot_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath //server/cmd/mknbserver)",
        "$(rootpath :netboot_rootfs)",
    ],
    data = [
        ":minikern.uImage",
        ":netboot_rootfs",
        "//server/cmd/mknbserver",
    ],
    tags = ["manual"],
)

# P9.1 end to end: a Go program on the board reads and writes a volume that
# is a file on the server, over our TCP and our NBD client, and the host
# checks the file afterwards byte for byte.
sh_test(
    name = "nbd_qemu_test",
    size = "medium",
    srcs = ["test/nbd_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :nbdtest_rootfs)",
        "$(rootpath //server/cmd/nbdserver)",
    ],
    data = [
        ":minikern.uImage",
        ":nbdtest_rootfs",
        "//server/cmd/nbdserver",
    ],
    tags = ["manual"],
)

# P9.3 end to end: the board formats an XFS filesystem over the network,
# mounts it, reads it back — and then Linux's xfs_repair judges the file.
sh_test(
    name = "xfs_qemu_test",
    size = "medium",
    srcs = ["test/xfs_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :xfstest_rootfs)",
        "$(rootpath //server/cmd/nbdserver)",
    ],
    data = [
        ":minikern.uImage",
        ":xfstest_rootfs",
        "//server/cmd/nbdserver",
    ],
    tags = ["manual"],
)

# Boots the real image under qemu-system-arm and checks the boot log. Needs
# QEMU and the cross toolchain on the host, hence manual.
sh_test(
    name = "boot_qemu_test",
    size = "small",
    srcs = ["test/boot_qemu_test.sh"],
    args = [
        "$(rootpath :minikern.uImage)",
        "$(rootpath :nanopi_neo_dtb)",
    ],
    data = [
        ":minikern.uImage",
        ":nanopi_neo_dtb",
    ],
    tags = ["manual"],
)

# Every test that needs a host tool — QEMU, clang's sanitizers, qemu-nbd —
# by name, since manual targets escape wildcards:
#   bazel test //:qemu_tests
test_suite(
    name = "qemu_tests",
    tags = ["manual"],
    tests = [
        ":boot_qemu_test",
        ":nbd_qemu_test",
        ":mkfsxfs_live_test",
        ":xfs_live_test",
        ":xfs_qemu_test",
        ":xfs_sanitized_test",
        ":nbd_server_live_test",
        ":net_nbd_live_test",
        ":cve_syscall_qemu_test",
        ":net_qemu_test",
        ":netboot_qemu_test",
        ":net_sockets_qemu_test",
        ":net_sanitized_test",
        ":go_abi_qemu_test",
        ":go_hello_qemu_test",
        ":cpufreq_qemu_test",
        ":cpuoff_qemu_test",
        ":sdboot_qemu_test",
        ":entropy_qemu_test",
        ":netboot_two_guests_test",
        ":oom_qemu_test",
        ":watchdog_qemu_test",
        ":exec_qemu_test",
        ":smp_qemu_test",
        ":tinygo_qemu_test",
        ":user_asm_qemu_test",
    ],
)
