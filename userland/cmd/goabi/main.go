// Command goabi is minikern's Go ABI regression suite. Each check
// exercises one part of the Linux interface the gc runtime leans on, prints
// PASS or FAIL with its name, and the program exits 0 only when all pass.
// It runs unchanged on Linux (qemu-arm), which is how a failure is told
// apart from a kernel bug.
package main

import (
	"fmt"
	"os"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// failed records that some check did not pass.
// check reports one result.
func check(name string, ok bool) {
	if ok {
		fmt.Println("PASS", name)
	} else {
		fmt.Println("FAIL", name)
		failed = true
	}
}

var failed bool

func goroutines() bool {
	var wg sync.WaitGroup
	var sum atomic.Int64
	for i := 0; i < 100; i++ {
		wg.Add(1)
		go func(n int) {
			defer wg.Done()
			sum.Add(int64(n))
		}(i)
	}
	wg.Wait()
	return sum.Load() == 4950
}

func channels() bool {
	ch := make(chan int)
	done := make(chan int)
	go func() {
		t := 0
		for v := range ch {
			t += v
		}
		done <- t
	}()
	for i := 1; i <= 1000; i++ {
		ch <- i
	}
	close(ch)
	return <-done == 500500
}

// A loop with no function calls can only be stopped by async preemption
// (SIGURG + rewriting the signal context's pc), which the GC needs here.
func preemption() bool {
	var stop atomic.Bool
	go func() {
		x := 0
		for !stop.Load() {
			x++
		}
		_ = x
	}()
	time.Sleep(50 * time.Millisecond)
	runtime.GC()
	stop.Store(true)
	return true
}

func gc() bool {
	var keep [][]byte
	for i := 0; i < 2000; i++ {
		b := make([]byte, 64*1024)
		b[0] = byte(i)
		if i%100 == 0 {
			keep = append(keep, b)
		}
	}
	runtime.GC()
	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)
	return len(keep) == 20 && ms.NumGC > 0
}

func nilDeref() (ok bool) {
	defer func() {
		r := recover()
		ok = r != nil && strings.Contains(fmt.Sprint(r), "nil pointer")
	}()
	var p *int
	return *p == 1
}

func files() bool {
	data, err := os.ReadFile("/etc/motd")
	if err != nil {
		fmt.Println("  readfile:", err)
		return false
	}
	ents, err := os.ReadDir("/")
	if err != nil {
		fmt.Println("  readdir:", err)
		return false
	}
	names := []string{}
	for _, e := range ents {
		names = append(names, e.Name())
	}
	st, err := os.Stat("/etc/motd")
	if err != nil {
		fmt.Println("  stat:", err)
		return false
	}
	_, err = os.Open("/does/not/exist")
	fmt.Println("  root:", strings.Join(names, " "))
	return string(data) == "hello from the tar root\n" && st.Size() == int64(len(data)) && os.IsNotExist(err)
}

func timers() bool {
	start := time.Now()
	t := time.NewTimer(30 * time.Millisecond)
	tick := time.NewTicker(5 * time.Millisecond)
	n := 0
loop:
	for {
		select {
		case <-tick.C:
			n++
		case <-t.C:
			break loop
		}
	}
	tick.Stop()
	el := time.Since(start)
	return el >= 30*time.Millisecond && el < 2*time.Second && n >= 3
}

func main() {
	fmt.Println("go", runtime.Version(), runtime.GOOS+"/"+runtime.GOARCH, "args", os.Args, "NumCPU", runtime.NumCPU())
	check("env", os.Getenv("HOME") == "/")
	check("goroutines", goroutines())
	check("channels", channels())
	check("timers", timers())
	check("preemption", preemption())
	check("gc", gc())
	check("nil-deref-panic", nilDeref())
	check("files", files())
	if failed {
		os.Exit(1)
	}
	fmt.Println("ALL PASS")
}
