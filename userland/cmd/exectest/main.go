// exectest runs other programs: os/exec end to end on minikern.
//
// It is one binary playing both parts. The parent re-runs itself with an
// argument, which is the ordinary shape of a test for fork and exec and
// needs no second program in the root filesystem.
package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strings"
	"syscall"
)

// self is the program's own path, which is what it execs.
var self = os.Args[0]

func child(role string) {
	switch role {
	case "hello":
		fmt.Print("hello from the child")
		os.Exit(0)
	case "status":
		os.Exit(9)
	case "echo":
		b, _ := io.ReadAll(os.Stdin)
		fmt.Printf("child read %q", b)
		os.Exit(0)
	case "env":
		fmt.Print(os.Getenv("MINIKERN_PASSED"))
		os.Exit(0)
	case "replaced":
		fmt.Print("the new image is running")
		os.Exit(0)
	case "replacer":
		// syscall.Exec from a Go program is an execve from a process with
		// several threads, which the kernel supports by ending the others
		// first (kernel/exec.h). Nothing after this line should run.
		if err := syscall.Exec(self, []string{self, "child", "replaced"}, os.Environ()); err != nil {
			fmt.Print("exec failed: ", err)
			os.Exit(1)
		}
		fmt.Print("execve returned, which it must never do")
		os.Exit(1)
	case "args":
		fmt.Print(strings.Join(os.Args[3:], "|")) // past the program, "child" and the role
		os.Exit(0)
	}
	fmt.Println("unknown role", role)
	os.Exit(1)
}

// check reports one result and remembers the first failure.
var failed bool

func check(name string, ok bool, detail string) {
	if ok {
		fmt.Printf("ok   %s\n", name)
		return
	}
	failed = true
	fmt.Printf("FAIL %s: %s\n", name, detail)
}

// output runs this program in a child role and returns what it printed.
func output(role string, extra ...string) (string, error) {
	cmd := exec.Command(self, append([]string{"child", role}, extra...)...)
	b, err := cmd.CombinedOutput()
	return string(b), err
}

func runHello() {
	out, err := output("hello")
	check("exec runs a child and its output comes back", err == nil && out == "hello from the child",
		fmt.Sprintf("out=%q err=%v", out, err))
}

func runStatus() {
	_, err := output("status")
	var ee *exec.ExitError
	code := -1
	if errors.As(err, &ee) {
		code = ee.ExitCode()
	}
	check("a child's exit status reaches the parent", code == 9, fmt.Sprintf("code=%d err=%v", code, err))
}

func runStdin() {
	cmd := exec.Command(self, "child", "echo")
	in, err := cmd.StdinPipe()
	if err != nil {
		check("a pipe carries bytes to the child", false, err.Error())
		return
	}
	out := &strings.Builder{}
	cmd.Stdout = out
	if err = cmd.Start(); err != nil {
		check("a pipe carries bytes to the child", false, err.Error())
		return
	}
	_, _ = io.WriteString(in, "down the pipe")
	_ = in.Close()
	err = cmd.Wait()
	check("a pipe carries bytes to the child", err == nil && out.String() == `child read "down the pipe"`,
		fmt.Sprintf("out=%q err=%v", out.String(), err))
}

func runEnv() {
	cmd := exec.Command(self, "child", "env")
	cmd.Env = append(os.Environ(), "MINIKERN_PASSED=through the exec")
	b, err := cmd.Output()
	check("the environment reaches the new image", err == nil && string(b) == "through the exec",
		fmt.Sprintf("out=%q err=%v", b, err))
}

func runArgs() {
	out, err := output("args", "one", "two", "three")
	check("every argument reaches the new image", err == nil && out == "one|two|three",
		fmt.Sprintf("out=%q err=%v", out, err))
}

// runReplace proves a multi-threaded process can replace its own image.
// The child runs the Go runtime — so it has several threads — and calls
// syscall.Exec; what comes back is the output of the program that replaced
// it, from the same process.
func runReplace() {
	out, err := output("replacer")
	check("a process with threads can replace its own image", err == nil && out == "the new image is running",
		fmt.Sprintf("out=%q err=%v", out, err))
}

func runMissing() {
	err := exec.Command("/no/such/program").Run()
	check("execing something that is not there fails cleanly", err != nil, "it reported success")
}

// runPipe uses a pipe on its own, with no child involved: the end-of-file
// that a closed writer produces is what every reader above depends on.
func runPipe() {
	r, w, err := os.Pipe()
	if err != nil {
		check("a pipe reports end of file when its writer closes", false, err.Error())
		return
	}
	go func() {
		_, _ = io.WriteString(w, "half a message")
		_ = w.Close()
	}()
	b, err := io.ReadAll(r)
	_ = r.Close()
	check("a pipe reports end of file when its writer closes", err == nil && string(b) == "half a message",
		fmt.Sprintf("read=%q err=%v", b, err))
}

func main() {
	if len(os.Args) > 2 && os.Args[1] == "child" {
		child(os.Args[2])
		return
	}
	fmt.Printf("exectest: pid %d\n", os.Getpid())
	runPipe()
	runHello()
	runStatus()
	runStdin()
	runEnv()
	runArgs()
	runReplace()
	runMissing()
	if failed {
		fmt.Println("exectest failed")
		os.Exit(1)
	}
	fmt.Println("exectest ok")
	os.Exit(7)
}
