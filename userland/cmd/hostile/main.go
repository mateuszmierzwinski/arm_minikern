// Command hostile is minikern's syscall-boundary regression suite (master
// Rule 25, in spirit): it attacks the kernel from user space with bad
// pointers, hostile lengths and forbidden addresses, and checks that every
// attempt fails with the errno Linux gives, while the process (and the
// kernel) carry on. It prints PASS/FAIL per case and exits 0 only when all
// pass. Every expected errno is the one Linux returns for the same call.
package main

import (
	"fmt"
	"os"
	"runtime/debug"
	"strings"
	"syscall"
	"unsafe"
)

// Linux ARM EABI syscall numbers the syscall package does not export.
const (
	sysMmap2        = 192
	sysGetrandom    = 384
	sysClockGettime = 263
	sysEpollCreate1 = 357
	sysEpollCtl     = 251
	sysUnknown      = 0x1ff
	sysExecve       = 11
	sysWait4        = 114
	sysPipe2        = 359
	sysClone        = 120
)

const (
	kernelAddr = 0xc0008000 // the kernel's own text, in its high half
	ioAddr     = 0xf1c28000 // UART0 through the kernel's I/O window
	wrapAddr   = 0xfffffff0
)

var failed bool

// expect reports whether a raw syscall failed with want.
func expect(name string, err syscall.Errno, want syscall.Errno) {
	if err == want {
		fmt.Println("PASS", name)
		return
	}
	fmt.Printf("FAIL %s: got errno %d (%v), want %d (%v)\n", name, int(err), err, int(want), want)
	failed = true
}

func check(name string, ok bool, detail string) {
	if ok {
		fmt.Println("PASS", name)
		return
	}
	fmt.Println("FAIL", name+":", detail)
	failed = true
}

func sys3(nr, a, b, c uintptr) syscall.Errno {
	_, _, e := syscall.RawSyscall(nr, a, b, c)
	return e
}

func sys6(nr, a, b, c, d, e, f uintptr) (uintptr, syscall.Errno) {
	r, _, errno := syscall.RawSyscall6(nr, a, b, c, d, e, f)
	return r, errno
}

func badPointers() {
	expect("write-from-kernel-memory", sys3(syscall.SYS_WRITE, 1, kernelAddr, 16), syscall.EFAULT)
	expect("write-wrapping-buffer", sys3(syscall.SYS_WRITE, 1, wrapAddr, 64), syscall.EFAULT)
	expect("open-kernel-path", sys3(syscall.SYS_OPEN, kernelAddr, 0, 0), syscall.EFAULT)
	expect("fstat-into-kernel", sys3(syscall.SYS_FSTAT64, 1, kernelAddr, 0), syscall.EFAULT)
	expect("getrandom-into-kernel", sys3(sysGetrandom, kernelAddr, 16, 0), syscall.EFAULT)
	expect("clock-into-kernel", sys3(sysClockGettime, 0, kernelAddr, 0), syscall.EFAULT)
	expect("sigaction-kernel-act", sys6e(syscall.SYS_RT_SIGACTION, uintptr(syscall.SIGUSR1), kernelAddr, 0, 8), syscall.EFAULT)
	f, err := os.Open("/etc/motd")
	if err != nil {
		check("open-motd", false, err.Error())
		return
	}
	defer f.Close()
	expect("read-into-kernel", sys3(syscall.SYS_READ, f.Fd(), kernelAddr, 16), syscall.EFAULT)
	d, _ := os.Open("/")
	defer d.Close()
	expect("getdents-into-kernel", sys3(syscall.SYS_GETDENTS64, d.Fd(), kernelAddr, 4096), syscall.EFAULT)
}

func sys6e(nr, a, b, c, d uintptr) syscall.Errno {
	_, e := sys6(nr, a, b, c, d, 0, 0)
	return e
}

