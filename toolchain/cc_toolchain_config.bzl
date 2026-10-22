"""A freestanding ARMv7-A (Cortex-A7) C toolchain over the host's arm-linux-gnu-gcc."""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:cc_toolchain_config_lib.bzl", "feature", "flag_group", "flag_set", "tool_path")
load("@rules_cc//cc:defs.bzl", "CcToolchainConfigInfo", "cc_common")
load(":paths.bzl", "CROSS_GCC_INCLUDE_ROOT", "CROSS_PREFIX")

_TOOLS = ["gcc", "ld", "ar", "cpp", "gcov", "nm", "objdump", "strip", "objcopy"]

_COMPILE_ACTIONS = [
    ACTION_NAMES.assemble,
    ACTION_NAMES.preprocess_assemble,
    ACTION_NAMES.c_compile,
]

_LINK_ACTIONS = [ACTION_NAMES.cpp_link_executable]

# Kernel code: soft-float so no VFP state is touched before the kernel enables
# it; no builtins, no stack protector, no PIC; sections split for --gc-sections.
_COMPILE_FLAGS = [
    "-mcpu=cortex-a7",
    "-marm",
    "-mfloat-abi=soft",
    "-std=c17",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-pic",
    "-fno-common",
    "-fno-stack-protector",
    "-ffunction-sections",
    "-fdata-sections",
    "-O2",
    "-g",
    "-Wall",
    "-Wextra",
    "-Werror",
    # A kernel stack is 8 KiB (KSTACK_PAGES), and the boot stack 16 KiB, so a
    # frame of more than 1 KiB is almost certainly a mistake. It caught a
    # struct tcp_conn (16 KiB of queues) being built on the stack to send a
    # reset, which any peer could have triggered (CLAUDE.md §10).
    "-Wframe-larger-than=1024",
]

_LINK_FLAGS = [
    "-nostdlib",
    "-static",
    "-Wl,--gc-sections",
    "-Wl,--build-id=none",
    "-Wl,-z,noexecstack",
]

def _flags_feature(name, actions, flags):
    return feature(
        name = name,
        enabled = True,
        flag_sets = [flag_set(actions = actions, flag_groups = [flag_group(flags = flags)])],
    )

def _impl(ctx):
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "minikern-armv7a",
        host_system_name = "x86_64-linux",
        target_system_name = "arm-none-eabi",
        target_cpu = "armv7",
        target_libc = "none",
        compiler = "gcc",
        abi_version = "eabi",
        abi_libc_version = "none",
        tool_paths = [tool_path(name = t, path = CROSS_PREFIX + t) for t in _TOOLS],
        cxx_builtin_include_directories = [CROSS_GCC_INCLUDE_ROOT],
        features = [
            _flags_feature("minikern_compile_flags", _COMPILE_ACTIONS, _COMPILE_FLAGS),
            _flags_feature("minikern_link_flags", _LINK_ACTIONS, _LINK_FLAGS),
        ],
    )

armv7a_toolchain_config = rule(
    implementation = _impl,
    attrs = {},
    provides = [CcToolchainConfigInfo],
)
