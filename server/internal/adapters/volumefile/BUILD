load("@io_bazel_rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "volumefile",
    srcs = [
        "store.go",
        "utils.go",
    ],
    importpath = "github.com/mateuszmierzwinski/arm_minikern/server/internal/adapters/volumefile",
    visibility = ["//server:__subpackages__"],
    deps = ["//server/internal/nbd"],
)

go_test(
    name = "volumefile_test",
    size = "small",
    srcs = ["store_test.go"],
    embed = [":volumefile"],
    deps = ["//server/internal/nbd"],
)
