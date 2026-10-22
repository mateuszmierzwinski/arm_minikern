"""kernel_image: build a cc_binary for the H3 and wrap it as ELF, raw binary and U-Boot uImage."""

load("@rules_pkg//pkg:tar.bzl", "pkg_tar")
load("//toolchain:paths.bzl", "CROSS_PREFIX", "DTC", "MKIMAGE", "TINYGO", "TINYGO_GOROOT")

_PLATFORM = "//platforms:nanopi_neo"
_TOOLCHAIN = "//toolchain:armv7a_toolchain"

def _to_h3_impl(_settings, _attr):
    return {
        "//command_line_option:platforms": str(Label(_PLATFORM)),
        "//command_line_option:extra_toolchains": [str(Label(_TOOLCHAIN))],
    }

_to_h3 = transition(
    implementation = _to_h3_impl,
    inputs = [],
    outputs = [
        "//command_line_option:platforms",
        "//command_line_option:extra_toolchains",
    ],
)

# The load and entry address come from the ELF itself, so the linker script is
# the only place that names it. _start is the first byte of the image.
_WRAP = """
set -eu
{prefix}objcopy -O binary "$1" "$2"
entry=$({prefix}readelf -h "$1" | awk '/Entry point address/ {{print $4}}')
{mkimage} -A arm -O linux -T kernel -C none -a "$entry" -e "$entry" -n minikern -d "$2" "$3" >/dev/null
"""

def _kernel_image_impl(ctx):
    elf = ctx.outputs.elf
    ctx.actions.symlink(output = elf, target_file = ctx.file.kernel)
    ctx.actions.run_shell(
        inputs = [ctx.file.kernel],
        outputs = [ctx.outputs.bin, ctx.outputs.uimage],
        command = _WRAP.format(prefix = CROSS_PREFIX, mkimage = MKIMAGE),
        arguments = [ctx.file.kernel.path, ctx.outputs.bin.path, ctx.outputs.uimage.path],
        mnemonic = "MinikernImage",
        progress_message = "Wrapping kernel image %{label}",
    )
    files = depset([elf, ctx.outputs.bin, ctx.outputs.uimage])
    return [DefaultInfo(files = files)]

kernel_image = rule(
    implementation = _kernel_image_impl,
    doc = "The kernel for the H3: <name>.elf (QEMU -kernel, gdb), <name>.bin (raw), <name>.uImage (bootm).",
    attrs = {
        "kernel": attr.label(
            mandatory = True,
            allow_single_file = True,
            cfg = _to_h3,
            doc = "A cc_binary linked with the kernel's linker script.",
        ),
    },
    outputs = {
        "elf": "%{name}.elf",
        "bin": "%{name}.bin",
        "uimage": "%{name}.uImage",
    },
)

def _device_tree_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".dtb")
    ctx.actions.run_shell(
        inputs = [ctx.file.src],
        outputs = [out],
        command = '{dtc} -q -I dts -O dtb -o "$2" "$1"'.format(dtc = DTC),
        arguments = [ctx.file.src.path, out.path],
        mnemonic = "Dtc",
        progress_message = "Compiling device tree %{label}",
    )
    return [DefaultInfo(files = depset([out]))]

device_tree = rule(
    implementation = _device_tree_impl,
    doc = "Compiles one .dts into <name>.dtb with the host's dtc.",
    attrs = {"src": attr.label(mandatory = True, allow_single_file = [".dts"])},
)

def _h3_init_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + "/init")
    ctx.actions.symlink(output = out, target_file = ctx.file.src)
    return [DefaultInfo(files = depset([out]))]

h3_init = rule(
    implementation = _h3_init_impl,
    doc = "Builds src for the H3 and presents it as <name>/init, ready for a pkg_tar root filesystem.",
    attrs = {
        "src": attr.label(mandatory = True, allow_single_file = True, cfg = _to_h3),
    },
)

# TinyGo with throwaway caches inside the action, so the build is sandboxed
# (a cold build of musl and compiler-rt takes ~3 s). GOWORK=off keeps the
# monorepo's go.work out of it; GOTOOLCHAIN=local pins the host Go TinyGo needs.
_TINYGO = """
set -eu
out="$PWD/$2"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cp "$1" "$tmp/main.go"
cd "$tmp"
env -i PATH="{goroot}/bin:/usr/bin:/bin" HOME="$tmp" XDG_CACHE_HOME="$tmp/cache" GOCACHE="$tmp/gocache" \
    GOPATH="$tmp/gopath" GOWORK=off GOTOOLCHAIN=local GOOS=linux GOARCH=arm \
    {tinygo} build -o "$out" main.go
"""

def _tinygo_binary_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.run_shell(
        inputs = [ctx.file.src],
        outputs = [out],
        command = _TINYGO.format(goroot = TINYGO_GOROOT, tinygo = TINYGO),
        arguments = [ctx.file.src.path, out.path],
        mnemonic = "TinyGo",
        progress_message = "TinyGo %{label}",
    )
    return [DefaultInfo(files = depset([out]), executable = out)]

tinygo_binary = rule(
    implementation = _tinygo_binary_impl,
    doc = "A linux/arm static binary from one Go file, built by the host's TinyGo.",
    attrs = {"src": attr.label(mandatory = True, allow_single_file = [".go"])},
)

def minikern_rootfs(name, init, **kwargs):
    """A ustar root filesystem: init as /init (mode 0755) plus userland/rootfs.

    Tagged manual: init may come from a host tool (TinyGo).
    """
    pkg_tar(
        name = name,
        empty_dirs = ["etc"],
        files = {
            init: "init",
            "//:userland/rootfs/etc/motd": "etc/motd",
        },
        modes = {"init": "0755"},
        tags = ["manual"],
        **kwargs
    )
