package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestVersionHasBothParts(t *testing.T) {
	seriesVersion, buildTimestamp = "1.2", "1700000000"
	if got := version(); got != "1.2.1700000000" {
		t.Fatalf("got %q", got)
	}
	// An unstamped build reports build 0 rather than inventing a number.
	seriesVersion, buildTimestamp = "1.2", "0"
	if got := version(); got != "1.2.0" {
		t.Fatalf("got %q", got)
	}
}

func TestAnnounceWritesTheAddress(t *testing.T) {
	path := filepath.Join(t.TempDir(), "ready")
	if err := announce(path, "127.0.0.1:7634"); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if strings.TrimSpace(string(data)) != "127.0.0.1:7634" {
		t.Fatalf("got %q", data)
	}
	info, err := os.Stat(path)
	if err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("mode %v, %v", info.Mode(), err)
	}
}

func TestAnnounceWithoutAPathDoesNothing(t *testing.T) {
	if err := announce("", "x"); err != nil {
		t.Fatal(err)
	}
}

func TestAnnounceReportsAnUnwritablePath(t *testing.T) {
	if err := announce(filepath.Join(t.TempDir(), "no", "such", "dir", "f"), "x"); err == nil {
		t.Fatal("a bad path was accepted")
	}
}

// The discard logger must not panic on any argument shape.
func TestDiscardLogger(t *testing.T) {
	var l discardLogger
	l.Printf("anything %d %s", 1, "two")
}

func TestSystemClockAdvances(t *testing.T) {
	var c systemClock
	if c.Now().IsZero() {
		t.Fatal("the clock returned the zero time")
	}
}
