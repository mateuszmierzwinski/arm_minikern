// Command netbootinit is the init for the netboot test: it proves that the
// whole root filesystem arrived over the network intact and that Go runs
// from it. It prints the runtime it was built with, reads a file that is
// not the binary (so the archive, not just /init, came across), lists the
// root, and exits 0.
package main

import (
	"fmt"
	"os"
	"runtime"
	"sort"
	"strings"
)

// motd is a file the rootfs carries; the test fixes its contents.
const motd = "/etc/motd"

func main() {
	fmt.Println("netbooted init running on", runtime.Compiler, runtime.Version(), runtime.GOARCH)
	data, err := os.ReadFile(motd)
	if err != nil {
		fmt.Println("FAIL cannot read", motd+":", err)
		os.Exit(1)
	}
	fmt.Printf("motd: %q (%d bytes)\n", strings.TrimSpace(string(data)), len(data))
	entries, err := os.ReadDir("/")
	if err != nil {
		fmt.Println("FAIL cannot list /:", err)
		os.Exit(1)
	}
	names := make([]string, 0, len(entries))
	for _, e := range entries {
		names = append(names, e.Name())
	}
	sort.Strings(names)
	fmt.Println("root:", strings.Join(names, " "))
	fmt.Println("NETBOOT INIT OK")
}
