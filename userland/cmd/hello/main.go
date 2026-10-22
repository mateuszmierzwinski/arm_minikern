// Command hello is minikern's demonstration init: it greets on stdout and
// stderr, sleeps, exercises the allocator and exits with a status the
// kernel reports. It builds with both TinyGo and the gc toolchain, and the
// same binary runs on Linux (qemu-arm) and on minikern.
package main

import (
	"fmt"
	"os"
	"runtime"
	"time"
)

// exitStatus is what the boot tests expect minikern to report.
const exitStatus = 7

func main() {
	fmt.Println("hello from", runtime.Compiler, "Go on minikern, pid", os.Getpid())
	fmt.Fprintln(os.Stderr, "this line went to stderr")
	start := time.Now()
	time.Sleep(20 * time.Millisecond)
	fmt.Println("slept", time.Since(start) >= 20*time.Millisecond)
	m := map[string]int{}
	for i := 0; i < 1000; i++ {
		m[fmt.Sprint(i)] = i
	}
	fmt.Println("map entries:", len(m))
	os.Exit(exitStatus)
}
