load("@io_bazel_rules_go//go:def.bzl", "go_binary", "go_library", "go_test")
load("//:version.bzl", "MINIKERN_VERSION")

go_library(
    name = "mknbserver_lib",
    srcs = [
        "main.go",
        "version.go",
    ],
    importpath = "github.com/mateuszmierzwinski/arm_minikern/server/cmd/mknbserver",
    visibility = ["//visibility:private"],
    # The series from //:version.bzl, the build from Bazel's --stamp clock
    # (master Rule 28.1); an unstamped build reports build 0.
    x_defs = {
        "seriesVersion": MINIKERN_VERSION,
        "buildTimestamp": "{BUILD_TIMESTAMP}",
    },
    deps = [
        "//server/internal/adapters/imagestore",
        "//server/internal/adapters/udptransport",
        "//server/internal/mknb",
    ],
)

go_binary(
    name = "mknbserver",
    embed = [":mknbserver_lib"],
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
)

go_test(
    name = "mknbserver_test",
    size = "small",
    srcs = ["main_test.go"],
    embed = [":mknbserver_lib"],
)
