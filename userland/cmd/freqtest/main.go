// freqtest keeps every CPU busy for long enough that the frequency
// governor has to make every decision it can make.
//
// It proves nothing about the hardware — QEMU has no PLL and no thermal
// sensor — and it is not meant to. What it drives is the chain above them:
// the load measurement, the policy, and the governor that applies the
// result. The kernel is booted with a fake sensor that ramps from cold to
// past critical and back, so one run walks that whole range while this
// program holds the load at 100%.
package main

import (
	"fmt"
	"os"
	"runtime"
	"sync"
	"time"
)

// busyFor spins on every CPU until the deadline, in a way the compiler
// cannot elide.
func busyFor(d time.Duration) uint64 {
	var wg sync.WaitGroup
	results := make([]uint64, runtime.NumCPU())
	deadline := time.Now().Add(d)
	for i := range results {
		wg.Add(1)
		go func(slot int) {
			defer wg.Done()
			var h uint64 = 1469598103934665603
			for n := 0; ; n++ {
				h ^= uint64(n)
				h *= 1099511628211
				if n%4096 == 0 && time.Now().After(deadline) {
					break
				}
			}
			results[slot] = h
		}(i)
	}
	wg.Wait()
	var all uint64
	for _, r := range results {
		all ^= r
	}
	return all
}

func main() {
	fmt.Printf("freqtest: %d cpus, holding them busy\n", runtime.NumCPU())
	if busyFor(5*time.Second) == 0 {
		fmt.Println("the work was elided")
		os.Exit(1)
	}
	fmt.Println("freqtest ok")
	os.Exit(7)
}
