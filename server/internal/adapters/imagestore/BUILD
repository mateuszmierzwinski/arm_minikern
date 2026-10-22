load("@io_bazel_rules_go//go:def.bzl", "go_library", "go_test")

go_library(
    name = "imagestore",
    srcs = ["store.go"],
    importpath = "github.com/mateuszmierzwinski/arm_minikern/server/internal/adapters/imagestore",
    visibility = ["//server:__subpackages__"],
    deps = ["//server/internal/mknb"],
)

go_test(
    name = "imagestore_test",
    size = "small",
    srcs = ["store_test.go"],
    embed = [":imagestore"],
    deps = ["//server/internal/mknb"],
)
