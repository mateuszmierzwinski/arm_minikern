load("@io_bazel_rules_go//go:def.bzl", "go_library", "go_test")

# The netboot protocol and the server's session logic: the hexagon. Pure
# Go over the interfaces in interfaces.go, standard library only.
go_library(
    name = "mknb",
    srcs = [
        "errors.go",
        "interfaces.go",
        "message.go",
        "models.go",
        "service.go",
        "tokens.go",
        "utils.go",
    ],
    importpath = "github.com/mateuszmierzwinski/arm_minikern/server/internal/mknb",
    visibility = ["//server:__subpackages__"],
)

go_test(
    name = "mknb_test",
    size = "small",
    srcs = [
        "cve_test.go",
        "message_test.go",
        "service_test.go",
        "utils_test.go",
    ],
    embed = [":mknb"],
)
