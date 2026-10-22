package main

import (
	"strings"
	"testing"
)

func TestVersion(t *testing.T) {
	seriesVersion, buildTimestamp = "1.0", "1760000000"
	if got := version(); got != "1.0.1760000000" {
		t.Errorf("version() = %q", got)
	}
}

// An unstamped build must say 0 rather than invent a number.
func TestVersionWithoutAStamp(t *testing.T) {
	seriesVersion, buildTimestamp = "1.0", ""
	if got := version(); got != "1.0.0" {
		t.Errorf("version() = %q, want 1.0.0", got)
	}
	seriesVersion, buildTimestamp = "1.0", "not-a-number"
	if got := version(); got != "1.0.0" {
		t.Errorf("version() = %q, want 1.0.0", got)
	}
}

func TestVersionIsNotEmpty(t *testing.T) {
	seriesVersion, buildTimestamp = "2.5", "17"
	if !strings.HasPrefix(version(), "2.5.") {
		t.Errorf("version() = %q", version())
	}
}

func TestIgnoreCancel(t *testing.T) {
	if err := ignoreCancel(nil); err != nil {
		t.Errorf("nil: %v", err)
	}
	if err := ignoreCancel(errBoom{}); err == nil {
		t.Error("a real error must survive")
	}
}

type errBoom struct{}

func (errBoom) Error() string { return "boom" }

func BenchmarkVersion(b *testing.B) {
	seriesVersion, buildTimestamp = "1.0", "1760000000"
	for b.Loop() {
		_ = version()
	}
}
