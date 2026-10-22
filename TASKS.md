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
- [ ] **P0.8 Own the UART.** Program UART0 ourselves: the CCU gate and reset bits, a
      baud divisor from the 24 MHz APB clock, 8N1, FIFO on. Booting no longer
      depends on U-Boot's setup.
- [ ] **P0.9 Real hardware.** An SD card with mainline U-Boot (`nanopi_neo`), a
      `boot.scr` that runs `bootm` with `${fdtcontroladdr}`, and a documented flash
      procedure in `README.md`. The banner must appear on the NEO's debug header.
      Optionally, the same SD image under QEMU (`-sd`, U-Boot `orangepi_pc`) to test
      the U-Boot path without the board.
- [ ] **P0.10 Version stamp.** The build part of `MAJOR.MINOR.BUILD` from Bazel's
      `{BUILD_TIMESTAMP}` on a `--stamp` build (master Rule 28.1), for C.

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
- [ ] **P1.11 Lease renewal** at T1 and rebinding at T2. Slirp's lease is a day long;
      a real LAN's may not be.
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
- [ ] P5.6 Two-QEMU test over `-netdev socket,mcast=`, so discovery is proven
      between two guests rather than through slirp's NAT.

## P6 — Full Go (gc toolchain) — done on one core (2026-10-21)

- [x] Threads (`clone` with the thread flag set; SETTLS, PARENT_SETTID,
      CHILD_CLEARTID), `futex` (WAIT/WAKE and their BITSET forms, both timespec
      widths), signal delivery with the ARM `rt_sigframe`, `sigaltstack`,
      `rt_sigreturn`, a sigpage trampoline, `tgkill`, `nanosleep`, `sched_yield`,
      epoll + eventfd for the netpoller. `go_abi_qemu_test` covers it.
- [ ] SMP across the H3's four cores (PSCI CPU_ON): needs real locks (CLAUDE.md §2).

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
- [ ] P7.6 `SO_RCVTIMEO`/`SO_SNDTIMEO` and `poll`/`select`, for programs that are
      not Go.

## P8 — Hardening and gaps (found on the way)

- [ ] **P8.1 `fork`/`execve`/`wait4`**: there is one process today. Go's `os/exec`
      needs them, and the proof is a QEMU test in which a Go program starts another
      program, pipes to it and reaps it.
- [ ] TCP congestion control (it sends what the peer's window allows, which is
      right on a LAN and wrong on a WAN), SACK, and out-of-order reassembly.
- [x] **The DMA window's cache state (2026-10-22).** `page_alloc` zeroes through the
      cacheable linear map, so handing that memory to a device left dirty lines to be
      written back over a packet later. It is cleaned and invalidated before the
      attributes change, and the cacheable alias unmapped so no page is reachable
      under two cacheabilities. `minikern.selftest=dma-alias` proves the alias faults.
      *Learned:* a QEMU-only test suite cannot see a cache bug at all (CLAUDE.md §10).
- [ ] An RTC driver (the H3 has one, and so does QEMU) instead of `minikern.epoch=`.
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
- [ ] A real entropy source (the H3 crypto engine's PRNG, or timing jitter): today's
      `getrandom` is xoshiro128** seeded from the timer and chip ID, which is fine for
      hash seeds and not fine for keys.
- [ ] `mprotect(PROT_NONE)` drops resident pages instead of keeping them inaccessible
      (Go does not depend on this).
- [ ] Out-of-memory policy: today a fault that cannot get a page kills the process.
- [ ] Relative `..` in paths, symlinks with relative targets.
- [ ] The VFP state is saved on every switch. Make it lazy once switches are frequent.

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
      - [ ] **P9.4d Removing a file and growing one past its own blocks**, which
            need what is still missing: deleting a btree record, and the free-space
            btrees with their AGF counters.

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

- [ ] **P10.4 Finer locks, where profiling asks.** Not before: the one lock holds
      and `klock_report` gives the contention count (743 of 1508 entries on four
      cores in the SMP test) to aim at.
- [ ] **P10.4 Finer locks only where profiling says so** — likely futex, the network
      stack, the run queue. Not designed up front.

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
- [ ] **P11.4 A governor last, and least.** `wfi` in idle plus PSCI `CPU_OFF` for
      unused cores saves more on this chip than frequency stepping does.