func hostileLengths() {
	long := strings.Repeat("a", 300) + "\x00"
	p := unsafe.Pointer(unsafe.StringData(long))
	expect("open-overlong-path", sys3(syscall.SYS_OPEN, uintptr(p), 0, 0), syscall.ENAMETOOLONG)
	iov := make([]syscall.Iovec, 1)
	expect("writev-huge-count", sys3(syscall.SYS_WRITEV, 1, uintptr(unsafe.Pointer(&iov[0])), 2000), syscall.EINVAL)
	bad := []syscall.Iovec{{Base: (*byte)(unsafe.Pointer(uintptr(kernelAddr))), Len: 8}}
	expect("writev-kernel-base", sys3(syscall.SYS_WRITEV, 1, uintptr(unsafe.Pointer(&bad[0])), 1), syscall.EFAULT)
	expect("sigaction-bad-setsize", sys6e(syscall.SYS_RT_SIGACTION, uintptr(syscall.SIGUSR1), 0, 0, 16), syscall.EINVAL)
	expect("sigaction-sigkill", sys6e(syscall.SYS_RT_SIGACTION, uintptr(syscall.SIGKILL), uintptr(unsafe.Pointer(&iov[0])), 0, 8), syscall.EINVAL)
	ss := struct{ sp, flags, size uint32 }{sp: 0x10000, size: 100}
	expect("sigaltstack-too-small", sys3(syscall.SYS_SIGALTSTACK, uintptr(unsafe.Pointer(&ss)), 0, 0), syscall.ENOMEM)
	ts := struct{ sec, nsec int32 }{-1, 0}
	expect("nanosleep-negative", sys3(syscall.SYS_NANOSLEEP, uintptr(unsafe.Pointer(&ts)), 0, 0), syscall.EINVAL)
	expect("clock-unknown-id", sys3(sysClockGettime, 99, uintptr(unsafe.Pointer(&ts)), 0), syscall.EINVAL)
}

const (
	protRW   = syscall.PROT_READ | syscall.PROT_WRITE
	anonPriv = syscall.MAP_PRIVATE | syscall.MAP_ANON
)

func memoryMap() {
	_, e := sys6(sysMmap2, kernelAddr&^0xfff, 4096, protRW, anonPriv|syscall.MAP_FIXED, ^uintptr(0), 0)
	expect("mmap-fixed-in-kernel", e, syscall.EINVAL)
	_, e = sys6(sysMmap2, 0x7ffff000, 0x2000, protRW, anonPriv|syscall.MAP_FIXED, ^uintptr(0), 0)
	expect("mmap-fixed-crossing-kernel", e, syscall.EINVAL)
	_, e = sys6(sysMmap2, 0, 0xffff0000, protRW, anonPriv, ^uintptr(0), 0)
	expect("mmap-4gib", e, syscall.ENOMEM)
	expect("munmap-kernel", sys3(syscall.SYS_MUNMAP, kernelAddr&^0xfff, 4096, 0), syscall.EINVAL)
	expect("mprotect-kernel", sys3(syscall.SYS_MPROTECT, kernelAddr&^0xfff, 4096, 7), syscall.EINVAL)
	expect("mincore-misaligned", sys3(syscall.SYS_MINCORE, 0x10001, 4096, 0), syscall.EINVAL)
	r, _, _ := syscall.RawSyscall(syscall.SYS_BRK, kernelAddr, 0, 0)
	check("brk-into-kernel-refused", r != kernelAddr, fmt.Sprintf("brk returned %#x", r))
}

// vmaExhaustion maps regions with alternating rights (so they cannot merge)
// until the kernel says no, then gives them all back.
func vmaExhaustion() {
	var got []uintptr
	var last syscall.Errno
	for i := 0; i < 400; i++ {
		prot := uintptr(syscall.PROT_READ)
		if i%2 == 0 {
			prot = protRW
		}
		a, e := sys6(sysMmap2, 0, 4096, prot, anonPriv, ^uintptr(0), 0)
		if e != 0 {
			last = e
			break
		}
		got = append(got, a)
	}
	for _, a := range got {
		sys3(syscall.SYS_MUNMAP, a, 4096, 0)
	}
	expect("vma-table-exhaustion", last, syscall.ENOMEM)
	a, e := sys6(sysMmap2, 0, 4096, protRW, anonPriv, ^uintptr(0), 0)
	check("mmap-after-exhaustion", e == 0, e.Error())
	sys3(syscall.SYS_MUNMAP, a, 4096, 0)
}

