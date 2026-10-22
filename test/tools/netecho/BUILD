load("@io_bazel_rules_go//go:def.bzl", "go_binary", "go_library")

# The host side of the socket tests: it runs on the build machine, so it is
# an ordinary host binary rather than a linux/arm one.
go_library(
    name = "netecho_lib",
    srcs = ["main.go"],
    importpath = "github.com/mateuszmierzwinski/arm_minikern/test/tools/netecho",
    visibility = ["//visibility:private"],
)

go_binary(
    name = "netecho",
    embed = [":netecho_lib"],
    visibility = ["//:__pkg__"],
)
