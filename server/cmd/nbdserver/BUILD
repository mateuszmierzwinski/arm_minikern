load("@io_bazel_rules_go//go:def.bzl", "go_binary", "go_library", "go_test")
load("//:version.bzl", "MINIKERN_VERSION")

go_library(
    name = "nbdserver_lib",
    srcs = [
        "main.go",
        "version.go",
    ],
    importpath = "github.com/mateuszmierzwinski/arm_minikern/server/cmd/nbdserver",
    visibility = ["//visibility:private"],
    x_defs = {
        "seriesVersion": MINIKERN_VERSION,
        "buildTimestamp": "{BUILD_TIMESTAMP}",
    },
    deps = [
        "//server/internal/adapters/tcpserver",
        "//server/internal/adapters/volumefile",
        "//server/internal/nbd",
    ],
)

go_binary(
    name = "nbdserver",
    embed = [":nbdserver_lib"],
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)

go_test(
    name = "nbdserver_test",
    size = "small",
    srcs = ["main_test.go"],
    embed = [":nbdserver_lib"],
)
