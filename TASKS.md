# minikern — tasks

The phased plan. Each task ends with a test that proves it, and each phase ends with
something that boots. The background is in `CLAUDE.md`: hardware facts in §3, the Go
requirements in §7. Mark a task done with `[x]` and the date. Add a line on anything
it taught us.

Legend: `[x]` done · `[ ]` to do · `[~]` partial

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
- [ ] **P5.5 Signing.** The digest proves the transfer, not the server: the first
      machine to answer the sweep decides what the board runs. Carry an Ed25519
      signature over (xid, length, digest) in the OFFER and ship the public key in
      the kernel. Until then netboot is for a network you control (NETBOOT.md §5).
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
- [ ] P7.5 DNS, so Go can dial a name rather than an address (the lease already
      carries the resolver).
- [ ] P7.6 `SO_RCVTIMEO`/`SO_SNDTIMEO` and `poll`/`select`, for programs that are
      not Go.

## P8 — Hardening and gaps (found on the way)

- [ ] `fork`/`execve`/`wait4`: there is one process today. Go's `os/exec` needs them.
- [ ] TCP congestion control (it sends what the peer's window allows, which is
      right on a LAN and wrong on a WAN), SACK, and out-of-order reassembly.
- [ ] An RTC driver (the H3 has one, and so does QEMU) instead of `minikern.epoch=`.
- [ ] A real entropy source (the H3 crypto engine's PRNG, or timing jitter): today's
      `getrandom` is xoshiro128** seeded from the timer and chip ID, which is fine for
      hash seeds and not fine for keys.
- [ ] `mprotect(PROT_NONE)` drops resident pages instead of keeping them inaccessible
      (Go does not depend on this).
- [ ] Out-of-memory policy: today a fault that cannot get a page kills the process.
- [ ] Relative `..` in paths, symlinks with relative targets.
- [ ] The VFP state is saved on every switch. Make it lazy once switches are frequent.
