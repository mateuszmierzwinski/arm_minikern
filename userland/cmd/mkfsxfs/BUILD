load("@io_bazel_rules_go//go:def.bzl", "go_binary", "go_library")

go_library(
    name = "mkfsxfs_lib",
    srcs = [
        "device.go",
        "main.go",
    ],
    importpath = "github.com/mateuszmierzwinski/arm_minikern/userland/cmd/mkfsxfs",
    visibility = ["//visibility:private"],
    deps = ["//userland/internal/xfsmk"],
)

# For the board: linux/arm, pure static, packed into a root filesystem.
go_binary(
    name = "mkfsxfs",
    embed = [":mkfsxfs_lib"],
    goarch = "arm",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)

# For the build machine, so xfs_repair can judge what it writes without a
# board in the loop (test/mkfsxfs_live_test.sh).
go_binary(
    name = "mkfsxfs_host",
    testonly = True,
    embed = [":mkfsxfs_lib"],
    visibility = ["//:__pkg__"],
)
