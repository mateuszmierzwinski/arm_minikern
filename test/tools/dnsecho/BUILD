load("@io_bazel_rules_go//go:def.bzl", "go_binary", "go_library")

# The host side of the DNS test: one name, one answer, so "the kernel can
# resolve a name" is a claim about the kernel and not about the internet.
go_library(
    name = "dnsecho_lib",
    srcs = ["main.go"],
    importpath = "github.com/mateuszmierzwinski/arm_minikern/test/tools/dnsecho",
    visibility = ["//visibility:private"],
)

go_binary(
    name = "dnsecho",
    embed = [":dnsecho_lib"],
    visibility = ["//:__pkg__"],
)
