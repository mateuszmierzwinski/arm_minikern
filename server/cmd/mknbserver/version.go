package main

import "fmt"

// The version, in the monorepo's MAJOR.MINOR.BUILD scheme (master Rule
// 28): the series comes from //:version.bzl and the build is Bazel's
// --stamp timestamp. A build without --stamp reports 0 rather than
// inventing a number.
var (
	seriesVersion  = "0.0"
	buildTimestamp = "0"
)

// version is the full version string.
func version() string {
	return fmt.Sprintf("%s.%s", seriesVersion, buildTimestamp)
}
