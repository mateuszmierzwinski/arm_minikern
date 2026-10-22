// nbdserver serves a directory of image files over NBD, so a minikern board
// can mount one as a block device over the network (CLAUDE.md §12).
//
// It is deliberately a separate program from mknbserver: netboot hands out a
// root filesystem read-only to anyone who asks, while this hands out a
// writable block device. Keeping them apart keeps that difference visible.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"

	"github.com/mateuszmierzwinski/arm_minikern/server/internal/adapters/tcpserver"
	"github.com/mateuszmierzwinski/arm_minikern/server/internal/adapters/volumefile"
	"github.com/mateuszmierzwinski/arm_minikern/server/internal/nbd"
)

var (
	listen   = flag.String("listen", "0.0.0.0:10809", "address to serve NBD on (10809 is the registered port)")
	volumes  = flag.String("volumes", "", "directory of <name>.img volume files (required)")
	readOnly = flag.Bool("read-only", false, "serve every volume read-only")
	maxConns = flag.Int("max-conns", 8, "how many clients may be connected at once")
	showVer  = flag.Bool("version", false, "print the version and exit")
)

func init() {
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: %s -volumes <dir> [options]\n\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "Serves image files over NBD for minikern boards.\n")
		fmt.Fprintf(os.Stderr, "An export named \"vol\" is the file <dir>/vol.img; a client that asks\n")
		fmt.Fprintf(os.Stderr, "for no name gets %s.img.\n\n", volumefile.DefaultName)
		fmt.Fprintf(os.Stderr, "Options:\n")
		flag.PrintDefaults()
	}
}

func main() {
	flag.Parse()
	if *showVer {
		fmt.Println(version())
		return
	}
	if *volumes == "" {
		fmt.Fprintln(os.Stderr, "error: -volumes is required")
		flag.Usage()
		os.Exit(1)
	}
	if err := run(); err != nil {
		log.Fatalf("nbdserver: %v", err)
	}
}

// run wires the composition root: a volume store, the session logic over
// it, and a TCP server over that (master Rule 7.3).
func run() error {
	logger := log.New(os.Stderr, "", log.LstdFlags)
	logger.Printf("nbdserver %s serving %s (read-only=%v)", version(), *volumes, *readOnly)
	store, err := volumefile.New(*volumes, *readOnly)
	if err != nil {
		return err
	}
	if names := store.List(); len(names) == 0 {
		logger.Printf("nbd: warning: no %s files in %s", volumefile.Suffix, *volumes)
	} else {
		logger.Printf("nbd: exports: %v", names)
	}
	server, err := tcpserver.New(*listen, nbd.NewService(store, logger), logger, *maxConns)
	if err != nil {
		return err
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	return ignoreCancel(server.Serve(ctx))
}

// ignoreCancel turns "we were asked to stop" into a clean exit.
func ignoreCancel(err error) error {
	if err != nil && ctxCancelled(err) {
		return nil
	}
	return err
}

func ctxCancelled(err error) bool {
	return err == context.Canceled || err == context.DeadlineExceeded
}
