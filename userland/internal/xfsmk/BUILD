load("@io_bazel_rules_go//go:def.bzl", "go_library", "go_test")

# The XFS formatter: pure Go over a Device port, so the same code formats a
# file on the build machine (where xfs_repair judges it) and /dev/nbd0 on
# the board.
go_library(
    name = "xfsmk",
    srcs = [
        "crc.go",
        "errors.go",
        "format.go",
        "interfaces.go",
        "layout.go",
        "models.go",
        "utils.go",
        "write_btrees.go",
        "write_headers.go",
        "write_inodes.go",
        "write_log.go",
    ],
    importpath = "github.com/mateuszmierzwinski/arm_minikern/userland/internal/xfsmk",
    visibility = ["//userland:__subpackages__"],
)

go_test(
    name = "xfsmk_test",
    size = "small",
    srcs = [
        "format_test.go",
        "layout_test.go",
        "utils_test.go",
    ],
    embed = [":xfsmk"],
)
