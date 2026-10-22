load("@io_bazel_rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "udptransport",
    srcs = ["transport.go"],
    importpath = "github.com/mateuszmierzwinski/arm_minikern/server/internal/adapters/udptransport",
    visibility = ["//server:__subpackages__"],
    deps = ["//server/internal/mknb"],
)

go_test(
    name = "udptransport_test",
    size = "small",
    srcs = ["transport_test.go"],
    embed = [":udptransport"],
    deps = ["//server/internal/mknb"],
)
