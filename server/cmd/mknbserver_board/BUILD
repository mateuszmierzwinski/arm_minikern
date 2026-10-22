load("@io_bazel_rules_go//go:def.bzl", "go_binary")

# The same server, cross-compiled to run **on** a board — which is why it
# lives here rather than in userland/: it is the server, not a program that
# uses one, and the server's internals are private to this tree.
go_binary(
    name = "mknbserver_board",
    srcs = ["main.go"],
    goarch = "arm",
    goos = "linux",
    pure = "on",
    static = "on",
    visibility = ["//:__pkg__"],
    deps = [
        "//server/internal/adapters/imagestore",
        "//server/internal/adapters/udptransport",
        "//server/internal/mknb",
    ],
)
