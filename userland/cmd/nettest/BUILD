load("@io_bazel_rules_go//go:def.bzl", "go_binary")

go_binary(
    name = "nettest",
    srcs = ["main.go"],
    goarch = "arm",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)
