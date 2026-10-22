# minikern — tasks

The phased plan. Each task ends with a test that proves it, and each phase ends with
something that boots. The background is in `CLAUDE.md`: hardware facts in §3, the Go
requirements in §7. Mark a task done with `[x]` and the date. Add a line on anything
it taught us.

Legend: `[x]` done · `[ ]` to do · `[~]` partial

---

## The order, and why (operator, 2026-10-22)

1. **P0.9, the real NEO, before anything concurrent.** SMP bugs are memory-ordering
   bugs, and QEMU's TCG reproduces neither Cortex-A7 reordering, nor caches, nor real
   interrupt timing; writing SMP before the board has booted is debugging two
   unknowns at once. **Operator-blocked**: it needs the board, U-Boot installed (root)
   and the serial adapter.
2. **The cheap wins** — P7.5 DNS, P8.9 `clock_settime`, P8.10 a writable `/tmp`,
   P8.11 a watchdog. Each is small and removes a reason this is awkward to use.
3. **P9, XFS over NBD.**
4. **P10, SMP**, stage 1 only, until profiling argues for more.
5. **P11, thermal before frequency scaling.** On the H3, DVFS is a thermal tool
   rather than a power one.
6. **P5.5, signing the netboot offer**: the gate between a lab toy and something
   that could be deployed.
7. **P8.1, `fork`/`execve`/`wait4`**, and with them a test that runs Go's `os/exec`
   on the board — the last of the named extras, because every other item above is
   independent of it.

---

## P0 — Boot (it starts, it talks, it knows where it is)

- [x] **P0.1 Toolchain and image (2026-10-21).** A project-local platform plus a
      `cc_toolchain` over `arm-linux-gnu-gcc`. The `kernel_image` rule produces
      `.elf`, `.bin` and `.uImage`. The host wildcard skips H3-only targets.
- [x] **P0.2 Entry (2026-10-21).** `start.S`: HYP→SVC, stack, BSS, `kmain`, with the
      entry mode reported.
