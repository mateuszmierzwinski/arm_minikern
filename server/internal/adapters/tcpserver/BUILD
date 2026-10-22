load("@io_bazel_rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "tcpserver",
    srcs = ["server.go"],
    importpath = "github.com/mateuszmierzwinski/arm_minikern/server/internal/adapters/tcpserver",
    visibility = ["//server:__subpackages__"],
    deps = ["//server/internal/nbd"],
)

go_test(
    name = "tcpserver_test",
    size = "small",
    srcs = ["server_test.go"],
    embed = [":tcpserver"],
    deps = ["//server/internal/nbd"],
)
