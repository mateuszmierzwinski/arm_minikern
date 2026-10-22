load("@io_bazel_rules_go//go:def.bzl", "go_library", "go_test")

# The NBD protocol and one session's logic: the hexagon. Pure Go over the
# interfaces in interfaces.go, standard library only.
go_library(
    name = "nbd",
    srcs = [
        "commands.go",
        "errors.go",
        "interfaces.go",
        "message.go",
        "models.go",
        "options.go",
        "service.go",
        "utils.go",
    ],
    importpath = "github.com/mateuszmierzwinski/arm_minikern/server/internal/nbd",
    visibility = ["//server:__subpackages__"],
)

go_test(
    name = "nbd_test",
    size = "small",
    srcs = [
        "cve_test.go",
        "message_test.go",
        "service_test.go",
        "utils_test.go",
    ],
    embed = [":nbd"],
)