func descriptors() {
	expect("close-unopened", sys3(syscall.SYS_CLOSE, 1000, 0, 0), syscall.EBADF)
	expect("close-negative", sys3(syscall.SYS_CLOSE, ^uintptr(0), 0, 0), syscall.EBADF)
	expect("dup2-out-of-range", sys3(syscall.SYS_DUP2, 1, 1000, 0), syscall.EBADF)
	var fds []int
	var last error
	for i := 0; i < 200; i++ {
		fd, err := syscall.Open("/etc/motd", syscall.O_RDONLY, 0)
		if err != nil {
			last = err
			break
		}
		fds = append(fds, fd)
	}
	for _, fd := range fds {
		syscall.Close(fd)
	}
	check("fd-exhaustion-emfile", last == syscall.EMFILE, fmt.Sprint(last))
	fd, err := syscall.Open("/etc/motd", syscall.O_RDONLY, 0)
	check("open-after-exhaustion", err == nil, fmt.Sprint(err))
	syscall.Close(fd)
}

func misc() {
	expect("unknown-syscall", sys3(sysUnknown, 0, 0, 0), syscall.ENOSYS)
	expect("futex-unaligned", sys6e(syscall.SYS_FUTEX, 0x10001, 0, 0, 0), syscall.EINVAL)
	expect("futex-kernel-word", sys6e(syscall.SYS_FUTEX, kernelAddr, 0, 0, 0), syscall.EFAULT)
	expect("tgkill-no-such-thread", sys3(syscall.SYS_TGKILL, uintptr(os.Getpid()), 9999, 0), syscall.ESRCH)
	expect("kill-no-such-process", sys3(syscall.SYS_KILL, 99, 0, 0), syscall.ESRCH)
	expect("open-for-write-readonly-root", errnoOf(syscall.Open("/etc/motd", syscall.O_WRONLY, 0)), syscall.EACCES)
	ep, _, _ := syscall.RawSyscall(sysEpollCreate1, 0, 0, 0)
	f, _ := os.Open("/etc/motd")
	ev := [4]uint32{1, 0, 0, 0}
	expect("epoll-regular-file", sys6e(sysEpollCtl, ep, 1, f.Fd(), uintptr(unsafe.Pointer(&ev[0]))), syscall.EPERM)
	f.Close()
	syscall.Close(int(ep))
}

func errnoOf(_ int, err error) syscall.Errno {
	if e, ok := err.(syscall.Errno); ok {
		return e
	}
	return 0
}

// faults: with SetPanicOnFault, a bad access is a recoverable panic, so the
// kernel's memory protection can be probed from Go.
func faults() {
	debug.SetPanicOnFault(true)
	check("read-kernel-memory-faults", faults_(func() { _ = *(*uint32)(unsafe.Pointer(uintptr(kernelAddr))) }), "read succeeded")
	check("read-io-window-faults", faults_(func() { _ = *(*uint32)(unsafe.Pointer(uintptr(ioAddr))) }), "read succeeded")
	check("write-own-text-faults", faults_(func() { *(*uint32)(unsafe.Pointer(uintptr(0x10000))) = 0 }), "write succeeded")
	buf := make([]uint32, 4)
	buf[0] = 0xe12fff1e // bx lr
	fn := *(*func())(unsafe.Pointer(&struct{ p unsafe.Pointer }{unsafe.Pointer(&buf[0])}))
	check("execute-heap-faults", faults_(fn), "heap was executable")
	debug.SetPanicOnFault(false)
}

func faults_(f func()) (faulted bool) {
	defer func() { faulted = recover() != nil }()
	f()
	return false
}

