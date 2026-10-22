// smptest asks the kernel how many cores it has and then uses them.
//
// Go sizes GOMAXPROCS from sched_getaffinity, so NumCPU is the kernel's
// answer coming back through the runtime. The work that follows is there to
// be *seen*: the kernel's per-core counters (minikern.netstat=1) must show
// every core having entered the kernel, which is the part a user-space
// program cannot claim for itself — on one core this program finishes just
// the same, only more slowly.
package main

import (
	"fmt"
	"os"
	"runtime"
	"sync"
	"sync/atomic"
	"time"
)

// spin burns CPU in a way the compiler cannot elide, returning a value that
// depends on every iteration.
func spin(rounds int) uint64 {
	var h uint64 = 1469598103934665603
	for i := 0; i < rounds; i++ {
		h ^= uint64(i)
		h *= 1099511628211
	}
	return h
}

// parallel runs spin on n goroutines at once and reports how long it took.
func parallel(n, rounds int) time.Duration {
	var wg sync.WaitGroup
	var sink atomic.Uint64
	start := time.Now()
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			sink.Add(spin(rounds))
		}()
	}
	wg.Wait()
	if sink.Load() == 0 {
		fmt.Println("the work was elided")
	}
	return time.Since(start)
}

// liveOnEveryP proves the runtime really has n Ps to run on: n goroutines
// must all be inside the barrier at once, and none of them yields, so the
// count can only reach n if the runtime is running them simultaneously.
func liveOnEveryP(n int) bool {
	var arrived atomic.Int32
	var seen atomic.Int32
	var wg sync.WaitGroup
	deadline := time.Now().Add(10 * time.Second)
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			arrived.Add(1)
			for time.Now().Before(deadline) {
				if arrived.Load() == int32(n) {
					seen.Add(1)
					return
				}
			}
		}()
	}
	wg.Wait()
	return seen.Load() == int32(n)
}

func main() {
	cpus := runtime.NumCPU()
	procs := runtime.GOMAXPROCS(0)
	fmt.Printf("cpus %d, gomaxprocs %d\n", cpus, procs)
	if cpus != procs {
		fmt.Println("FAIL: GOMAXPROCS does not follow NumCPU")
		os.Exit(1)
	}
	if !liveOnEveryP(cpus) {
		fmt.Println("FAIL: the runtime could not run one goroutine per cpu at once")
		os.Exit(1)
	}
	fmt.Printf("all %d goroutines ran at once\n", cpus)

	const rounds = 4000000
	one := parallel(1, rounds)
	many := parallel(cpus, rounds)
	// Reported, never asserted: under emulation the host decides how much
	// real parallelism there is, so a ratio is evidence and not a contract.
	fmt.Printf("one %v, %d at once %v (%.2fx the work in %.2fx the time)\n",
		one, cpus, many, float64(cpus), many.Seconds()/one.Seconds())
	fmt.Println("smptest ok")
	os.Exit(7)
}
