load("@rules_cc//cc:defs.bzl", "cc_toolchain")
load(":cc_toolchain_config.bzl", "armv7a_toolchain_config")

package(default_visibility = ["//:__subpackages__"])

exports_files(["paths.bzl"])

filegroup(name = "empty")

armv7a_toolchain_config(name = "armv7a_config")

cc_toolchain(
    name = "armv7a_cc",
    all_files = ":empty",
    compiler_files = ":empty",
    dwp_files = ":empty",
    linker_files = ":empty",
    objcopy_files = ":empty",
    strip_files = ":empty",
    supports_param_files = 0,
    toolchain_config = ":armv7a_config",
)

# Selected only for the H3 platform, which no host build ever asks for: the
# image rule (//:minikern.bzl) transitions into it.
toolchain(
    name = "armv7a_toolchain",
    target_compatible_with = ["//platforms:allwinner_h3"],
    toolchain = ":armv7a_cc",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