// processes attacks the fork/exec surface, which is the newest and the one
// that hands user-supplied strings and pointers to the loader.
//
// The access-control case is the important one: wait4 must refuse a pid that
// is not the caller's child. Otherwise any process could harvest the exit
// status of any other — the broken-access-control class (master Rule 25.3),
// reached through a pid rather than a URL.
func processes() {
	var st [1]uint32
	expect("execve with a NULL path is EFAULT", sys3(sysExecve, 0, 0, 0), syscall.EFAULT)
	expect("execve of the kernel's own address is EFAULT", sys3(sysExecve, kernelAddr, 0, 0), syscall.EFAULT)
	expect("execve of a directory is refused", sys3(sysExecve, strAddr("/dev"), 0, 0), syscall.EACCES)
	expect("execve of something that is not there is ENOENT",
		sys3(sysExecve, strAddr("/no/such/file"), 0, 0), syscall.ENOENT)
	expect("execve of a file that is not an ELF is refused",
		sys3(sysExecve, strAddr("/etc/hosts"), 0, 0), syscall.ENOEXEC)
	expect("execve with an argv pointer into the kernel is EFAULT",
		sys3(sysExecve, strAddr("/init"), kernelAddr, 0), syscall.EFAULT)
	expect("wait4 for a process that is not my child is ECHILD",
		sys3(sysWait4, 2, uintptr(unsafe.Pointer(&st[0])), 0), syscall.ECHILD)
	expect("wait4 for pid 1 from pid 1 is ECHILD", sys3(sysWait4, 1, 0, 0), syscall.ECHILD)
	expect("wait4 for any child when there are none is ECHILD",
		sys3(sysWait4, ^uintptr(0), 0, 0), syscall.ECHILD)
	expect("pipe2 into the kernel's memory is EFAULT", sys3(sysPipe2, kernelAddr, 0, 0), syscall.EFAULT)
	expect("pipe2 into an unmapped page is EFAULT", sys3(sysPipe2, wrapAddr, 0, 0), syscall.EFAULT)
	expect("clone asking for a pidfd is refused", sys3(sysClone, 0x1000|0x100, 0, 0), syscall.EINVAL)
	expect("clone of an address space of its own is refused", sys3(sysClone, 17 /* SIGCHLD */, 0, 0),
		syscall.ENOSYS)
	// This program is a Go program, so it has several threads, and execve
	// here only supports one (kernel/exec.h). The refusal must be clean and
	// must leave this process running — which is the whole claim, since the
	// alternative is a new image sharing an address space with threads that
	// are still in the old one.
	e := sys3(sysExecve, strAddr("/init"), 0, 0)
	check("execve from a multi-threaded process is refused, not attempted",
		e == syscall.EAGAIN, fmt.Sprintf("got errno %d (%v)", int(e), e))
	execveArgBomb()
	pipeExhaustion()
}

// strAddr is the address of a NUL-terminated copy of s, for the raw calls.
func strAddr(s string) uintptr {
	b := append([]byte(s), 0)
	keepAlive = append(keepAlive, b)
	return uintptr(unsafe.Pointer(&b[0]))
}

// keepAlive holds the byte slices whose addresses were handed to the kernel,
// so the collector cannot move or free them while a call is in flight.
var keepAlive [][]byte

// execveArgBomb hands execve more argument bytes than it will take. The
// limit must be a refusal, not a kernel that copies until something breaks.
func execveArgBomb() {
	const many = 4096
	argv := make([]uintptr, many+1)
	big := strAddr(strings.Repeat("a", 1024))
	for i := 0; i < many; i++ {
		argv[i] = big
	}
	argv[many] = 0
	e := sys3(sysExecve, strAddr("/init"), uintptr(unsafe.Pointer(&argv[0])), 0)
	check("an oversized argument list is refused", e == syscall.E2BIG,
		fmt.Sprintf("got errno %d (%v)", int(e), e))
}

// pipeExhaustion opens pipes until the descriptor table is full, which must
// be EMFILE rather than a kernel that allocates until it cannot.
func pipeExhaustion() {
	var fds [2]int32
	opened := 0
	var last syscall.Errno
	for i := 0; i < 256; i++ {
		last = sys3(sysPipe2, uintptr(unsafe.Pointer(&fds[0])), 0, 0)
		if last != 0 {
			break
		}
		opened += 2
	}
	check("pipes run out of descriptors cleanly", last == syscall.EMFILE && opened > 0,
		fmt.Sprintf("after %d descriptors: errno %d (%v)", opened, int(last), last))
}

func main() {
	badPointers()
	hostileLengths()
	memoryMap()
	vmaExhaustion()
	descriptors()
	misc()
	processes()
	faults()
	if failed {
		os.Exit(1)
	}
	fmt.Println("ALL HOSTILE CASES REFUSED")
}
