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
	"syscall"
	"time"
)

// failed records that some check did not pass.
// check reports one result.
// start is a monotonic reference taken before anything else, so the clock
// check can prove that setting the wall clock did not move it.
var start = time.Now()

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

// tmpfs checks the one writable filesystem: a great deal of Go code needs
// somewhere to put a file, and until there was a /tmp there was nowhere.
func tmpfs() bool {
	f, err := os.CreateTemp("/tmp", "probe")
	if err != nil {
		fmt.Println("  createtemp:", err)
		return false
	}
	name := f.Name()
	payload := []byte("written to a filesystem that is only memory\n")
	if _, err := f.Write(payload); err != nil {
		fmt.Println("  write:", err)
		return false
	}
	if err := f.Close(); err != nil {
		fmt.Println("  close:", err)
		return false
	}
	back, err := os.ReadFile(name)
	if err != nil || string(back) != string(payload) {
		fmt.Printf("  read back: %q %v\n", back, err)
		return false
	}
	return tmpfsGrows() && tmpfsLists(name) && tmpfsRemoves(name) && tmpfsRefuses()
}

// tmpfsGrows writes more than one buffer's worth, a little at a time: a
// file that grows has to keep what it already had.
func tmpfsGrows() bool {
	const name = "/tmp/grown"
	f, err := os.Create(name)
	if err != nil {
		fmt.Println("  create:", err)
		return false
	}
	defer func() { _ = os.Remove(name) }()
	for i := 0; i < 2000; i++ {
		if _, err := fmt.Fprintf(f, "line %d\n", i); err != nil {
			fmt.Println("  append:", err)
			_ = f.Close()
			return false
		}
	}
	if err := f.Close(); err != nil {
		fmt.Println("  close:", err)
		return false
	}
	data, err := os.ReadFile(name)
	if err != nil {
		fmt.Println("  reread:", err)
		return false
	}
	if !strings.HasPrefix(string(data), "line 0\n") || !strings.HasSuffix(string(data), "line 1999\n") {
		fmt.Printf("  grown file is %d bytes and starts %q\n", len(data), data[:min(12, len(data))])
		return false
	}
	return true
}

func tmpfsLists(name string) bool {
	ents, err := os.ReadDir("/tmp")
	if err != nil {
		fmt.Println("  readdir /tmp:", err)
		return false
	}
	want := strings.TrimPrefix(name, "/tmp/")
	for _, e := range ents {
		if e.Name() == want {
			return true
		}
	}
	fmt.Printf("  /tmp does not list %s (%d entries)\n", want, len(ents))
	return false
}

func tmpfsRemoves(name string) bool {
	if err := os.Remove(name); err != nil {
		fmt.Println("  remove:", err)
		return false
	}
	if _, err := os.Open(name); !os.IsNotExist(err) {
		fmt.Println("  the file is still there:", err)
		return false
	}
	return true
}

// tmpfsRefuses checks what must not work: a subdirectory (there are none),
// and removing something on the read-only root.
func tmpfsRefuses() bool {
	if _, err := os.Create("/tmp/sub/file"); err == nil {
		fmt.Println("  a subdirectory was accepted")
		return false
	}
	if err := os.Remove("/etc/motd"); err == nil {
		fmt.Println("  a file on the read-only root was removed")
		return false
	}
	return true
}

// clockSet checks the wall clock can be set, which is what lets an NTP
// client written in Go fix the time on a board with no battery.
func clockSet(epoch int64) bool {
	want := time.Unix(epoch, 0)
	if err := syscall.Settimeofday(&syscall.Timeval{Sec: int32(epoch)}); err != nil {
		fmt.Println("  settimeofday:", err)
		return false
	}
	now := time.Now()
	if now.Before(want) || now.Sub(want) > 5*time.Second {
		fmt.Printf("  the clock reads %v, want about %v\n", now, want)
		return false
	}
	// The monotonic clock must not have moved with it.
	before := time.Since(start)
	if err := syscall.Settimeofday(&syscall.Timeval{Sec: int32(epoch + 86400)}); err != nil {
		fmt.Println("  settimeofday again:", err)
		return false
	}
	if jumped := time.Since(start) - before; jumped > time.Second {
		fmt.Printf("  the monotonic clock jumped by %v when the wall clock was set\n", jumped)
		return false
	}
	return true
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
	check("tmpfs", tmpfs())
	check("clock-set", clockSet(1767225600)) // 2026-01-01, well past any boot default
	if failed {
		os.Exit(1)
	}
	fmt.Println("ALL PASS")
}
