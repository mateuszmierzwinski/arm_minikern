// mkfsxfs formats an XFS filesystem on a block device — on minikern that is
// /dev/nbd0, a volume that lives as a file on a server.
//
// It is a user-space program because formatting belongs in user space:
// nothing in the kernel needs to do it, Go is a far better place to get a
// hundred packed fields right, and the result is judged by Linux's own
// xfs_repair rather than by us (XFS.md §1).
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/mateuszmierzwinski/arm_minikern/userland/internal/xfsmk"
)

var (
	device = flag.String("device", "/dev/nbd0", "block device or image file to format")
	force  = flag.Bool("force", false, "format even if the device already holds a filesystem")
	show   = flag.Bool("show", false, "print the layout and write nothing")
	label  = flag.Bool("quiet", false, "print nothing on success")
)

func init() {
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: %s [options]\n\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "Formats an XFS v5 filesystem that minikern can read (XFS.md).\n")
		fmt.Fprintf(os.Stderr, "One allocation group, an internal log, 4 KiB blocks.\n\n")
		fmt.Fprintf(os.Stderr, "Options:\n")
		flag.PrintDefaults()
	}
}

func main() {
	flag.Parse()
	if err := run(); err != nil {
		fmt.Fprintf(os.Stderr, "mkfsxfs: %v\n", err)
		os.Exit(1)
	}
}

// run is the composition root: open the device, plan, and either print the
// plan or carry it out (master Rule 7.3).
func run() error {
	dev, err := openDevice(*device)
	if err != nil {
		return err
	}
	defer func() { _ = dev.Close() }()
	if *show {
		return showLayout(dev)
	}
	if err := refuseExisting(dev); err != nil {
		return err
	}
	layout, err := xfsmk.Format(dev, xfsmk.Defaults(), nil)
	if err != nil {
		return err
	}
	if !*label {
		report(layout)
	}
	return nil
}

func showLayout(dev *fileDevice) error {
	size, err := dev.Size()
	if err != nil {
		return err
	}
	layout, err := xfsmk.Plan(size, xfsmk.Defaults())
	if err != nil {
		return err
	}
	report(layout)
	return nil
}

// report prints the geometry, in the shape xfs_info does: enough for
// somebody to check it against what they expected.
func report(l xfsmk.Layout) {
	fmt.Printf("meta-data=%-24s isize=%-6d agcount=%d, agsize=%d blks\n", *device, l.InodeSize,
		l.AgCount, l.AgBlocks)
	fmt.Printf("data     =%-24s bsize=%-6d blocks=%d, free=%d\n", "", l.BlockSize, l.Blocks,
		l.Fdblocks())
	fmt.Printf("naming   =%-24s version 2, ftype=1\n", "")
	fmt.Printf("log      =internal log           bsize=%-6d blocks=%d, start=%d (group %d)\n",
		l.BlockSize, l.LogBlocks, l.LogStart, l.LogAg)
	fmt.Printf("root     =inode %d, inodes=%d, free=%d\n", l.RootIno, l.Icount(), l.Ifree())
}

// refuseExisting stops a filesystem being thrown away by accident: the
// device is a volume somebody may care about, and -force is how they say
// they do not.
func refuseExisting(dev *fileDevice) error {
	magic := make([]byte, 4)
	if _, err := dev.ReadAt(magic, 0); err != nil {
		return nil // an unreadable start is not evidence of a filesystem
	}
	isXFS := magic[0] == 0x58 && magic[1] == 0x46 && magic[2] == 0x53 && magic[3] == 0x42
	if isXFS && !*force {
		return fmt.Errorf("%s already holds an XFS filesystem (use -force)", *device)
	}
	return nil
}
