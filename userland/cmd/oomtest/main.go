// oomtest runs the board out of memory on purpose.
//
// The policy being tested is the kernel's answer to "there is no page for
// you": the process is killed with SIGKILL and the reason is printed. Not a
// segmentation fault — a runtime that catches those (Go's does) would
// report "invalid memory address" and the real cause would be lost.
//
// So this program is **expected to die**. Its last line before that says
// how far it got, which is what makes the test's own failure legible if the
// policy ever changes.
package main

import (
	"fmt"
	"os"
	"syscall"
)

func main() {
	const chunk = 8 << 20 // 8 MiB at a time, touched so it is really ours
	kept := make([][]byte, 0, 256)
	for i := 0; ; i++ {
		m, err := syscall.Mmap(-1, 0, chunk, syscall.PROT_READ|syscall.PROT_WRITE,
			syscall.MAP_PRIVATE|syscall.MAP_ANON)
		if err != nil {
			// Running out of *address space* or of mapping slots is a
			// different answer, and a legitimate one: say so and stop.
			fmt.Printf("oomtest: mmap refused after %d MiB: %v\n", i*chunk>>20, err)
			os.Exit(2)
		}
		for at := 0; at < len(m); at += 4096 {
			m[at] = byte(i) // the touch is what asks for the page
		}
		kept = append(kept, m)
		fmt.Printf("oomtest: %d MiB resident\n", (i+1)*chunk>>20)
	}
}
