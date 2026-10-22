load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# Built for the kernel's ABI: linux/arm, pure Go (static, no cgo). rules_go
# cross-compiles with the monorepo's SDK; GOARM is the toolchain default (7).
go_binary(
    name = "goabi",
    srcs = ["main.go"],
    goarch = "arm",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
