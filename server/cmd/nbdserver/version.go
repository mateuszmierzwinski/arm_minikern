package main

import (
	"fmt"
	"strconv"
)

// Set by Bazel through x_defs: the series from //:version.bzl and the build
// from the --stamp clock (master Rule 28.1). An unstamped build reports 0.
var (
	seriesVersion  = "0.0"
	buildTimestamp = "0"
)

// version is MAJOR.MINOR.BUILD, where BUILD is the unix build time.
func version() string {
	build, err := strconv.ParseInt(buildTimestamp, 10, 64)
	if err != nil {
		build = 0
	}
	return fmt.Sprintf("%s.%d", seriesVersion, build)
}
