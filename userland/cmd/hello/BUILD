load("@io_bazel_rules_go//go:def.bzl", "go_binary")
load("//:minikern.bzl", "tinygo_binary")

exports_files(["main.go"])

go_binary(
    name = "hello",
    srcs = ["main.go"],
    goarch = "arm",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)

# The same program through TinyGo (host tool, so manual).
tinygo_binary(
    name = "hello_tinygo",
    src = "main.go",
    tags = ["manual"],
    visibility = ["//:__pkg__"],
)
