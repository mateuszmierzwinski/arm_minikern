# minikern

A small kernel, written from scratch in C, that implements **Linux's syscall
interface** — so a Go binary built for `linux/arm` runs on it unchanged. It copies
no Linux code: no driver, header or algorithm is vendored from the Linux tree.
What it reuses are *interface facts* — syscall numbers, the register calling
convention, the structs user space sees, the process start state, the errno values.

The board is the FriendlyElec **NanoPi NEO**: Allwinner H3, quad Cortex-A7, 512 MiB.
Everything below is proven in QEMU's `orangepi-pc`, which is the same chip; the real
board has not been tried yet (see [Real hardware](#real-hardware)).

## What runs today

In QEMU it boots, brings up the network with its own driver and its own IPv4 stack,
and runs a **standard gc Go 1.26 binary** — or a TinyGo one — as `/init`: goroutines,
threads, timers, garbage collection, signals and async preemption included. A Go
program on it can use the `net` package over UDP and TCP, serve and speak HTTP, and
**fetch its own root filesystem over the network** with this project's own protocol
([`NETBOOT.md`](NETBOOT.md)) — accepting only offers signed by the key it was told
to trust, so the first machine to answer the sweep decides nothing. The Ed25519
verification is written from scratch here too, with no signing in it: the private
half stays on the server.

```
minikern 1.0.0 (NanoPi NEO / Allwinner H3), machine 0
cpu: midr 410fc075, entered in hyp mode, timer 62.500 MHz
boot: info atags at 40000100
mem: 512 MiB at 40000000
initrd: 1648640 bytes at 48000000
net: eth0 02:64:b7:00:81:11, phy 0044:1400 at 1, link up 100M full duplex
minikern: boot complete
minikern: started /init as pid 1
hello from gc Go on minikern, pid 1
this line went to stderr
slept true
map entries: 1000
minikern: init exited with status 7
net: eth0 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3 (dhcp, lease 86400 s)
net: 10.0.2.2 answered ping seq 1
```

It runs on **all four cores**, with no libc and no libgcc. `memcpy`, 64-bit division,
the formatter, the allocators, the TAR reader, the Ethernet driver, the IPv4 stack,
the TCP state machine, the NBD client, the XFS reader and the pipes are all in this
tree.

A Go program can also **run another program**: `os/exec` works, with pipes, exit
statuses and the environment. And Go sees four CPUs, because there are four — one
lock around the kernel preserves what a single core guaranteed, and the parallelism
it buys is in user space, where a Go program spends its time.

**What it is not.** There is no shell and no package of userland tools; `/init` is
the program you build. One core's worth of kernel runs at a time (that lock), there
is no IPv6, no DHCP renewal, and TCP has no congestion control, SACK or reassembly.
Filesystem writes are narrow on purpose: file data into blocks a file already owns,
and the size that leaves behind — creating a file needs the allocator, which is not
written. `execve` from a multi-threaded process is refused rather than supported.
Frequency scaling is policy-complete but its two register writes have never run on
silicon, so it is off unless asked for. Those limits are listed where they bite:
`src/core/net/tcp.h`, `src/core/xfs/xfs.h`, `CLAUDE.md` §9, `TASKS.md`.

## Build and test

Prerequisites, on Fedora:

```bash
sudo dnf install gcc-arm-linux-gnu binutils-arm-linux-gnu uboot-tools dtc \
                 qemu-system-arm qemu-img libnbd xfsprogs clang tinygo
```

Plus **Bazel 9** (`.bazelversion` pins 9.2.0; bazelisk picks it up). The gc Go
programs use the Go SDK Bazel downloads itself, declared in `MODULE.bazel`, so no
host Go is needed for them. TinyGo is optional — only the TinyGo targets need it, and
it wants a host Go between 1.19 and 1.25, whose path is `TINYGO_GOROOT` in
[`toolchain/paths.bzl`](toolchain/paths.bzl): **edit that line to your own install.**

The image (`minikern.elf`, `.bin` and `.uImage`), the 31 host test targets, and the
20 further ones that need QEMU, `qemu-nbd` or xfsprogs:

```bash
bazel build //:minikern
bazel test  //...
bazel test  //:qemu_tests
```

The QEMU tests shell out to host tools, so they are tagged `manual` and a wildcard
build never runs them — **run `:qemu_tests` by name after any change.** They cover
both boot paths, the deliberate exceptions, a TinyGo and a gc hello-world, the Go ABI
suite, 42 hostile syscalls, the network milestone, Go's sockets, a complete netboot,
`os/exec` running a child end to end, four cores being used with every core's
kernel entries counted, the frequency governor against a ramping fake sensor,
every network test again under ASan and UBSan, our NBD client against `qemu-nbd` and
our NBD server against `nbdinfo`, `nbdcopy` and `qemu-io`, the XFS reader against
filesystems `mkfs.xfs` made, and the board formatting a filesystem over the network
for `xfs_repair` to judge.

Boot it by hand with a Go program as init:

```bash
bazel build //:minikern //:hello_gc_rootfs
qemu-system-arm -M orangepi-pc -nographic -nic user \
    -kernel bazel-bin/minikern.uImage \
    -initrd bazel-bin/hello_gc_rootfs.tar \
    -append "console=uart0"
```

`bazel run //:qemu` does the same for the bare kernel;
Ctrl-A X quits. The guest answers UDP on port 7 (echo). Forward it with
`-nic user,hostfwd=udp:127.0.0.1:5007-:7`, then, from the host:
`exec 3<>/dev/udp/127.0.0.1/5007; printf hi >&3; read -N 2 -t 2 r <&3; echo $r`.

## Kernel command line

| Option | Effect |
|---|---|
| `minikern.init=/path` | program to run as pid 1 (default `/init`) |
| `minikern.halt=poweroff` | power off (PSCI) when init ends or the kernel panics |
| `minikern.strace=1` | log every syscall |
| `minikern.epoch=<seconds>` | wall-clock time at boot (there is no RTC driver) |
| `minikern.selftest=undef\|dabt\|pabt` | take that exception on purpose |
| `minikern.net=off` | leave the network down |
| `minikern.netboot[=<ip>]` | fetch the root filesystem over the network; with an address, probe only that one |
| `minikern.nbd=<ip>[:<port>][/<export>]` | attach a network block device as `/dev/nbd0` (NBD, port 10809 by default) |
| `minikern.netstat=1` | print the network and block-cache counters when init exits |
| `minikern.env.NAME=VALUE` | put `NAME=VALUE` in init's environment |

## Netbooting

The board finds its server by **sweeping the subnet with unicast UDP** on a high port
— multicast is usually blocked — and the first valid reply names the server. The
archive then arrives in windowed blocks and is verified against the SHA-256 the
server promised before anything is mounted. The protocol is
[`NETBOOT.md`](NETBOOT.md); the server is a Go program in this tree.

```bash
bazel build //:netboot_rootfs //server/cmd/mknbserver
mkdir -p /tmp/images && cp bazel-bin/netboot_rootfs.tar /tmp/images/default.tar
bazel run //server/cmd/mknbserver -- -images /tmp/images &
qemu-system-arm -M orangepi-pc -nographic -nic user \
    -kernel bazel-bin/minikern.uImage \
    -append "console=uart0 minikern.netboot"      # no -initrd at all
```

```
netboot: looking for a server on the local subnet (unicast sweep)
netboot: server at 10.0.2.2 offers 1781760 bytes in 1740 blocks
netboot: 1781760 bytes from 10.0.2.2 in 1740 blocks (sha256 eef80950dd4c739c...)
minikern: started /init as pid 1
netbooted init running on gc go1.26.8 arm
```

A board asks for `<mac>-<chip id>.tar`, then `<mac>.tar`, then `default.tar`.

**The digest proves the transfer, not the server.** The first machine to answer the
sweep decides what the board runs, so netboot is for a network you control until the
offer is signed (`NETBOOT.md` §5, `TASKS.md` P5.5). The server does carry the
amplification defence a block protocol needs — a random session token the client
echoes on every request, so a forged source address cannot aim 16 KiB of traffic at
a victim with a 24-byte datagram.

## Storage over the network

The volume is a file on a server, reached over **NBD** — a protocol everything else
already speaks, so each end of ours is tested against a reference implementation
rather than only against the other end:

```bash
bazel build //server/cmd/nbdserver
mkdir -p /tmp/volumes && truncate -s 512M /tmp/volumes/vol.img
bazel run //server/cmd/nbdserver -- -volumes /tmp/volumes &
# then boot with minikern.nbd=<your address>:10809/vol
```

On the board, `/dev/nbd0` is an ordinary file to a Go program: open it, seek it,
read and write it. A Go program can **format** it as XFS
(`userland/internal/xfsmk`) and the kernel can **mount** it:

```go
syscall.Mount("/dev/nbd0", "/mnt", "xfs", syscall.MS_RDONLY, "")
```

XFS was chosen because **Linux can check our work**: `mkfs.xfs` makes the test
images, `xfs_repair -n` says whether what we wrote is still a valid filesystem,
`xfs_logprint` reads the log, and `xfs_db` prints any structure in doubt. That is
the standard the tests hold it to — including one in which the board formats a
filesystem over the network and `xfs_repair` finds nothing wrong with it, and
another in which the board grows a file and four judges agree about the result,
none of them ours:

```
TRANS: tid:0x6d6b7472  #items:1
        INODE: #regs:2   ino:0x63  flags:0x1
        CORE inode:
                magic:IN  mode:0x81a4  ver:3  format:2
                size:0x3ffd4  nblks:0x40  nextents:1
```

That is `xfs_logprint` decoding a transaction the board wrote: the file's new size,
logged before it was written in place. A program on the board can create a file,
write into blocks it does not have yet, and remove it again — and the host then
finds the free-inode and free-block counts exactly where they started, so a leak
of one block would show. The reader still **refuses** anything it does not fully
understand — an unknown feature flag, a dirty log, a
btree-format fork — because refusing is what keeps it non-destructive.
[`XFS.md`](XFS.md) has the measured on-disk facts, the support matrix, and the four
log facts that came out of `xfs_logprint` rather than any document.

## Writing an init

Put a Go `main` package in `userland/cmd/<name>/main.go` with a `go_binary`
(`goos = "linux"`, `goarch = "arm"`, `pure = "on"`), then add a `minikern_rootfs`
naming it in the top-level `BUILD`. Anything under `userland/rootfs/` lands in every
root filesystem. `/dev` and `/proc/self/stat` are synthesized rather than stored.
`userland/cmd/nettest` is the worked example — UDP and TCP, client and server, plus
HTTP over loopback.

## Layout

| Path | What it is |
|---|---|
| `src/lib/` | freestanding helpers with no kernel knowledge: formatter, 64-bit division, `memcpy`, SHA-256 |
| `src/core/` | the portable hexagon — boot info, FDT, allocators, VMAs, ELF, TAR, and the whole network stack. Host-tested, includes nothing from `arch/`, `kernel/` or `board/` |
| `src/arch/arm/` | ARMv7-A: entry, vectors, MMU, page tables, GIC, generic timer, PSCI |
| `src/kernel/` | scheduler, processes, address spaces, signals, futexes, VFS, sockets, the syscall tables |
| `src/board/h3/` | the NanoPi NEO: memory map, UART, EMAC, SID — and `main.c`, the composition root |
| `server/` | the netboot server, in Go, hexagonal |
| `userland/` | the Go programs the kernel runs, including the ABI and socket suites |
| `test/` | host unit tests, the hostile/CVE suites, and the QEMU test scripts |

The dependency rule is that `core` never includes `kernel/`, `arch/` or `board/`, and
only `board/<board>/main.c` names concrete adapters. A port is a struct of function
pointers plus `self`, so a test fakes it with a stack struct — which is why the
network stack, the allocators and the page-table encoding are all tested on the host.

## Real hardware

**The card image is built and the boot chain is tested; what is left needs the board
in hand.** The chain is BROM → U-Boot SPL → mainline U-Boot → `boot.scr` → `bootm`
with U-Boot's own device tree, and `sdboot_qemu_test` boots exactly that — the same
image you would write to a card — rather than using QEMU's `-kernel` shortcut.

Two things only that path shows, and both matter on silicon:

- **The generic timer runs at 24 MHz**, the real H3 frequency, where `-kernel`
  reports 62.5 MHz. A kernel that hard-coded either number would be wrong on one
  path and right on the other; this one reads `CNTFRQ`.
- **U-Boot's PSCI implements `CPU_ON` but refuses `SYSTEM_OFF`.** All four cores come
  up, and the board cannot power itself off — so the kernel reports the refusal and
  idles, which is what it should do.

### Making a card

Fedora 43 ships no armv7 U-Boot any more (`uboot-images-armv7` is gone; only the
armv8 package remains), so build one first — no root required:

```bash
scripts/build_uboot.sh nanopi_neo          # or orangepi_pc, which QEMU emulates
bazel build //:minikern.uImage //:goabi_rootfs
scripts/mksdcard.sh \
    -u ../../../../tools/uboot/nanopi_neo/u-boot-sunxi-with-spl.bin \
    -k bazel-bin/minikern.uImage \
    -r bazel-bin/goabi_rootfs.tar \
    -a "console=uart0" \
    -o /tmp/minikern-sd.img
sudo dd if=/tmp/minikern-sd.img of=/dev/sdX bs=4M conv=fsync   # the card, not a disk
```

Then connect the NEO's 3-pin debug header (UART0, **3.3 V**, 115200 8N1 — not 5 V),
power up, and the banner appears:

```bash
picocom -b 115200 /dev/ttyUSB0
```

The card's layout is U-Boot at 8 KiB (sector 16, where the BROM looks), and one FAT
partition at 1 MiB holding `minikern.uImage`, `rootfs.uImage` and `boot.scr`.

### What the board still has to settle, and the one command for it

Boot the card with `minikern.hwreport=1` and send the report back. It prints every
value this kernel took from the datasheet **beside what the datasheet says it should
be**, so a mismatch is visible to someone reading a serial log who has never seen the
code:

```
hw: midr 410fc075 (datasheet: 410fc075, Cortex-A7 r0p5)
hw: cntfrq 24000000 Hz (datasheet: 24000000 on silicon; QEMU's -kernel path says 62500000)
hw: ccu bus_gate0 00020000 (EMAC gate is bit 17, expected set)
hw: syscon ephy_clk ........ at 01c00030 — the register QEMU does not model
hw: ths sample 0: raw ... -> ..... mC (plausible at room temperature: 20000-45000)
```

Three registers and a pin are all that is left unverified, and they are the ones with
no model to check them against:

- **`EMAC_EPHY_CLK_REG` (`0x01C00030`)**, which selects the internal PHY, powers it
  and sets its MDIO address. On a board this register is the difference between "link
  up" and nothing. (The EMAC's clock gates and resets are *not* in this category —
  they live in the clock block, which QEMU does model, and all four are verified.)
- **The thermal sensor's offset and slope.** QEMU models no sensor, so the constants
  have never been compared with a real temperature. Both `0` and `0xfff` are refused
  as "no sensor", so a wrong reading cannot quietly drive the governor.
- **The PIO pin that switches VDD_CPUX** (`minikern.vddcpux=`), since QEMU models no
  PIO. Without it the operating points above 816 MHz are not offered at all, which is
  the safe direction to be wrong in.

## Documentation

- [`CLAUDE.md`](CLAUDE.md) — the design record: measured hardware facts, the
  architecture and its rules, what "Go binaries run unchanged" actually requires
  (measured, syscall by syscall), and every pitfall this project has already paid for.
- [`NETBOOT.md`](NETBOOT.md) — the MKNB protocol, wire format and threat model.
- [`XFS.md`](XFS.md) — how this kernel reads and writes XFS, what it refuses, and
  the on-disk facts it was built from, measured rather than assumed.
- [`TASKS.md`](TASKS.md) — the phased plan, what is done and what is next.

## License

MIT — see [`LICENSE`](LICENSE).
