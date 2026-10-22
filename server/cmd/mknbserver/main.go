// Command mknbserver serves minikern root filesystems over the netboot
// protocol in NETBOOT.md: it answers a board's unicast discovery probe and
// then sends the tar archive in windowed blocks, verified by the digest it
// promised.
//
// It is the composition root (master Rule 7.3): the only place concrete
// adapters are named.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/mateuszmierzwinski/arm_minikern/server/internal/adapters/imagestore"
	"github.com/mateuszmierzwinski/arm_minikern/server/internal/adapters/udptransport"
	"github.com/mateuszmierzwinski/arm_minikern/server/internal/mknb"
)

var (
	addr        = flag.String("addr", fmt.Sprintf(":%d", mknb.Port), "UDP address to serve on")
	images      = flag.String("images", "", "directory of .tar images (required)")
	maxSessions = flag.Int("max-sessions", mknb.DefaultMaxSessions, "concurrent transfers to allow")
	quiet       = flag.Bool("quiet", false, "do not log each offer and transfer")
	showVersion = flag.Bool("version", false, "print the version and exit")
	readyFile   = flag.String("ready-file", "", "write the bound address here once serving (for tests)")
)

func init() {
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: %s -images <dir> [options]\n\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "minikern netboot server (MKNB v%d).\n\n", mknb.Version)
		fmt.Fprintf(os.Stderr, "A board asks for <mac>-<chip id>.tar, then <mac>.tar, then default.tar.\n\n")
		fmt.Fprintf(os.Stderr, "Options:\n")
		flag.PrintDefaults()
	}
}

// systemClock is the real clock.
type systemClock struct{}

func (systemClock) Now() time.Time { return time.Now() }

// discardLogger drops the service's per-transfer lines; the start-up line
// and the closing summary are the command's own and still print.
type discardLogger struct{}

func (discardLogger) Printf(string, ...any) {}

// chooseLogger returns the logger the flags asked for.
func chooseLogger() mknb.Logger {
	if *quiet {
		return discardLogger{}
	}
	return log.New(os.Stderr, "mknbserver: ", log.LstdFlags)
}

// announce writes the bound address where a test can read it.
func announce(path, text string) error {
	if path == "" {
		return nil
	}
	return os.WriteFile(path, []byte(text+"\n"), 0o600)
}

func main() {
	flag.Parse()
	if *showVersion {
		fmt.Printf("mknbserver %s\n", version())
		return
	}
	if *images == "" {
		fmt.Fprintln(os.Stderr, "error: -images is required")
		flag.Usage()
		os.Exit(1)
	}
	if err := run(); err != nil {
		log.Fatalf("mknbserver: %v", err)
	}
}

// run opens the socket and serves until a signal arrives.
func run() error {
	info, err := os.Stat(*images)
	if err != nil || !info.IsDir() {
		return fmt.Errorf("-images %q is not a directory", *images)
	}
	conn, err := udptransport.Listen(*addr)
	if err != nil {
		return err
	}
	defer conn.Close()
	logger := chooseLogger()
	service := mknb.NewService(imagestore.New(*images), conn, systemClock{}, mknb.RandomTokens{}, logger,
		mknb.Config{MaxSessions: *maxSessions})
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	log.Printf("mknbserver %s serving %s on %s", version(), *images, conn.LocalAddr())
	if err := announce(*readyFile, conn.LocalAddr().String()); err != nil {
		return err
	}
	if err := conn.Serve(ctx, service); err != nil && ctx.Err() == nil {
		return err
	}
	s := service.Stats()
	log.Printf("stopped: %d probes, %d offers, %d blocks, %d refused", s.Discovers, s.Offers, s.Blocks,
		s.BadToken+s.Busy+s.NoImage)
	return nil
}