- [x] **P0.3 Console (2026-10-21).** `console_port` over UART0, plus `kprintf` (`kfmt`,
      host-tested against libc's `snprintf`).
- [x] **P0.4 Boot info (2026-10-21).** Classifies r2 as ATAGs, FDT or none. Reads
      ATAGs (bounded and hostile-input tested), clamps memory to 512 MiB, and reads the
      command line.
- [x] **P0.5 Power and the boot test (2026-10-21).** PSCI SYSTEM_OFF on
      `minikern.halt=poweroff`. `:boot_qemu_test` checks the boot log and that QEMU
      exits by itself.
- [x] **P0.6 Device tree (2026-10-21).** `core/fdt.c`: bounded header checks, a
      token walker with a depth limit, `/memory` `reg` honouring the root's cell sizes,
      `/chosen` `bootargs` and `linux,initrd-*`. `fdt_test` covers truncation at every
      length, hostile lengths, unbalanced and over-deep trees. `boot_qemu_test` boots
      with `-dtb` from `dts/nanopi-neo.dts`. *Learned:* QEMU fills `/chosen` itself and
      puts the blob just after the initrd.
- [x] **P0.7 Exceptions and panic (2026-10-21).** `vectors.S` saves every trap on the
      SVC stack with `srsdb`. Kernel faults dump the registers and panic. Under
      `minikern.selftest=undef|dabt|pabt`, the boot test asserts all three.
- [x] **P0.8 Own the UART (2026-10-22).** UART0 is programmed here: its clock gate
      and reset in the CCU, the divisor for 115200 from the 24 MHz APB2 clock
      (13 — 0.16% out, where a UART tolerates about 2%), 8N1 and both FIFOs on.
      Anything still in the shift register is allowed to leave first, or the last
      line of whoever ran before us becomes rubbish on the wire.
      Nothing *needed* this: U-Boot leaves the port configured and QEMU needs no
      setup at all. That is the problem it solves — a kernel that depends on
      whoever ran before it having set the console up has no console the first
      time something else boots it, and a silent board is the hardest kind to
      look at. It is also self-checking: a wrong divisor, gate or frame makes the
      console go silent, which every test in the project notices at once.
- [~] **P0.9 Real hardware — the card and the chain are done and tested; the banner
      on the NEO needs the board (2026-10-22).** Everything this task asks for that
      does not require holding the hardware is finished, including the part it offered
      as optional, which turned out to be the valuable half.
      **Done and proved:** `scripts/mksdcard.sh` builds the card — U-Boot at 8 KiB
      (sector 16, where the BROM looks), one FAT partition at 1 MiB holding
      `minikern.uImage`, `rootfs.uImage` and a `boot.scr` that runs `bootm` with
      `${fdtcontroladdr}` — and `sdboot_qemu_test` boots **that image**, not a
      `-kernel` shortcut: BROM → SPL → U-Boot → `boot.scr` → `bootm` → the kernel
      reading U-Boot's own device tree → a Go program out of the ramdisk. The flash
      procedure is in `README.md`, and the image that boots in QEMU is byte for byte
      the one that would be written to a card.
      **Two facts only this path could show**, both now in CLAUDE.md §3:
      the generic timer reports **24.000 MHz** here — the real H3 frequency — where
      the `-kernel` path reports 62.5 MHz, so either number hard-coded would be wrong
      on one path and nobody would have noticed; and U-Boot's PSCI implements `CPU_ON`
      (four cores up) but **refuses `SYSTEM_OFF`**, so a board booted from a card
      cannot power itself off — the kernel reports the refusal and idles, which is why
      this test waits for a line rather than for QEMU to exit.
      **A detour worth recording:** Fedora 43 ships no armv7 U-Boot at all
      (`uboot-images-armv7` is gone, only `uboot-images-armv8` remains), and packages
      need root. `scripts/build_uboot.sh` builds it as a per-machine host tool into
      the gitignored `tools/uboot/` — the convention Rule 29 uses for the scanners —
      and carries the three config knobs modern Fedora forces off
      (`TOOLS_LIBCRYPTO`/`TOOLS_KWBIMAGE`: no `openssl/engine.h`;
      `TOOLS_MKEFICAPSULE`: no gnutls) plus the assembly `binman` can no longer do
      (setuptools 81 dropped `pkg_resources`). No U-Boot code enters this repository.
      **What is left, and why it cannot be done here:** the banner on the NEO's debug
      header, and with it the EMAC's clock/reset/PHY-select bits, the thermal sensor
      and the PIO pin for VDD_CPUX — QEMU models none of them, so
      `minikern.cpufreq=on` has still never run on silicon. That needs the board, a
      3.3 V serial adapter and root to write a card.
- [x] **P0.10 Version stamp (2026-10-22).** The build part of `MAJOR.MINOR.BUILD`
      is Bazel's own build timestamp, written into a generated C file by the
      `build_stamp` rule; the series comes from the monorepo's one constant as
      before. An ordinary build prints `1.0.0` and a stamped one `1.0.1792665133`.
      *Deviation from master Rule 28.1, which says `--stamp`:* the Starlark call
      that tells a rule whether `--stamp` was passed is **private API**, so the
      opt-in is a build setting of our own with the same shape and default —
      `bazel build --//:stamp`. The reason the rule
      gives still holds: an unstamped build says 0 rather than printing a number
      that means nothing. The stamp file is an input only when stamping, so an
      ordinary build does not relink the kernel for a number it may not use.

## P1 — Network up (DHCP lease, answers ping) — done in QEMU (2026-10-21)

- [x] **P1.1 Timer (2026-10-21, with P2).** The virtual generic timer: CNTVCT for the
      monotonic clock, CVAL for a drift-free 1 kHz tick, scaled from CNTFRQ.
- [x] **P1.2 GIC and IRQs (2026-10-21, with P2).** `arch/arm/gic.c` (GIC-400) and
      `kernel/irq.c`, with one handler struct per interrupt ID. The timer (PPI 27) and the
      UART receive line (SPI 0) are wired.
- [x] **P1.3 Clocks and resets for the EMAC (2026-10-21).** `board/h3/emac.c`
      `power_on`: the CCU bus gates and resets for the EMAC and the EPHY, plus
      `EMAC_EPHY_CLK_REG` (internal PHY, address 1, 24 MHz, powered). *Unverified on
      silicon* (QEMU ignores them): check at P0.9.
- [x] **P1.4 MDIO and link (2026-10-21).** Clause-22 MDIO, PHY ID, reset +
      autonegotiation, link wait, speed/duplex from the partner's abilities:
      `eth0: ... phy 0044:1400 at 1, link up 100M full duplex`.
- [x] **P1.5 EMAC driver (2026-10-21).** Chained rings (32 RX, 8 TX) in an uncached
      1 MiB DMA window (`kernel/dma`), interrupt-driven receive, the MAC from the chip
      ID (`board/h3/sid.c`: PRCTL/RDKEY), as a `netdev_port`.
- [x] **P1.6–P1.8 The stack (2026-10-21).** `core/net`: Ethernet, ARP (no unsolicited
      replies), IPv4 (fragments and martians dropped), ICMP echo (rate-limited, no
      broadcast replies), UDP (checksummed, one handler per port), the DHCP client
      (xid + MAC matching, bounded options, backoff, NAK). `net_test`, with 17 cases
      including 200 000 mutated frames, also runs under ASan and UBSan
      (`net_sanitized_test`).
- [x] **P1.9 Network boot test (2026-10-21).** `net_qemu_test`: link up, the slirp
      lease `10.0.2.15/24 gw 10.0.2.2`, three pings answered by the gateway, and a host
      datagram echoed back through `hostfwd` to the guest's port 7. **The milestone:
      "booting and bringing up network".**
- [x] **P1.10 CVE-class review (2026-10-21).** Against the classes that hit other small
      stacks (Ripple20, Amnesia:33, INFRA:HALT: length-field confusion, fragment
      reassembly, option overruns), plus ARP poisoning, ping floods, smurf, fraggle,
      echo/chargen ping-pong, LAND/martian sources. Each mitigation has a test in
      `net_test`. There is no TCP, DNS or fragment reassembly, so their classes have no
      surface yet: re-run this review when they arrive.
- [x] **P1.11 Lease renewal (2026-10-22).** A lease is a loan with a term, and the
      client now follows its clock: at **T1** — half the lease — it asks the server
      that granted it, directly, from its own address and with the address in
      `ciaddr` rather than as a requested-address option (which is what tells a
      server this is an extension, RFC 2131 §4.3.6); at **T2** it stops expecting
      that server and asks the subnet; and when the lease runs out it **gives the
      address up**, because an address past its lease is someone else's to hand
      out and keeping it is a conflict rather than a convenience. A NAK of a
      renewal drops it too.
      Five host cases over a controlled clock, including that an answered renewal
      puts the clock back without the bound callback firing again. *Learned:* a
      unicast renewal needs the server's hardware address first, so the ARP
      exchange comes before the renewal on the wire — which the test has to answer
      for the renewal to leave at all. And `dhcp_input` had to accept an ACK while
      renewing: without that the client renews until the lease expires underneath
      it.
      Slirp's lease is a day long, so QEMU never exercises this; the host
      cases are where it is proved, and a real LAN is where it matters.
- [x] **P1.12 TCP (2026-10-21, with P7).** See P7.3.

## P2 — Kernel core — done (2026-10-21)

- [x] P2.1 MMU: high-half kernel (TTBR1) + per-process user tables (TTBR0, TTBCR.N=1),
      normal WBWA memory, device memory for I/O, XN everywhere except text.
- [x] P2.2 `core/pmm` (bitmap, aligned runs) and `core/kheap` (size classes, misuse
      detection), both host-tested; `kernel/mem` wires them.
- [x] P2.3 Address spaces: `core/vma` regions, lazy anonymous pages, `mmap`/`munmap`/
      `mprotect`/`madvise`/`brk`/`mincore`, checked user copies.
- [x] P2.4 Processes and threads: user entry and exit through one trapframe, VFP
      enable + eager save/restore per thread, TPIDRURO per thread.
- [x] P2.5 Scheduler: round-robin, preemptive on a 10 ms slice; sleep channels with
      deadlines and signal interruption.

## P3 — The Linux syscall interface (TinyGo runs) — done (2026-10-21)

- [x] P3.1 ELF loader (`core/elf`, host-tested with hostile headers) and the start
      stack (`core/ustack`): auxv with `HWCAP_TLS`, `AT_RANDOM`, `AT_PLATFORM` "v7l".
- [x] P3.2 Syscall dispatch: per-domain tables, `-ENOSYS` logged once, `minikern.strace=1`.
- [x] P3.3 The TinyGo set and far more (CLAUDE.md §7): ~85 calls across processes,
      memory, time, signals, futexes, files, directories, epoll/eventfd.
- [x] P3.4 File descriptors and consoles: fds 0–2 on the console, devfs `console`,
      `tty`, `ttyS0`, `null`, `zero`, `full`, `random`, `urandom`, `stdin`/`stdout`/
      `stderr` (aliases for the fds); console input through the UART receive IRQ.
- [x] P3.5 Milestone: `tinygo_qemu_test`.

## P4 — TAR root filesystem and /init — done (2026-10-21)

- [x] P4.1 `core/tar` (checksums, bounds, PAX skipping, tested with GNU tar's output)
      and `kernel/vfs`: tarfs at `/`, devfs at `/dev`, procfs `/proc/self/stat`
      (bdwgc reads field 28), `getdents64` including the two mounts.
- [x] P4.2 `/init` from Go source by Bazel (rules_go and TinyGo), packed by
      `minikern_rootfs` (`pkg_tar`).
- [x] P4.3 Initrd delivery through ATAG_INITRD2 or FDT `/chosen`.

## P5 — Netboot: own discovery and transfer — done in QEMU (2026-10-21)

- [x] **P5.1 Protocol (NETBOOT.md).** MKNB v1: a paced unicast sweep for discovery
      (the operator's design: multicast is usually blocked), an OFFER naming the
      image's length, block size and SHA-256, a windowed block fetch, and a session
      token that keeps the server from being a UDP amplifier.
- [x] **P5.2 The server**, `server/`: Go, hexagonal, standard library only. 10 CVE
      cases (amplification, traversal, exhaustion, disclosure, fuzzing) plus unit
      tests and benchmarks; three Bazel test targets.
- [x] **P5.3 The client**, `core/net/netboot.c` (pure, 22 host tests incl. ASan) and
      `kernel/netboot.c`. Sweeps, fetches, verifies the digest, mounts, runs `/init`;
      abandons a server that offers and then stalls, and falls back to the initrd.
- [x] **P5.4 End-to-end test**, `netboot_qemu_test`: the real server on the host, the
      board with no initrd at all, a Go init running from the fetched archive.
- [~] **P5.5 Who decides what the board runs.**
      - [x] **A pinned root digest (2026-10-22).** `minikern.rootfs.sha256=<hex>`
            accepts only the image with that digest, and refuses every other
            **at the offer** — so a machine that answers the sweep first cannot
            substitute an image *or* deny service: the board refuses it and goes
            on sweeping. Proved both ways on the board: with the right pin the
            ordinary path is unchanged (8 offers, 1740 blocks), with a wrong one
            763 probes bring 0 offers accepted and 7 refused, and it falls back to
            the initrd. A pin that will not parse **stops the boot** rather than
            being ignored, because a boot that looked protected and was not would
            be worse than no pin.
      - [x] **An Ed25519 signature over the offer (2026-10-22).** The board
            accepts only offers signed by the holder of the key it was given
            (`minikern.netboot.key=`), which is what a pin cannot do: it
            authenticates the **server**, so the server may change what it serves
            and nothing else on the subnet can make an offer the board will look
            at. The signature covers the session's own id, so a recorded offer
            cannot be replayed into a later boot. Proved by booting from a server
            that signs, and then by the same server with signing turned off —
            offering the *right* image and refused anyway, because of who was
            offering it.
            **Written from scratch, as everything here is** (CLAUDE.md §2): SHA-512
            (constants generated, not transcribed), the field of 2^255-19 in eight
            32-bit limbs with schoolbook multiplication, and the curve. Plain
            rather than quick on purpose — this verifies one signature per boot,
            so the fast representations with their delayed reductions would buy
            nothing and cost the only thing that matters, being able to read it.
            Nothing is constant-time and nothing needs to be: a key, a signature
            and a message are all public, and **there is no signing here** — the
            private half stays on the machine that signs.
            Checked against the library that will sign in production: 47 vectors
            from Go's `crypto/ed25519`, every single-bit flip of a 64-byte
            signature, and the field arithmetic against Python's arbitrary
            precision at p-1, p-2, (p-1)/2 and 2^254.
- [x] **P5.6 Two guests on a segment of their own (2026-10-22).** One board runs
      the server — the same server the host runs, from the same packages,
      cross-compiled and serving an image out of its own root filesystem — and the
      other netboots from it with **no initrd at all**, so the only way it can run
      a program is that the program arrived over the wire. It does: 1648640 bytes
      in 1610 blocks, and then `hello from gc Go on minikern`.
      This is what every other netboot test could not prove. The emulator's
      address translation answers probes for addresses nobody owns and invents
      hosts (CLAUDE.md §10), so "it found the server" has always meant "it found
      the host"; here nothing on the host takes part in the conversation.
      *Two things it needed, both of which are features now.* A **static address**
      (`minikern.ip=`), because a segment with two machines on it has no DHCP
      server — and a board on a network without one is not a strange case. And a
      **hardware address on the command line** (`minikern.mac=`), because QEMU
      gives every machine the same chip serial number and the address is derived
      from it: two guests shared one address and neither could talk to the other,
      which cost 250 probes to notice.
      *And one thing that was not:* QEMU's multicast backend needs a host route
      for the group, so two guests joined to one never see each other's frames. A
      `listen`/`connect` socket pair is the same thing a cable between two boards
      is, and needs nothing of the host but a port.

## P6 — Full Go (gc toolchain) — done on one core (2026-10-21)

- [x] Threads (`clone` with the thread flag set; SETTLS, PARENT_SETTID,
      CHILD_CLEARTID), `futex` (WAIT/WAKE and their BITSET forms, both timespec
      widths), signal delivery with the ARM `rt_sigframe`, `sigaltstack`,
      `rt_sigreturn`, a sigpage trampoline, `tgkill`, `nanosleep`, `sched_yield`,
      epoll + eventfd for the netpoller. `go_abi_qemu_test` covers it.
- [x] SMP across the H3's four cores — done in P10, with the lock it needed.

## P7 — Sockets for Go's `net` — done for UDP and TCP (2026-10-21)

- [x] **P7.1 The socket layer**, `kernel/socket.h`: a socket is a file whose ops
      forward to a per-protocol table (`sock_udp.c`, `sock_tcp.c`), with 15 syscalls
      in `sys_socket.c`.
- [x] **P7.2 UDP sockets**: a 16-datagram queue per socket, connected filtering,
      `MSG_PEEK`, ephemeral binding, `recvfrom`/`sendto`/`recvmsg`/`sendmsg`.
- [x] **P7.3 TCP** (`core/net/tcp.h`): the state machine with 16 host tests over a
      lossy, duplicating, reordering wire, and 12 hostile-input cases.
- [x] **P7.4 The milestone**, `net_sockets_qemu_test`: Go's `net` over UDP and TCP in
      both directions, including a full HTTP client and server inside the guest.
- [x] **P7.5 DNS (2026-10-22).** No kernel resolver: `/etc/resolv.conf` and
      `/etc/hosts` are generated from the DHCP lease (`kernel/genfs.c`) and Go's own
      pure-Go resolver does the rest over the UDP sockets it already has.
      `minikern.dns=<ip>` overrides the lease, and survives it. Proved in
      `net_sockets_qemu_test` against a one-name DNS server of ours
      (`test/tools/dnsecho`), so the check is about the kernel rather than about the
      internet. *Learned:* QEMU's `guestfwd` is TCP-only, so the guest cannot be
      pointed at a host program on port 53 without root — the test aims the query
      with a `Dial` of its own and checks the generated file separately.
- [x] **P7.6 `poll`, `select` and the socket timeouts (2026-10-22).** Go uses none
      of them — its netpoller is epoll, which is why epoll came first — so these
      are for everything else, and they are tested by calling them raw from Go.
      `poll` and `select` share one implementation of "what is ready, and how long
      will you wait", because two would drift; `select`'s bitmaps are turned into
      pollfds and back. A descriptor that is not open is `POLLNVAL` on that entry
      rather than an error for the whole call.
      `SO_RCVTIMEO` and `SO_SNDTIMEO` reach every blocking path — receive, send
      and **accept** — and a timeout answers `EAGAIN`, as Linux does. A partial
      send that then times out reports what it sent, because a caller told zero
      will send those bytes twice.
      *Learned in the test:* Go opens every socket non-blocking, so a read on one
      returns EAGAIN immediately and would have passed a timeout test without the
      kernel ever waiting. The test makes a blocking socket with raw syscalls,
      which is the only kind the option means anything for.

## P8 — Hardening and gaps (found on the way)

- [x] **P8.1 `fork`/`execve`/`wait4`** — done below, with the `os/exec` test it
      asked for.
- [x] **TCP congestion control and reassembly (2026-10-22).** Two gaps, and they
      were the same gap twice: the stack trusted the path.
      **Congestion control** in Reno's shape: a window that starts at ten
      segments (RFC 6928 — one was chosen when a round trip cost a second),
      doubles each round trip while below the threshold, then grows by a segment
      per round trip; three duplicate acknowledgements mean one segment is
      missing and the rest arrived, so it is resent at once and the window is
      halved rather than abandoned; a **timeout** is the strong signal and takes
      the window back to one segment. Sending whatever the peer's window allowed
      is right on a cable with nothing else on it and, anywhere else, means
      filling a queue until it drops and then doing it again.
      **Reassembly**: a segment that arrives early is now held — four of them per
      connection — until the gap before it is filled, and filling one gap drains
      everything that has become contiguous. A fifth is dropped, which is exactly
      the old behaviour, so the limit degrades into what it replaces rather than
      into a failure. The stack counts what it held, and the test asserts the
      count rather than trusting that reordering happened.
      **SACK is deliberately left out**, and the reason is worth keeping: it is an
      improvement on reassembly rather than a substitute for it. Without the queue
      there is nothing for a receiver to acknowledge selectively; with it, a loss
      already costs one round trip instead of a window's worth.
- [x] **The DMA window's cache state (2026-10-22).** `page_alloc` zeroes through the
      cacheable linear map, so handing that memory to a device left dirty lines to be
      written back over a packet later. It is cleaned and invalidated before the
      attributes change, and the cacheable alias unmapped so no page is reachable
      under two cacheabilities. `minikern.selftest=dma-alias` proves the alias faults.
      *Learned:* a QEMU-only test suite cannot see a cache bug at all (CLAUDE.md §10).
- [x] **An RTC driver (2026-10-22).** The board's own clock gives the wall time at
      boot, so a kernel no longer starts in 1970 and a Go program's
      `time.Now().Year()` is the current one — which is what the ABI suite now
      asserts, before it goes on to set the clock.
      **QEMU models this device** (`allwinner-rtc-sun6i`), so the register layout
      was measured rather than assumed: a selftest printed the two registers raw
      and they matched the host's clock to the second, with the year counted from
      1970 and the weekday in the top bits. The calendar arithmetic is pure and
      host-tested — the epoch, a leap day, the end of a leap year, 2100 (which is
      not a leap year), and a day-by-day sweep from 1999 to 2101 where every day
      must be 86400 seconds after the one before.
      An unset clock reads as zeroes and is **refused** by the conversion rather
      than turned into a plausible-looking lie; `minikern.epoch=` still wins when
      given, because an operator who says what time it is has a reason, and
      because that is how the tests pin one.
- [x] **P8.9 `clock_settime` (2026-10-22).** `settimeofday`, `clock_settime` and
      `clock_settime64` set the wall clock; the monotonic clock is the generic timer
      and cannot be moved, which the test checks. An NTP client in Go can now fix
      the time on a board with no battery.
- [x] **P8.10 A writable `/tmp` (2026-10-22).** `kernel/tmpfs.c`: sixteen flat
      files of at most 256 KiB out of the kernel heap, with create, write, grow,
      list, unlink and unlink-while-open. `unlink`/`unlinkat` refuse everywhere
      else, because everywhere else is read-only. The ABI suite exercises it
      through `os.CreateTemp`, `os.ReadDir` and `os.Remove`.
- [x] **P8.11 A watchdog (2026-10-22).** `minikern.watchdog=<seconds>` reboots the
      board if the kernel stops ticking for that long, which is the one failure
      nothing else here can report: a kernel that has stopped taking interrupts
      cannot print, cannot panic and cannot be asked anything — and three of
      P10.3's bugs looked exactly like that. **QEMU models this device**
      (`allwinner-wdt-sun6i`), unlike the thermal sensor and the PLLs, so it is
      tested in both directions rather than hoped for: `minikern.selftest=wedge`
      masks interrupts and spins, and the board reboots (9 times in 20 s), while a
      healthy boot with the watchdog on runs to its power-off untouched. The
      second half is what decides whether a watchdog stays switched on, so it is
      not an afterthought.
      It is fed from the tick and is no cleverer than that on purpose: a watchdog
      that tried to judge whether work was *progressing* would reboot an idle
      board, which is how watchdogs get turned off. `minikern.hangcheck` remains
      the software half, for the case where the tick is still arriving.
- [x] **A real entropy source: the times at which interrupts arrive (2026-10-22).**
      The generator was a good sequence from a bad seed — the timer and the chip's
      serial number — and QEMU gives every machine the same serial number and starts
      the timer at zero, so **two boots produced the same bytes**. Fine for the hash
      seeds Go asks for; not fine for a key, which matters in a tree that verifies
      signatures.
      The kernel now harvests what it actually has: the cycle counter read at every
      interrupt, mixed into a pool a word at a time on the interrupt path and read out
      through SHA-256 in counter mode, so a read neither hands out the pool nor repeats
      itself. `getrandom` draws from the pool once it has had its fill of samples
      (128, which a boot passes in its first moments) and mixes it into the generator
      before then.
      **The H3 crypto engine was the other candidate and is not usable**: QEMU models
      no crypto engine, so its register sequence could be written but never run — and
      an unrunnable driver for the one thing a key depends on is worse than none.
      *What the test proves and what it does not.* `entropy_qemu_test` boots the same
      image twice and requires the bytes to differ, which is the mechanism working. It
      does not prove the bytes are unpredictable, and does not claim to: under an
      emulator the jitter follows the host's scheduler, and on real hardware it is a
      packet arriving from another machine and a clock that is not ours. One run can
      never tell those apart, which is exactly how a test comes to bless a constant.
- [x] **`mprotect(PROT_NONE)` keeps the pages (2026-10-22).** The page table can
      say "mapped, and user space may not touch it" (AP = privileged only), so a
      program that protects a region and unprotects it finds its own bytes rather
      than zeroes. Dropping them was a different program's behaviour. Go does not
      depend on it, which is exactly why it has a test of its own.
- [x] **Out-of-memory policy (2026-10-22).** Two halves, and a test for both:
      memory is handed out **optimistically** — a Go runtime reserves half a
      gigabyte of address space it never touches, so refusing by size would refuse
      every Go program — and the process that finally cannot be given a page is
      **killed with SIGKILL**, with the reason printed once.
      It used to get SIGSEGV, which says the wrong thing: a runtime that catches
      those (Go's does) reports "invalid memory address" and the real cause is
      lost. On the board, a program reaches 504 MiB of 512 and is then killed by
      signal 9.
- [x] **Relative `..` in paths, and symlinks with relative targets
      (2026-10-22).** `..` is resolved against what came before it, which is what
      makes a relative path usable — and doing it *before* anything looks at the
      name is what keeps it from being a way out of the root: `..` at the top has
      nothing to remove, so it folds away. A symlink with a relative target is now
      resolved against the link's own directory rather than refused, with the
      depth **bounded** (a link to itself is the easy loop, two links to each
      other the next). Every root filesystem carries one link of each kind, so the
      resolver always has something to resolve, and the ABI suite reads a file
      through both.
- [x] **The floating-point state moves only when it is used (2026-10-22).** The
      unit is switched **off** for the thread being switched to, and the first
      floating-point instruction it runs traps as undefined — which is where the
      previous owner's registers are saved, this thread's restored, and the
      instruction retried. A thread that never does arithmetic (a goroutine on a
      channel, the netpoller) never pays for thirty-two registers.
      The kernel reports both numbers so the claim can be checked rather than
      believed, and the SMP test asserts the relation: **7 moves in 67 switches**.
      *Two things it cost:* saving the old state before switching the unit on
      traps again, in kernel mode, where an undefined instruction is a panic; and
      signal delivery assumed the registers are always live in the hardware, which
      was true when every switch restored them and became a way to read whichever
      thread last did arithmetic.

---

## P9 — XFS over the network (NBD)

The volume is a file on the server and the board mounts it over the network. XFS is
the right choice here **because Linux can check our work**: `mkfs.xfs` makes the test
images, `xfs_repair -n` judges whether what we wrote is still a valid filesystem, and
`xfs_db` inspects any structure. The hard rule, in CI: **`xfs_repair -n` is clean
after every test.**

- [x] **P9.1 The block layer (2026-10-22).** An NBD client over our own TCP (newstyle fixed
      handshake; `NBD_CMD_READ`/`WRITE`/`FLUSH`/`DISC`), a buffer cache over it, and
      `/dev/nbd0` so a userland program can reach the volume. The server is Go,
      serving an image file. Tested both ways round — our client against `qemu-nbd`
      as a reference server, and our server against a reference client — so neither
      end is only ever checked by the other. *Done:* `core/nbd.c` (20 hostile
      cases), `core/bcache.c` (13), the Go server (8 targets incl. its CVE
      suite), both live tests, and `nbd_qemu_test` end to end with the host
      verifying the image byte for byte. *Learned:* a refused export must
      leave the handshake open, which only a reference client revealed; and
      anything dialling out at boot must wait for the DHCP lease first.
- [x] **P9.2 Read-only mount (2026-10-22).** v5 images from Linux `mkfs.xfs` with a minimal
      feature set: `crc` and `finobt` on, `reflink` and `rmap` off. Superblock and AG
      headers, the inode btree, extent-format files, shortform and block directories.
      **Refuse to mount a dirty log, or any unsupported feature flag.** That refusal
      is what keeps it non-destructive.
- [x] **P9.3 mkfs, in Go, in userland (2026-10-22)**, writing to `/dev/nbd0`.
      Formatting does not belong in the kernel, it is far easier to write in Go, and
      `xfs_repair` validates the result. *Done:* `userland/internal/xfsmk` (the same
      package formats a file on the build machine and `/dev/nbd0` on the board),
      `mkfsxfs`, a `mount` syscall and `kernel/xfsfs.c` so a Go program can mount
      what it wrote, and `xfs_qemu_test`: the board formats a 512 MiB filesystem
      over the network, mounts it, reads it, and **xfs_repair on the host finds
      nothing wrong with it**. *Learned:* five numbers came from xfs_repair's own
      complaints rather than from any document (XFS.md §7) — among them that the log
      record header's cycle-data array is 64 entries, not 56.
- [~] **P9.4 Writes.** The log first. No log recovery at first: keep refusing a dirty
      log and let Linux `xfs_repair` deal with a crash until replay exists.
      - [x] **P9.4a File data into blocks a file already has (2026-10-22).** XFS does
            not journal data, so this changes no metadata and needs no transaction —
            which is why it is possible before the log exists. A hole, an unwritten
            extent, growing a file and creating one are all refused. A read-write
            mount is `mount(..., 0)`; `MS_RDONLY` still gives the old behaviour.
            Proved on real images (`xfs_live_test`) and from the board, writing into
            a file `mkfs.xfs` put there, with `xfs_repair -n` clean afterwards and
            the bytes found in the volume (`xfs_qemu_test` phase two).
      - [x] **P9.4b Writing the log (2026-10-22).** A metadata change — the file's
            recorded size — goes to the log as a proper transaction and then to its
            place, with the inode's own CRC recomputed, and an unmount record after
            it so the log is clean. A write may now reach the end of the blocks a
            file already owns, which a block-aligned file has none of and a
            262000-byte one has 144 bytes of.
            **`xfs_logprint` decodes the record the board wrote** — one item, two
            regions, `mode:0x81a4 ver:3 format:2`, `nextents:1` and
            `size:0x3ffd4` — and calls the log `<CLEAN>`; `xfs_repair -n` finds
            nothing wrong; `xfs_db` reads the new size off the inode. Four facts
            came out of that tool rather than any document, and the one worth
            remembering is that **log items are native-endian** while the rest of
            XFS is big-endian (XFS.md §9).
            *Also fixed on the way:* the kernel flushes a mounted filesystem when
            the machine stops. "The program should have called fsync" is not an
            answer a kernel gets to give.
      - [x] **P9.4c Creating a file (2026-10-22).** An inode comes out of the inode
            btrees — both of them, every chunk and the chunks with a free inode —
            the group header and the superblock counter follow it, the inode is
            written with everything a v5 inode carries, and an entry appears in
            the parent's shortform directory. The create and its parent are logged
            as **one transaction**, because a file that exists in a directory but
            has no inode is not half a create.
            The board creates a file over the network, lists it back, is refused a
            duplicate, and **`xfs_repair -n` finds nothing wrong**; `xfs_db` reads
            the new inode and agrees about its mode, format, size and link count.
            Measured facts and the three bugs it cost are in XFS.md §10.
            *Refused, each with its own errno rather than worked around:* the last
            free inode of a chunk, a new chunk, a tree with branches, a directory
            that has outgrown its inode, `O_TRUNC`.
      - [x] **P9.4d Removing a file, and growing one past its own blocks
            (2026-10-22).** The free-space btrees, both of them, with their group
            header and the superblock counter: a growing file asks for the block
            after its last one so that it stays a single extent, and a removed
            file gives its extents back joined to whatever they abut.
            The test counts: the board creates a file, writes 9000 bytes into
            blocks it did not have, reads them back and removes it, and the host
            finds **60 free inodes where there were 60 and 114596 free blocks
            where there were 114596**, with `xfs_repair -n` clean. A leak of one
            block would show.
            *The bug worth remembering:* an inode btree's records are keyed by the
            group-relative inode number, and **a chunk does not start at a
            multiple of its own size** — mkfs.xfs's first starts at inode 96 — so
            a chunk is found by range, not by masking. Masking made every remove
            fail with EIO (XFS.md §11).
            *Still refused:* filling a hole or an unwritten extent inside a file,
            a directory that has outgrown its inode, a leaf that would have to
            split, a tree with branches.

---

## P10 — SMP across the four cores

Stage 1 is a big kernel lock on purpose. The payoff is Go running `GOMAXPROCS=4` in
*user* space, and this kernel spends little time in itself.

- [x] **P10.1 Bring up CPUs 1–3 (2026-10-22).** PSCI `CPU_ON`, a private
      bring-up translation table (the primary has given the low half to user space
      by then, so there is no identity map left), a stack each, and the MMU on.
      They report themselves and then idle: they run no kernel code and touch no
      kernel structure, so the kernel is exactly as correct as it was while the hard
      part is proved on its own. `minikern.smp=off` leaves them alone.
      *Learned:* **PSCI starts a secondary in the mode the primary was entered in**
      — HYP on QEMU — and from HYP a CP15 write to c1/c0/0 goes to HSCTLR, not
      SCTLR, so a core that skips the HYP→SVC drop configures the wrong MMU and
      dies with PSCI reporting success (CLAUDE.md §10).
- [x] **P10.1b What belongs to one core (2026-10-22).** Each core sets up its own
      vector table (`trap_init_cpu`), interrupt interface and banked timer
      interrupt (`gic_init_cpu`), tick (`timer_start_cpu`), **VFP**
      (`arm_vfp_enable`) and current-task slot (`sched_init_cpu`). *Learned:* a
      core that skips the VFP enable faults on the first floating-point
      instruction — which, inside the kernel while holding the lock, deadlocked in
      silence. `trap_dispatch` now leaves the lock alone for a kernel-mode fault,
      so a kernel bug on any core panics with a message instead of freezing.
- [x] **P10.2 One lock around the kernel (2026-10-22).** A ticket spinlock
      (`arch/arm/spinlock.h`) taken at the single way in — `trap_dispatch` — and
      released on the one way out to user mode, `trap_exit`. The rules are in
      `kernel/klock.h`: the lock belongs to the core rather than to the stack, so a
      context switch hands it on; an idle core gives it up around `wfi`; and
      `irq_wait` gives it up while it waits, because the interrupt it waits for
      arrives through the same door. TLB maintenance is the broadcast form now
      (`TLBIALLIS`). All 47 targets pass with it in place.
      *Learned:* a freshly started thread reaches user mode without ever having
      come through `trap_dispatch`, so releasing there deadlocked init on its first
      syscall; the release belongs at the exit, which both paths share.
      A reschedule IPI is **not** done: an idle core notices work on its next tick
      instead, which is a millisecond of latency rather than a correctness problem.
- [x] **P10.3 Fix what the single-core assumption bought (2026-10-22).** Four cores
      schedule by default (`minikern.smp=off` goes back to one), Go sees four CPUs
      through `sched_getaffinity` and sizes GOMAXPROCS from them, and
      `smp_qemu_test` proves both halves: four goroutines run at once and **every
      core shows kernel entries** — a count user space cannot fake. Four cores do
      4× the work in 1.6× the time under TCG.
      **Seven single-core assumptions, each a real bug:**
      1. `sched_yield` pushed a core's **own idle context** onto the shared run
         queue, and fell back to core 0's idle context whatever core it ran on.
      2. `TTBCR` and `TTBR1` are **per core**, and only the primary set them: a
         secondary kept `start.S`'s one-table regime, so the first
         `mmu_switch_user` on it replaced the *whole* address space with a user
         table and the kernel vanished from under it — an endless prefetch abort
         whose faulting instruction was the vector table.
      3. A task stops being `current` before `arm_switch` has saved it, so a
         second core could pop it (two cores, one stack) or reap it (a freed
         kernel stack under a running core). Both are answered by one record of
         "the task this core is leaving", released on the far side of the switch.
      4. A **freshly started thread** never runs that far side (`arm_thread_start`
         goes straight out to user mode), so the task it displaced was never
         re-queued: a lost wakeup that stranded the machine about one boot in six.
      5. `sched_kill_task` freed any task that was not *this* core's current —
         including one another core was running.
      6. The kill mark lived in `t->state`, and the thread's next `sched_sleep`
         **overwrote it**: the thread then waited for ever as an ordinary sleeper
         and its exiting process could never end.
      7. `proc_switch_mm` decided whether to install an address space by comparing
         the outgoing task's with the incoming one — a guess about the hardware
         that `mmu_clear_user` falsified. It records what each core has installed
         now.
      Found with the QEMU monitor (every core's PC) and then with the kernel's own
      new hang check, which prints the task and process tables and panics instead
      of freezing: `minikern.hangcheck=<seconds>`. The last bug was invisible
      without it — a sleeping thread belonging to a process with no other threads.

- [~] **P10.4 Finer locks, where profiling asks — a decision, not a gap.** The one
      lock holds, and `klock_report` gives the number to aim at when there is a
      reason to (743 contended of 1508 entries on four cores). Splitting it before
      a profile says which structure is the contended one would be guessing, and
      every split is a chance to introduce exactly the kind of bug P10.3 lists.
(Finer locks are P10.4 below, and deliberately not designed up front.)

---

## P11 — Thermal, then frequency scaling

**What can and cannot be judged here.** QEMU models neither the thermal sensor nor
the PLLs, so this is the one feature in the project whose hardware half has no
test anywhere but on the board — the same position as the EMAC's PHY setup
(CLAUDE.md §3). The work was therefore split so that the half that can be proved,
is: the policy is pure and host-tested, the governor is driven through ports, and
`minikern.cpufreq=test` runs the whole chain against a fake sensor that ramps from
cold to past critical. The real thing is **off unless asked for**
(`minikern.cpufreq=on`), because an untested register write that browns out a core
is worse than no frequency scaling.

- [x] **P11.5 The clock half of the hardware path, driven for real (2026-10-22).**
      "QEMU models neither the sensor nor the PLL" was half wrong, and the half that
      was wrong mattered: QEMU **does** model the clock control unit
      (`allwinner-h3-ccu`). So `minikern.cpufreq=probe` now drives the real adapter —
      not the fake — through every published operating point and reads `PLL_CPUX`
      back, decoding the factors: the arithmetic, the field placement, the
      clock-source handover and each refusal are all checked against a model of the
      block. That is the standard the EMAC driver was first held to, and it was
      available all along.
      **It found a real bug in the first run.** With only N varied the PLL stops at
      768 MHz (32 × 24 MHz), so **three of this chip's five published points — 816,
      912 and 1008 MHz — had been failing with EINVAL**, silently, for as long as the
      adapter had existed. Nothing had caught it because the governor's tests drive a
      *fake* clock: the policy was proved and the register arithmetic had never run.
      K is varied as well now, smallest first, and all five points read back exactly
      the frequency asked for.
      Two facts about QEMU's model worth keeping: it **does** raise the PLL lock bit,
      so the adapter's lock wait is exercised rather than merely compiled; and it
      accepts the source switch, so the park-on-the-crystal handover is real here too.
- [x] **P11.6 A sensor reading that cannot be believed (2026-10-22).** A sensor that
      cannot be *read* was already treated as the worst case. One that answers with an
      **impossible number** was not — and that is the dangerous direction: this chip's
      conversion turns the all-ones sample of an absent or unclocked sensor block into
      **-264 °C**, and the governor answered a reading like that by going to the top of
      the table and staying there with nothing watching the temperature. On silicon
      that is the difference between a throttled board and a cooked one, and it would
      have presented as "the governor works".
      Two guards, at the two levels that can each be wrong on their own: the adapter
      now rejects both ends of the field (`0` and `0xfff`) as "no sensor", and the
      governor rejects any reading outside -40…150 °C from **any** port, counts them,
      and says so at the end of a run. A rejected reading is treated exactly as a
      failed one: assume the worst.
      The test is a pair, because one half alone proves nothing: a reading that is cold
      but **possible** (20 °C) must still reach 1008 MHz, and the impossible one must
      stay pinned at 480 MHz under a full load with the supply pin present. Both are
      asserted, and the second checks that no faster point was ever applied rather
      than just looking at where it ended.

- [x] **P11.1 Thermal first, and the policy (2026-10-22).** `core/cpufreq.h`
      decides: temperature overrules load, a critical reading goes straight to the
      lowest point, and a hysteresis band below the throttle point stops the CPU
      flapping across it. 15 host cases, including that a steady hot temperature
      *settles* instead of oscillating. `kernel/thermal.c` wires sensor → policy →
      clock and measures load the only honest way: the share of ticks on which a
      core had work. `board/h3/ths.c` reads the sensor — **register map and
      conversion unverified on silicon.**
- [x] **P11.2 Frequency changes (2026-10-22).** `board/h3/cpux.c` parks the cores
      on the 24 MHz crystal, reprograms PLL_CPUX, waits for lock and switches
      back, because a core running on a PLL being reprogrammed runs at an
      undefined frequency. The operating points are **mainline's H3 table**, not
      invented; only those the NEO's fixed supply can feed are offered, since the
      GPIO that switches VDD_CPUX is not driven yet. **Unverified on silicon.**
      `cpufreq_qemu_test` proves the rest: a Go program holds four cores busy
      while the fake sensor ramps, and every decision the policy can make appears
      in the log — up under load, down across the threshold, the critical reading
      reported even with nothing left to give up, and the recovery on the way
      back down.
- [x] **P11.3 The VDD_CPUX pin (2026-10-22).** `minikern.vddcpux=<pin>` names the
      pin that switches the supply, which unlocks the points above 816 MHz: with
      it the governor climbs the whole published table to 1008 MHz at 1300 mV, and
      **without it those points are not offered at all** — asking for a frequency
      the core cannot be fed at is the one mistake here that does not announce
      itself, so `cpux_set` refuses a voltage it has no way to supply.
      The pin is **configuration, not a constant**: which one it is belongs to the
      board's schematic and differs between boards that share this chip, so
      guessing one in the kernel would be guessing at someone else's wiring. The
      half that can be tested is which pin was meant, and it is (`core/gpio.h`, 6
      host cases over every boundary and every kind of rubbish); the pin
      controller itself is **unverified on silicon**, since QEMU does not model
      it.
- [x] **P11.4 Idle cores power themselves off (2026-10-22).** The claim this task
      was written around is right, so it is what got built instead of another
      governor: an idle Cortex-A7 that is **off** draws no core power, while one
      sitting in `wfi` still has its clock and its voltage. A core with nothing to run
      calls PSCI `CPU_OFF`, and is started again — re-entering at `_secondary_start`,
      which is why the per-core setup had to be repeatable — when work appears.
      `minikern.cpuoff=1`; off by default, because an emulator spends no power and
      there is no reason to change how every boot schedules until the kernel runs on a
      board.
      **The invariant that makes it safe: the boot core never parks.** A wake always
      races a power-off — firmware reports ALREADY_ON for a core that has decided to
      go but has not gone — and the answer is not to win that race but to survive
      losing it: a core that is still ticking retries the wake, so a lost one costs
      the same millisecond an idle core already costs to notice work. Without a core
      that cannot park, the last wake could be lost with nobody left to retry it.
      **Three bugs, and two of them passed a test first.**
      *The counter lied about what it measured.* Parking cleared the core's bit in the
      online mask, so `smp_scheduling` fell to 1, Go sized `GOMAXPROCS` to 1 and built
      a workload that never needed the other cores — which then stayed off. A power
      optimisation had removed the parallelism it existed to save power on, and the
      test passed. A parked core is available on demand, so it still counts.
      *The wake passed a virtual address to firmware that wanted a physical one.*
      Every core parked once, stayed off, and reported the single kernel entry it had
      made on the way in — and "every core has more than zero entries" was satisfied
      by exactly that. The check now wants entries in the thousands and more parks
      than there are cores, because a core that comes back, finds the work done and
      parks again is the cycle working.
      *And the real one: a stale record of what the core had installed.* `mmu_init_cpu`
      re-entry points TTBR0 at the empty table, while `proc.c`'s per-core record still
      named the address space from before the power-off — so the next switch to that
      same process skipped the TTBR0 write and the core ran user code with no user
      mapping: a prefetch abort on the first instruction, in SVC with interrupts
      masked, which presents as a wedged machine and not as a fault. It is the vfork
      bug of P10.3 from the other direction — there the record was guessed, here it
      outlived the register it described — and it was found by asking every core where
      its PC was, which is the only tool that has ever worked on this class.
