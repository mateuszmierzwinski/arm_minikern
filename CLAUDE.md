# CLAUDE.md — minikern

> The design record: the architecture and the reasoning behind it, the hardware
> facts as measured rather than as documented, what "Go binaries run unchanged"
> actually requires — syscall by syscall — and every pitfall this project has
> already paid for. The plan is `TASKS.md`, the netboot protocol `NETBOOT.md`.
> When the code and this file disagree, fix this file in the same change.
>
> minikern is developed inside a private monorepo and published from it, so §2
> is written against that monorepo's master ruleset, which is not part of this
> repository. It is kept as it stands because what is interesting there is the
> deviations and their reasons, whether or not the rule being deviated from is
> in front of you.

---

## 0. Goal

**A clean, from-scratch kernel that copies no Linux code but implements Linux's
syscall interface, so Go binaries run unchanged.**

It runs on the **FriendlyElec NanoPi NEO (512 MiB)**, which has an Allwinner H3
(4× Cortex-A7). Its job is deliberately narrow:

1. **Boot.** Load from U-Boot, report what it found, and own the machine.
2. **Bring up networking.** The H3's on-chip EMAC and internal PHY, plus a small IPv4 stack.
3. **Consoles.** `stdout` and `stderr` go to the serial console and are visible as
   devices under a virtual `/dev`.
4. **The Linux ARM EABI syscall interface**, enough for a statically linked Go binary.
   TinyGo comes first and the standard `gc` toolchain later (§7).
5. **A TAR-based root filesystem.** The archive *is* the filesystem, and `/init` is a
   Go program built from source.
6. **Network boot.** Later, the root filesystem is fetched over the network with our
   own discovery and transfer protocol.

"Linux-like" means **ABI-compatible**: the syscall numbers, the register calling
convention, the `struct` layouts user space sees, the process start state (argc, argv,
envp, auxv) and the errno values. These are interface facts. Using them copies no
code. Nothing from the Linux source tree — no driver, header or algorithm — is copied
or vendored here. Ideas may be borrowed and are then rewritten. Where a datasheet and
the Linux sources disagree, the datasheet and measurement on QEMU or hardware decide.

Step by step: each phase in `TASKS.md` ends with something that boots and is tested.

---

## 1. Status

| Phase | State |
|---|---|
| P0 Boot | **Done in QEMU (2026-10-21).** HYP/SVC entry, high-half kernel with MMU and caches, exception vectors and panic, ATAG *and* FDT boot info, PSCI power-off. **The real boot chain is tested from an SD-card image** (BROM → SPL → U-Boot → `boot.scr` → `bootm` with U-Boot's device tree), which is where the timer turns out to run at 24 MHz rather than QEMU's 62.5 and where PSCI refuses `SYSTEM_OFF`. Not yet run on the NEO itself: that needs the board and a 3.3 V serial adapter (P0.9). |
| P1 Network | **Done in QEMU (2026-10-21).** Own EMAC driver, own IPv4 stack: DHCP lease, ARP, ICMP echo, UDP, and a hardened posture against the classes that have hit other small stacks. |
| P2 Kernel core | **Done (2026-10-21).** Page allocator, kernel heap, user address spaces with lazy faults, GIC + generic-timer tick, preemptive scheduler, threads. |
| P3 Syscalls / TinyGo | **Done (2026-10-21).** TinyGo 0.39 binaries run unchanged. |
| P4 TAR root + `/init` | **Done (2026-10-21).** ustar initrd as `/`, devfs, procfs; `/init` built from Go source by Bazel. |
| P5 Netboot | **Done in QEMU, and authenticated (2026-10-22).** Our own MKNB protocol (NETBOOT.md): a unicast sweep finds the server, the archive arrives in windowed blocks and is verified against SHA-256 before anything is mounted. A board can be told which image's digest to accept, and **which key's offers** — Ed25519 verification written from scratch here, with no signing, so the private half stays on the server. The server is a Go program in this tree. |
| P6 Full Go (gc) | **Done for one core (2026-10-21).** Go 1.26 binaries run unchanged: threads, futex, epoll timers, async preemption by SIGURG, GC, signals. SMP is not started. |
| P7 Sockets | **Done for UDP and TCP (2026-10-21).** Go's `net` package works: UDP both ways, TCP active and passive open, and a complete HTTP exchange (client *and* server) inside the guest. |
| P10 SMP | **Done, and idle cores can power off (2026-10-22).** All four Cortex-A7s run user code. One ticket spinlock around the kernel (`kernel/klock.h`) replaces "IRQs masked = locked", taken at `trap_dispatch` and released at `trap_exit`; each core initialises what is its own (vector table, interrupt interface, tick, VFP, address-space regime); TLB maintenance is the broadcast form. Go sees four CPUs and uses them: 4× the work in 1.6× the time, with every core showing kernel entries. With `minikern.cpuoff=1` an idle core powers itself off (PSCI `CPU_OFF`) and is started again when work appears, which on this chip saves more than stepping the clock; the boot core never parks, so a wake that races a power-off is always retried. Seven single-core assumptions turned out to be bugs — they are listed in TASKS.md P10.3, and `minikern.hangcheck` exists because the last one was invisible without it. |
| P8 fork/exec | **Done (2026-10-22).** Go's `os/exec` runs programs: `clone(CLONE_VM\|CLONE_VFORK)`, own pipes, `execve`, `wait4`, zombies that keep an exit status for their parent. Measured from a real Go binary rather than guessed, including that refusing `CLONE_PIDFD` makes Go fall back on its own. |
| P11 Thermal/DVFS | **Policy done, idle power done, the clock adapter driven for real (2026-10-22).** A pure, host-tested policy (temperature overrules load, hysteresis against flapping) and a governor driven through ports, proved end to end against a ramping fake sensor. QEMU **does** model the clock block, so `minikern.cpufreq=probe` drives the real adapter through every operating point and reads `PLL_CPUX` back — which caught three of five published points failing with EINVAL, because varying only N stops at 768 MHz. The **thermal sensor and the regulator pin have no model**, so they remain the board's to settle; a reading that is impossible is now refused rather than believed. |
| P9 Storage | **NBD, and XFS a program can actually use (2026-10-22): reading, formatting, logged writes, creating files, growing them and removing them.** Our own NBD client over our own TCP, a buffer cache, `/dev/nbd0`, a Go NBD server in-tree — and an XFS v5 reader that mounts filesystems Linux made, lists every directory shape and reads files byte for byte, refusing every feature it does not fully understand and every dirty log. A Go program formats a volume over the network (`userland/internal/xfsmk`) and `mount("/dev/nbd0", "/mnt", "xfs", …)` mounts it; `xfs_repair` finds nothing wrong with what the board writes (`XFS.md`). |

A Go program on the board can now also reach a **network block device** and
read an **XFS filesystem** off it — the volume being a file on a server, and
the filesystem one `mkfs.xfs` made — and a write that grows a file into blocks
it already owns goes through the **log**, as a transaction `xfs_logprint`
decodes field by field (XFS.md §9).

**The goal is met in QEMU:** a standard `GOOS=linux GOARCH=arm` Go binary, and a TinyGo one, run unchanged on a kernel that copies no Linux code — and a Go program can now fetch its own root filesystem over the network and talk TCP and UDP. 64 Bazel test targets prove it on every change.

---


## 2. Deviations from the master ruleset (C, freestanding)

The master rules are written for Go. The language table in the master `CLAUDE.md`
says C follows them "in spirit". This is how minikern applies them:

| Master rule | Here |
|---|---|
| Hexagonal architecture (Rules 1.2–1.4) | **Applies.** `src/core` is the hexagon. It sees only `core/ports.h`. Board and arch code are adapters, and `src/board/<board>/main.c` is the composition root (§5). |
| `interfaces.go` (Rule 7.4) | `core/ports.h` holds every port: a struct of function pointers plus `self`. |
| `errors.go` (Rule 8.3) | `core/errno.h`. The values are **Linux's errno numbers**, because the same codes go back to user space as `-errno`. |
| `models.go` / `output.go` / `utils.go` | No direct equivalent. One `.c`/`.h` pair per concern. Pure transforms live in `src/lib` and `src/core` and are host-tested. |
| Function length (Rule 11) | **Applies in full**: 25 code lines, 27 tolerated, 30 only with the named test in the doc comment. |
| Doc comments (Rule 15) | Apply to C. The comment block sits above the declaration in the `.h` file, and to a `static` function's definition when it is non-trivial. |
| Mocking with gomock (Rule 17) | N/A. C ports are structs of function pointers, so a test passes a fake struct. The harness is `test/ktest.h`, with no framework. |
| Benchmarks (Rule 18.2) | Deferred until there is a hot path worth measuring (the network checksum, the TAR lookup). |
| Concurrency (Rule 20) | **One lock around the whole kernel** (`kernel/klock.h`), which preserves exactly the property the single-core design relied on — one core inside the kernel at a time — on four cores, without auditing every structure. IRQs are still masked throughout the kernel, so there is no nesting; the lock is taken at `trap_dispatch`, released at `trap_exit`, handed on across a context switch, and given up by an idle core and by `irq_wait`. The parallelism this buys is in **user space**, which is where a Go program spends its time; finer locks come only where profiling asks (TASKS.md P10.4). |
| CLI flags (Rule 21) | N/A in the kernel. Configuration is the **kernel command line** (`core/cmdline.h`). |
| Sonar (Rule 24) | **Skipped.** There is no `go.mod` anywhere in the project (the Go code here builds through Bazel alone, as `src/go/purplelx`'s installer does), so the decision logic in Rule 24 says skip, and the CFamily analyzer is not used for the C. |
| CVE suite (Rule 25) | **Applies, and is the project's largest test surface.** Four suites: `cve_syscall_qemu_test` (42 hostile syscalls from a Go program), `net_test` + `netboot_test` + `tcp_hostile_test` (malformed and attacking packets, also under ASan and UBSan), and the server's `cve_test.go` (amplification, traversal, exhaustion). Each class is named with the product class it comes from. |
| Go code in a C project | The netboot server (`server/`) and the userland programs (`userland/cmd/*`) are Go, inside a C project's tree, because they are parts of *this* system: a protocol has two ends and they must agree. They follow the master Go rules (hexagonal, `interfaces.go`, `models.go`, `errors.go`, flags, tests with benchmarks). |
| Security posture script (Rule 29) | The script runs Go scanners, so it does not apply yet. It will apply to any Go code this project adds (the `/init` program, a netboot server). For C, `-Wall -Wextra -Werror` is the floor. The CVE-class review (Rule 25) starts with P1, because a network stack is attack surface: malformed frames, ARP/DHCP spoofing, length-field overflows. |
| Pure-static Go binaries | N/A to the kernel. The Go programs it runs are static ELF by construction. |
| Versioning (Rule 28) | The series comes from `//:version.bzl` through `local_defines`; the build part is Bazel's build timestamp, written into a generated C file by `build_stamp`. **One deviation:** the rule says `--stamp`, but the Starlark call that reveals it is private API, so the opt-in is our own build setting with the same shape — `--//:stamp`. An unstamped build still reports `0` rather than a number that means nothing. |

**No libc, no third-party code — not even libgcc.** Everything the kernel needs is
written here: `memcpy` and friends (`src/lib/kmem.c`), 64-bit division
(`src/lib/div64.c` + `aeabi_div.S`), the formatter, the allocators, the TAR reader, and
later the network stack. Adding any dependency needs the operator's approval first.

---

## 3. The hardware — measured facts

**QEMU 10.1.5 `-M orangepi-pc`** emulates the same Allwinner H3. Measured with a probe
kernel on 2026-10-21:

| Fact | Value | Consequence |
|---|---|---|
| CPU | MIDR `0x410fc075` (Cortex-A7 r0p5) | ARMv7-A with the virtualization extensions, VFPv4, NEON, and hardware `udiv` in ARM state |
| Entry mode, `-kernel x.uImage` | **HYP** (CPSR `0x1da`), r2 = `0x40000100` (ATAGs) | `start.S` drops HYP → SVC |
| Entry mode, `-kernel x.elf` | **SVC** (CPSR `0x1d3`), r2 = 0, no boot information | the board's defaults apply |
| ATAGs QEMU passes | CORE, MEM (1 GiB at `0x40000000`), CMDLINE (from `-append`), INITRD2 with `-initrd` | `core/bootinfo.c` reads them |
| `-dtb` / `-initrd` placement | initrd at `0x48000000` (DRAM + 128 MiB), the DTB right after it (`0x48001000` for a small initrd); QEMU fills `/chosen` bootargs and `linux,initrd-*` itself | both lie in the RAM the pmm reserves at boot |
| RAM | **QEMU refuses anything but 1 GiB** for this machine | the kernel clamps memory to the NEO's 512 MiB |
| Generic timer | CNTFRQ = **62.5 MHz** entered with `-kernel`, and **24.000 MHz** — the real H3 frequency — when the same kernel is booted from an SD card through U-Boot (measured 2026-10-22, `sdboot_qemu_test`) | always read CNTFRQ; never hard-code it. One number is wrong on each path, and only the card path shows it |
| PSCI through U-Boot | `CPU_ON` works (all four cores come up), **`SYSTEM_OFF` is refused** | a board booted from a card cannot power itself off: the kernel reports the refusal and idles. QEMU's own PSCI, on the `-kernel` path, does power off |
| SD boot | QEMU's BROM reads the SPL from **sector 16** and U-Boot finds its payload 32 KiB further on (`SYS_MMCSD_RAW_MODE_U_BOOT_SECTOR = 0x40`); `boot.scr` on the first FAT partition is run by the default `bootcmd` | `scripts/mksdcard.sh` builds the card, and the image that boots in QEMU is the one written to hardware |
| UART0 | `0x01C28000`, 16550-style, registers 4 bytes apart, LSR at `+0x14` | works without setup under QEMU; U-Boot sets it up on hardware |
| EMAC MDIO | `0x01C30000` + `0x48` (MII_CMD) / `+0x4C` (MII_DATA) | the internal PHY answers at **address 1**, ID `0x0044:0x1400` (it also appears at `0x11`; QEMU decodes only 4 address bits) |
| PSCI | `smc #0` with r0 = `0x84000008` (SYSTEM_OFF) ends QEMU, exit status 0, in about 40 ms | the boot test powers off instead of timing out |
| NIC | `-nic user` connects slirp to the EMAC: DHCP `10.0.2.15/24`, gateway `.2` (it answers ping), DNS `.3`, lease 86400 s | `net_qemu_test` |
| EMAC model | 32-bit accesses only; it pads RX frames to 64 bytes and reports length + 4 (FCS); a write of `TX_CTL1` with DMA enable starts transmission; there is no address filter | the driver subtracts the FCS and re-kicks TX on every send |
| SID | read through PRCTL (`0x01C14040`: `offset << 16 \| 0xAC00 \| 2`, wait for bit 1 to clear) and RDKEY (`+0x60`); QEMU's ID is `02c00081 11112222 33330000 44556677` | `board/h3/sid.c`; the MAC address and the entropy seed come from it |

**From the H3 datasheet. Verify each one when first used (P1+):**

- DRAM starts at `0x40000000`. The NEO has 512 MiB, so DRAM ends at `0x60000000`.
- GIC-400: distributor `0x01C81000`, CPU interface `0x01C82000`. UART0 is SPI 0
  (interrupt ID 32), EMAC is SPI 82 (ID 114).
- CCU `0x01C20000`. EMAC clock gate is `BUS_CLK_GATING_REG0` (`+0x60`) bit 17, and its
  reset is `BUS_SOFT_RST_REG0` (`+0x2C0`) bit 17. The internal PHY's gate is
  `BUS_CLK_GATING_REG4` (`+0x70`) bit 0 and its reset `BUS_SOFT_RST_REG2` (`+0x2C8`) bit 2.
  **All four verified against QEMU's clock model** (`hwreport_qemu_test`, 2026-10-22):
  the driver sets exactly those bits and they read back. They had been written off as
  "unverified on silicon" together with the PHY-select register, which was three
  things treated as one — the gates and resets are in the block QEMU models.
- System-control `EMAC_EPHY_CLK_REG` (`0x01C00030`) selects the internal PHY,
  powers it up and sets its MDIO address (1). The bit layout must be checked against
  the datasheet. This register is the difference between "link up" and "nothing" on
  real hardware, while QEMU does not model it at all.
- **Pin controllers: there are two.** PA..PH are the main one (`0x01C20800`); **PL is
  port 0 of the always-on one** (`0x01F02C00`), which needs its clock ungated from the
  R_CCU (`0x01F01400 + 0x28`, bit 0) first. That matters because **the CPU-supply
  switch on every H3 board that has one is on PL** — the Orange Pi One's is PL6, 1.1 V
  to 1.3 V. **The NanoPi NEO has no CPU supply regulator at all**, so its three
  operating points up to 816 MHz are the board rather than a limitation of this
  kernel. (Established from the device trees of an implementation that runs on this
  hardware, 2026-10-22; see §10.)
- **Thermal sensor (`0x01C25000`)**: needs its bus clock ungated (`0x068` bit 8), its
  reset released (`0x2D0` bit 8) and its module clock enabled (`0x074` bit 31, divider
  0 for 24 MHz) before any of its registers mean anything. **This kernel did none of
  it until 2026-10-22** — see §10. All three are verified against QEMU's clock model
  (`hwreport_qemu_test`); the sensor itself has no model.
  **Its conversion constants are disputed.** The datasheet's are
  `223000 - raw * 119100 / 1000`; an implementation that runs on this hardware uses
  `217000 - raw * 1211 / 10`, about 9 °C cooler at a room-temperature sample. The
  datasheet's are in force, because the rule is that the datasheet and a measurement
  decide, and because they read hotter, so the error throttles early. The hardware
  report prints both for every sample, so one reading from a room of a known
  temperature settles it.
- SID (`0x01C14200`) holds the chip's unique ID. U-Boot derives the NEO's MAC address
  from it, and we will do the same (a locally administered MAC built from the SID), so
  a board keeps one MAC across reboots.
- The NEO's debug UART is the 3-pin header: UART0, 3.3 V, 115200 8N1.

**The boot chain on real hardware** (P0.9): BROM → U-Boot SPL → U-Boot from the SD
card. Mainline U-Boot's `nanopi_neo_defconfig` provides it (Fedora's
`uboot-images-armv7` may ship it ready-made; check). U-Boot then runs:

```
load mmc 0:1 0x42000000 minikern.uImage
setenv bootargs console=uart0
bootm 0x42000000 - ${fdtcontroladdr}
```

`bootm` copies the image to its load address (`0x40008000`), disables caches and the
MMU, and passes U-Boot's own device tree in r2. That is why reading the FDT is P0.6:
modern sunxi U-Boot builds do not produce ATAGs. QEMU can also run this whole chain
(`-sd <image>` with a U-Boot built for `orangepi_pc`), which tests the real path
without hardware.

---

## 4. Toolchain and build

Host tools. Each is checked before use (master Rule 27.7 in spirit):

| Tool | Package | Used for |
|---|---|---|
| `arm-linux-gnu-gcc` 15.2, binutils 2.45 | `gcc-arm-linux-gnu`, `binutils-arm-linux-gnu` | the kernel. This is a bare cross compiler with no libc, which suits a freestanding kernel. `libgcc.a` provides `__aeabi_uldivmod` |
| `mkimage` | `uboot-tools` | the uImage wrapper for `bootm` / QEMU |
| `qemu-system-arm` 10.1.5 | `qemu-system-arm` | running and testing the kernel |
| U-Boot 2025.07 for `orangepi_pc` | **built by `scripts/build_uboot.sh`** into the monorepo's gitignored `tools/uboot/` | the SD-card boot test. Fedora 43 ships **no** armv7 U-Boot (`uboot-images-armv7` is gone, only armv8 remains), and packages need root, so it is built as a per-machine host tool — the same convention Rule 29 uses for the scanners. Three config knobs have to go off (`TOOLS_LIBCRYPTO`, `TOOLS_KWBIMAGE`, `TOOLS_MKEFICAPSULE`: Fedora 43's openssl-devel has no `engine.h` and there is no gnutls), and `binman` cannot run (setuptools 81 dropped `pkg_resources`), so the script assembles the final file itself |
| `sfdisk`, `mkfs.vfat`, `mcopy` | `util-linux`, `dosfstools`, `mtools` | building the SD-card image without root |
| `qemu-arm` (user mode) | `qemu-user` | **measuring** what a Go binary asks of Linux: `qemu-arm -strace ./bin` |
| `tinygo` 0.39.0 (LLVM 19, Go 1.25.5) | `tinygo` | `/init` (P4) |
| `go` 1.26 | host | the `gc` comparison binaries (§7) |

clang 21 with ROCm's `ld.lld` 19 also builds the kernel (proved in the first spike).
It is the fallback if gcc ever becomes a problem.

**How Bazel builds a kernel without touching shared configuration:**

- `platforms/BUILD` defines the project's **own** constraint (`:allwinner_h3`) and a
  `:nanopi_neo` platform. The root module does not register `@platforms`, and a
  bare-metal target shares no constraint with a host anyway.
- `toolchain/` defines a `cc_toolchain` over `/usr/bin/arm-linux-gnu-*`, which is
  selected only for that platform. Every kernel compile flag lives in
  `toolchain/cc_toolchain_config.bzl`. Host-tool paths are listed once in
  `toolchain/paths.bzl`.
- `minikern.bzl`'s `kernel_image` rule switches its `kernel` attribute to the H3
  platform (and registers the toolchain through `extra_toolchains` in that
  transition). It emits `minikern.elf` / `.bin` / `.uImage`. The load address is read
  back from the ELF, so `src/arch/arm/link.ld` is the only place it is written down.
- H3-only libraries carry `target_compatible_with = [":allwinner_h3"]`, so
  `bazel build //src/c/...` on the host **skips** them, and the wildcard stays green on
  machines without the cross compiler. Targets that need host tools (the image, the
  QEMU runner, the boot test) are tagged **`manual`** and are built by name.
- Portable code (`:kfmt`, `:core`) is the **same target** in both configurations. The
  host tests and the kernel link identical sources.

```bash
bazel build //:minikern          # the image
bazel run   //:qemu              # boot it; Ctrl-A X quits
bazel test  //...               # 13 host unit-test targets
bazel test  //:qemu_tests        # every QEMU test (manual)
```

**After any change, run both test commands.** Because of the `manual` tag, the QEMU
tests are invisible to a wildcard, so a change that breaks the image keeps the host tests
green. `:qemu_tests` is a `test_suite` naming them all:

| Test | Proves |
|---|---|
| `boot_qemu_test` | ATAG and FDT boots, initrd discovery, the three deliberate exceptions |
| `user_asm_qemu_test` | user mode, the syscall path and the start state, with no runtime involved |
| `tinygo_qemu_test` | a TinyGo 0.39 binary, unchanged |
| `go_hello_qemu_test` | the same program through the gc toolchain (rules_go, Go 1.26) |
| `go_abi_qemu_test` | the gc runtime's Linux surface: threads, futex, timers, preemption, GC, faults, files |
| `cve_syscall_qemu_test` | 42 hostile syscalls and memory accesses, all refused |
| `net_qemu_test` | P1: EMAC + PHY, DHCP lease, gateway ping, UDP echo from the host via hostfwd |
| `net_sockets_qemu_test` | P7: Go's `net` over UDP and TCP, both directions, plus HTTP inside the guest |
| `netboot_qemu_test` | P5: the board sweeps, finds the real Go server, fetches and verifies its root filesystem, and runs a Go init from it |
| `net_sanitized_test` | every network test rebuilt with clang under ASan and UBSan |
| `xfs_sanitized_test` | the XFS reader likewise: it parses somebody's filesystem |
| `net_nbd_live_test` | our NBD **client** against `qemu-nbd`, a server nobody here wrote |
| `nbd_server_live_test` | our NBD **server** against `nbdinfo`, `nbdcopy` and `qemu-io` |
| `nbd_qemu_test` | P9.1: a Go program on the board reads and writes a volume on a server, and the host checks the file afterwards with `cmp` |
| `xfs_live_test` | P9.2: the XFS reader against filesystems `mkfs.xfs` made, judged by `xfs_repair` |
| `mkfsxfs_live_test` | P9.3: the Go formatter's output, judged by `xfs_repair` and then read by our own reader |
| `xfs_qemu_test` | P9.3: the board formats a filesystem over the network, mounts it, reads it — and `xfs_repair` on the host approves it |
| `netboot_two_guests_test` | P5.6: one board serves another its root filesystem on a segment of their own, with nothing of the host in the conversation |
| `entropy_qemu_test` | two boots of one image produce different random bytes |
| `cpuoff_qemu_test` | idle cores power themselves off and come back, four cores still offered, same result |
| `cpufreq_qemu_test` | P11: the governor against a ramping fake sensor, with and without a supply pin |
| `hwreport_qemu_test` | P0.9: the hardware report is complete, and seven clock/reset bits read back as documented — the EMAC's four and the thermal sensor's three |
| `cpuclock_qemu_test` | P11: the **real** clock adapter against QEMU's clock block — every operating point set and read back from `PLL_CPUX`, and every refusal |
| `sdboot_qemu_test` | P0.9: the real chain — BROM → SPL → U-Boot → `boot.scr` → `bootm` with U-Boot's device tree → a Go program from the ramdisk, all from the card image that would be written to a board |

**Userland programs** live in `userland/cmd/<name>/main.go` and build with rules_go
(`goos = "linux"`, `goarch = "arm"`, `pure = "on"`). `minikern_rootfs` (in `minikern.bzl`)
packs one as `/init`, plus `userland/rootfs/`. TinyGo goes through `tinygo_binary`: the
host's TinyGo, pointed at a Go of the 1.19–1.25 range it supports (`TINYGO_GOROOT` in
`toolchain/paths.bzl`), with throwaway caches so it stays sandboxed.

Kernel command line (all optional):

| Option | Effect |
|---|---|
| `minikern.init=/path` | the program to run as pid 1 (default `/init`) |
| `minikern.halt=poweroff` | power off when init ends, or on a panic (the tests use it) |
| `minikern.strace=1` | log every syscall with its arguments and result |
| `minikern.epoch=<seconds>` | the wall clock at boot (default 0: there is no RTC driver yet) |
| `minikern.selftest=undef\|dabt\|pabt\|dma-alias\|wedge` | take that exception, or wedge the kernel on purpose |
| `minikern.cpufreq=on\|test\|probe\|test-coldsensor\|test-badsensor` | run the governor against the real hardware, or a fake sensor that ramps; `probe` drives the **real clock adapter** through every operating point and reads the PLL back (QEMU models the clock block); the two sensor modes answer with a cold-but-possible and an impossible reading |
| `minikern.vddcpux=<pin>` | the pin that switches VDD_CPUX, e.g. `PA10` — without it the operating points above 816 MHz are not offered |
| `minikern.watchdog=<seconds>` | reboot the board if the kernel stops ticking for that long |
| `minikern.net=off` | leave the network down |
| `minikern.netboot[=<ip>]` | fetch the root filesystem over the network; with an address, probe only that one and never sweep |
| `minikern.netboot.key=<hex>` | the Ed25519 public key whose holder's offers this board will accept, refusing every other — so only one machine can say what it runs |
| `minikern.rootfs.sha256=<hex>` | accept only a netbooted root filesystem with this digest, refusing every other offer — so answering the sweep first decides nothing |
| `minikern.nbd=<ip>[:<port>][/<export>]` | attach a network block device as `/dev/nbd0` before init runs (port 10809 by default) |
| `minikern.ip=<addr>/<prefix>` | bring the interface up with this address and run no DHCP client (with `minikern.gw=` beside it) |
| `minikern.mac=<xx:xx:xx:xx:xx:xx>` | use this hardware address instead of the one derived from the chip's serial number — needed for two QEMU guests, which share a serial number |
| `minikern.dns=<ip>` | the resolver `/etc/resolv.conf` should name, instead of the lease's |
| `minikern.hwreport=1` | print every value this kernel took from the datasheet, beside what it should be — the one command to run on a real board |
| `minikern.hangcheck=<seconds>` | panic with the task table when every core has been idle this long and something is still asleep — a lost wakeup reported instead of a silent hang |
| `minikern.cpuoff=1` | let an idle core power itself off with PSCI CPU_OFF, started again when work appears (the boot core never parks) |
| `minikern.smp=off` | do not start the other three cores at all |
| `minikern.netstat=1` | print the interface, TCP and block-cache counters when init exits |
| `minikern.env.NAME=VALUE` | put `NAME=VALUE` in init's environment |

---

## 5. Architecture

```
src/
├── lib/        freestanding helpers, no kernel knowledge (host-tested)
│   ├── kfmt.*      printf-style formatter (tested against libc)
│   ├── div64.*     64-bit division; aeabi_div.S binds it to the EABI names
│   ├── crc32c.*    CRC-32C, which every v5 XFS metadata block carries
│   ├── sha512.*    SHA-512;  fe25519.*  the field of 2^255-19
│   ├── ed25519.*   verifying a signature — never signing one
│   └── kmem.c      memset / memcpy / memmove / memcmp (kernel only)
├── core/       the hexagon: pure, portable C, host-tested; includes only core/ and lib/
│   ├── ports.h     every port: console, console input, power, page source, irq chip
│   ├── errno.h     Linux errno values (they go back to user space as -errno)
│   ├── bootinfo.*, fdt.*, cmdline.*    what the boot loader passed
│   ├── pmm.*       physical pages: a bitmap, aligned runs
│   ├── kheap.*     kernel objects: power-of-two classes over a page port
│   ├── vma.*       a process's regions: insert/remove/protect/split/merge/find-free
│   ├── elf.*       static ARM ELF validation
│   ├── ustack.*    the process start state (argc, argv, envp, auxv)
│   ├── tar.*       ustar, read in place
│   ├── console.*   kprintf over the console port
│   ├── blockdev.h  a block device: fixed-size blocks, addressed by index
│   ├── bcache.*    a buffer cache over one, because the device is a network away
│   ├── nbd.*       the NBD client: a volume that is a file on a server
│   ├── xfs/        XFS v5 (XFS.md): superblock, inodes, extents, directories
│   │               in all three shapes, the log (checked and written), and
│   │               the inode and free-space btrees — creating a file,
│   │               growing it and removing it
│   └── net/        the IPv4 stack, all pure and host-tested
│       ├── net.h       the one public header (netif, ports, UDP, ICMP)
│       ├── net_eth.c   Ethernet II framing and the input entry point
│       ├── net_arp.c   ARP, with a cache that believes only what it asked for
│       ├── net_ipv4.c  IPv4 in and out, the loopback path, ICMP echo
│       ├── net_udp.c   UDP, one handler per port;  net_echo.c  RFC 862
│       ├── net_dhcp.c  the DHCP client
│       ├── netboot.*   the MKNB client: sweep, fetch, verify (NETBOOT.md)
│       └── tcp.h + net_tcp{,_in,_out}.c   TCP: state machine, input, output
├── arch/arm/   ARMv7-A
│   ├── start.S     entry: HYP→SVC, boot page table, MMU on, jump high
│   ├── vectors.S   vector table, trapframe save/restore, thread switch, VFP
│   ├── trap.*      trap dispatch to the kernel's handlers; register dumps
│   ├── mmu.*       TTBR0/TTBR1 split, TLB and cache maintenance
│   ├── pgtable.*   user translation tables (pure, host-tested)
│   ├── gic.*       GIC-400;  timer.*  generic timer;  psci.*  power;  cpu.*
│   ├── smp.*       the other three cores: PSCI bring-up, per-core setup
│   ├── spinlock.h  a ticket lock, the one synchronisation primitive
│   └── layout.h    the virtual address map
├── kernel/     board-independent kernel services, over arch/ and core/
│   ├── mem.*       the live pmm + kheap + table memory
│   ├── sched.*     tasks (threads), run queue, sleep channels, preemption
│   ├── proc.*      processes (thread groups), exit, exit_group
│   ├── mm.*        address spaces: lazy faults, mmap/munmap/mprotect/brk, user copies
│   ├── exec.*      ELF + stack + sigpage → a runnable process
│   ├── signal.*    actions, masks, altstack, rt_sigframe delivery, rt_sigreturn
│   ├── klock.*     one lock around the kernel: one core inside it at a time
│   ├── watchdog.*  feeds the board's watchdog while the tick still arrives
│   ├── thermal.*   reads the temperature, asks the policy, moves the clock
│   ├── futex.*, time.*, irq.*, fault.*, random.*, tty.*, panic.*, selftest.*
│   ├── entropy.*   a pool fed by the times at which interrupts arrive
│   ├── net.*       the live interface: EMAC frames in, tick, DHCP, echo service
│   ├── netboot.*   fetches the root filesystem before init runs
│   ├── dma.*       an uncached window of RAM for descriptor rings and buffers
│   ├── nbd.*       the live NBD client: our TCP as its transport, /dev/nbd0
│   ├── xfsfs.*     an XFS filesystem in the namespace (mount, read, list, write)
│   ├── tmpfs.*     a small writable filesystem in memory, at /tmp
│   ├── genfs.*     the files the kernel makes up: /etc/resolv.conf, /etc/hosts
│   ├── socket.*    BSD sockets over the stack;  sock_udp.c, sock_tcp.c
│   ├── vfs.*       files and the namespace;  tarfs.*, devfs.*, procfs.*
│   └── syscall.* + sys_<domain>.c   the syscall table, one file per domain
└── board/h3/   the NanoPi NEO: memory map, UART, EMAC, SID, and main.c
                 (the composition root)

server/         the servers, in Go, each a hexagon in internal/ with its
                adapters: cmd/mknbserver (netboot, NETBOOT.md) and
                cmd/nbdserver (a volume file over NBD). Both carry their own
                adversarial suites — amplification for one, traversal and
                out-of-range writes for the other.
userland/       Go programs the kernel runs: cmd/hello, cmd/goabi (the ABI
                suite), cmd/hostile (the syscall attack suite), cmd/nettest
                (the socket suite), cmd/nbdtest, cmd/xfstest, cmd/netbootinit,
                plus rootfs/ files — and internal/xfsmk, the XFS formatter,
                which is user-space code on purpose (XFS.md §7).
test/tools/     host-side helpers for the tests (netecho).
```

**The address space** (`arch/arm/layout.h`): user space is 0–2 GiB through TTBR0
(TTBCR.N = 1), one table per process. The kernel's half is one table for life (TTBR1):
all 512 MiB of RAM linearly at `0xC0000000` (virt = phys + `0x80000000`), and the H3's
on-chip I/O at `0xF0000000`. The kernel is linked high and loaded low; `start.S` builds
the boot table with PC-relative code, then jumps. In user space, the stack's top is at
`0x7FFF0000` (8 MiB), the signal trampoline page at `0x7FFF8000`, and mmap allocates
top-down below the stack, never under 4 MiB.

**Traps**: every exception lands on the current SVC stack (`srsdb`), so the
exception modes need no stacks. A trap from user mode puts its frame at the very top
of the thread's 8 KiB kernel stack, so that frame *is* the user context: syscalls
read r0–r7 from it, and signal delivery and `rt_sigreturn` rewrite it.

**The kernel never dereferences a user pointer.** `mm_copy_in/out` check the region's
rights, fault the page in, and copy through the linear map. A bad pointer is `-EFAULT`,
never a kernel abort (the hostile suite proves it). File ops see only kernel buffers.

**The syscall table** is assembled at boot from per-domain tables (`sys_proc.c`,
`sys_mem.c`, `sys_time.c`, `sys_signal.c`, `sys_futex.c`, `sys_file.c`, `sys_dir.c`,
`sys_poll.c`): one `struct syscall_entry {nr, name, call}` per call, the C form of
master Rule 6.3. An unclaimed number is `-ENOSYS` and logged once: the log names the
next syscall a program needs.

Rules that keep it clean:

- **Dependencies point inward.** `core` never includes `kernel/`, `arch/` or `board/`.
  `kernel/` uses arch interfaces and core, never a board. Only
  `board/<board>/main.c` names concrete adapters (master Rule 7.3 in C). A second
  board, or a host simulation of the network stack, arrives without touching `core`.
- **A port is a struct of function pointers plus `void *self`.** A test fakes it with
  a stack struct.
- **No non-`static` globals** outside the composition root.
- **Every MMIO access goes through a named register offset.**
- **Bounded parsing.** Anything that reads loader-, network-, file- or user-supplied
  memory takes a bound, rejects what runs past it, and has a hostile test.

---

## 6. Coding conventions

- C17, `-Wall -Wextra -Werror` on both host and target. The kernel is built `-O2 -g`,
  soft-float (`-mfloat-abi=soft`: the kernel never touches VFP state by accident), ARM
  state, `-ffreestanding -fno-builtin`.
- Names: `module_verb_object` for exported functions (`bootinfo_from_atags`,
  `console_attach`). The file is named after the module. Header guards are
  `MINIKERN_<DIR>_<FILE>_H`.
- Errors: return `int` with `KERR_OK` or a negative `KERR_*`. Out-parameters are filled
  only on success, unless the doc comment says otherwise.
- Comments: a header comment says what the module is and why. Single-line comments in
  bodies are for magic numbers only (`/* 0x84000008: PSCI 0.2 SYSTEM_OFF */`), as in
  master Rule 15.
- Tests: `test/<module>_test.c`, one `cc_test` each. Cover both the happy path and the
  hostile one: truncated, overlong and malformed input.

---

## 7. Research: what "Go binaries run unchanged" requires

Measured 2026-10-21 by building a hello-world (`fmt.Println` + `os.Getpid` +
`fmt.Fprintln(os.Stderr, …)`) for `linux/arm` and running it under
`qemu-arm -strace`.

### TinyGo 0.39 (`GOOS=linux GOARCH=arm tinygo build`) — the first target

- **Static ELF, 225 KiB, single-threaded, linked at `0x10000`.** It has four `PT_LOAD`
  segments and a `PT_TLS` (4 bytes). Its C runtime is **musl**, linked in.
- **Hard-float EABI (ARMv6 + VFPv2).** The kernel must enable VFP (CPACR cp10/cp11,
  FPEXC.EN) before entering user mode, and must save and restore the VFP registers
  once more than one process exists.
- Syscalls in order: `set_tid_address`, `rt_sigprocmask`, `rt_sigaction` (SIGBUS,
  SIGILL, SIGSEGV and signal 41), `clock_gettime64` (PROCESS_CPUTIME, REALTIME,
  MONOTONIC_RAW), `open`/`read`/`close` of **`/proc/self/stat`** (the conservative GC
  looks for its stack bottom there), `mmap2` of anonymous 64 KiB heap chunks,
  `sched_getaffinity`, `prlimit64(RLIMIT_NOFILE)`, `getpid`, `write(1)`, `write(2)`,
  `exit_group`. That is **14 distinct syscalls**.
- **The ARM private syscall `set_tls` (`0xf0005`)**, which `-strace` does not print.
  musl's `__set_thread_area` checks **`HWCAP_TLS`** in `AT_HWCAP`. With that bit set,
  it reads the thread pointer from `TPIDRURO` (cp15) and calls `set_tls`. Without it,
  it falls back to Linux's kuser helper page at `0xffff0fe0`. **Set `HWCAP_TLS` and
  implement `set_tls`, and the kuser page is never needed.**
- With `-gc=leaking -scheduler=none`, the binary is 152 KiB and instead does one
  **`mmap2` of 1 GiB** for the heap, plus a `getrandom`. A 512 MiB board can only
  honour that with lazy anonymous mappings (allocate on first touch), or by refusing it
  cleanly. The default GC asks for 64 KiB at a time, so it avoids the issue.

### gc Go 1.26 (`GOOS=linux GOARCH=arm GOARM=7 CGO_ENABLED=0`) — the later target

- Static ELF, 1.5 MiB, linked at `0x10000`. **It is multi-threaded from the first
  instruction:** 3× `clone` (threads), `futex`, `tgkill` plus **`SIGURG`** (async
  preemption), `sigaltstack`, `rt_sigreturn`, 114× `rt_sigaction`, `rt_sigprocmask`,
  `sched_yield`, `sched_getaffinity`, `nanosleep`, `clock_gettime`, `mmap2`,
  `madvise`, `brk`, `gettid`, `getpid`, `prctl`, `fcntl`, `openat`, `pread64`
  (`/sys/kernel/mm/transparent_hugepage/hpage_pmd_size`, which may fail), `read`,
  `write`, `exit_group`. That is **25 distinct syscalls.**
- This needs threads in one address space, a futex wait queue and real signal
  delivery (a frame on the user stack, `sigreturn`). That is a scheduler-and-signals
  project in its own right, which is why it is P6, after TinyGo works end to end.

### What both need from the kernel

- **The process start state:** `sp` → `argc`, `argv[]`, `NULL`, `envp[]`, `NULL`, then
  the auxv pairs `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ` (4096), `AT_ENTRY`,
  `AT_RANDOM` (16 bytes on the stack), `AT_HWCAP` (VFP, VFPv3, NEON, **TLS**, …),
  `AT_HWCAP2`, `AT_PLATFORM` ("v7l"), `AT_EXECFN`, `AT_NULL`.
- **The EABI syscall convention:** `svc #0`, number in r7, arguments in r0–r6, result
  in r0 (`-4095..-1` = `-errno`).
- **A user address space** starting at `0x10000`, so the **MMU is mandatory** (P2).
  User space is in the low 3 GiB and the kernel is mapped where user space cannot reach
  it.
- **Unknown syscalls return `-ENOSYS` and are logged once by number.** That log is how
  each next syscall is found.

### What Go's `net` package needs (measured on minikern, 2026-10-21)

- **Sockets are always non-blocking.** Go opens them with `SOCK_NONBLOCK`, so
  `connect` returns `EINPROGRESS` and the netpoller waits for writability.
- **After a non-blocking connect it calls `getpeername` to confirm.** On a zero
  `SO_ERROR` Go does *not* trust the poller ("the runtime poller can wake us up
  spuriously", `net/fd_unix.go`): it calls `getpeername`, and only a success means
  connected. A socket whose own `connected` flag had not caught up answered
  `ENOTCONN`, and Go waited out the whole dial timeout on a connection it already
  had. The socket's view of the connection must therefore follow the protocol's
  state, which is what the TCP layer's change notification is for.
- `getsockname` follows every successful connect and bind, including for UDP.
- `SO_ERROR` must clear on read, `SO_TYPE`, `SO_SNDBUF` and `SO_RCVBUF` must answer,
  and an IPv6 option must fail with `ENOPROTOOPT` so Go stays on IPv4.
- `socket(AF_INET6, ...)` must fail with `EAFNOSUPPORT`, or Go prefers IPv6 and
  then cannot reach anything.
- An HTTP server needs `accept4`, and `http.Client` needs `shutdown` for its
  half-close; both arrive with `SOCK_NONBLOCK|SOCK_CLOEXEC` set.

### "sysctl" — resolved (operator, 2026-10-21)

"Linux-like sysctls" means **the calls that let TinyGo and the Go runtime run**: the
syscall interface above. No `/proc/sys` tree is wanted.

### What gc Go taught us (measured on minikern, 2026-10-21)

- **Go on ARM sets `SA_RESTORER` with a zero `sa_restorer`** (`_SA_RESTORER = 0 //
  unused on ARM`). Linux then returns from the handler through its own trampoline in a
  per-process "sigpage". minikern maps the same at `0x7FFF8000` (`mov r7, #173; svc #0`,
  ARM and Thumb). Without it, the first SIGURG preemption crashes.
- **`time.Sleep` needs the netpoller**: `epoll_create1`, `eventfd2`, `epoll_ctl`, and
  `epoll_pwait` with a timeout (that timeout is the timer wheel's deadline).
  Registering a regular file must fail with `EPERM`, as on Linux, so Go uses blocking reads.
- At start it also calls `fcntl(F_GETFD)` on fds 0–2 (they must exist), `sigaltstack`
  per thread, 64× `rt_sigaction`, and `prctl(PR_SET_VMA)` (`EINVAL` is fine).
- Its 32-bit heap reserves big `PROT_NONE` regions (512 MiB at 4 MiB) and commits them
  with `mmap(MAP_FIXED)`: lazy mapping makes that cost nothing.
- rules_go cross-compiles `linux/arm` with the default GOARM (7, hard-float), so the
  kernel's VFP save/restore across threads and signals is exercised by every test.

---

## 8. Root filesystem

- **The format is POSIX ustar** (100-byte name plus a 155-byte prefix, octal sizes,
  checksummed 512-byte headers). The archive is **read-only and used in place**: the
  index points into the archive's own bytes, so nothing is copied.
- **`/dev` is synthesized** rather than stored: `console`, `ttyS0`, `null`, `zero`,
  plus `stdout` and `stderr`, which are aliases for fd 1 and fd 2 of the opening
  process. `/proc/self/stat` is synthesized too, because TinyGo reads it.
- **`/init` is a TinyGo binary built from `init/main.go`** by a Bazel rule (host
  `tinygo`, so `manual`). The archive is built by Bazel (`rules_pkg` `pkg_tar` is
  already in `MODULE.bazel`).
- **Delivery:** an initrd (QEMU `-initrd`, U-Boot `bootm addr initrd fdt`, read from
  ATAG_INITRD2 or FDT `/chosen/linux,initrd-*`), or **fetched over the network** with
  our own protocol (NETBOOT.md). `minikern.netboot` asks for the network; if that
  fails the initrd is used instead, and the fallback is logged.

## 9. Network

- **The driver is our own** (`board/h3/emac.c`), from the datasheet's EMAC chapter
  and checked against QEMU's model. Chained rings: 64 RX, 8 TX, 1600-byte buffers.
  They live in the **DMA window**: 1 MiB of RAM, normal non-cacheable, at
  `0xE0000000` (`kernel/dma`), so the caches never need maintenance in steady
  state. Creating it does: the memory arrives zeroed through the cacheable linear
  map, so it is cleaned and invalidated out of the cache before the attributes
  change and its cacheable alias is then unmapped — one page is never reachable
  under two cacheabilities (§10). Receive is interrupt-driven (SPI 82); transmit
  kicks the DMA on every send. The MAC address comes from the chip's SID, so a
  board keeps it.
- **The stack is our own** (`core/net`): Ethernet II, ARP, IPv4 with a loopback
  path, ICMP echo, UDP, a DHCP client, and **TCP**. It is pure, so it is tested on
  the host with crafted frames, hostile frames and mutation fuzzers, under ASan.
  `kernel/net` wires it up; `minikern.net=off` skips all of it.
- **TCP** (`core/net/tcp.h`) implements the state machine, sequence arithmetic with
  wraparound, a send queue with one retransmission timer and exponential backoff, a
  real advertised window, the MSS option, RST generation and validation, and a
  bounded accept backlog, **congestion control** in Reno's shape (slow start,
  congestion avoidance, fast retransmit and fast recovery) and **reassembly** of
  segments that arrive early — four held per connection, after which one is dropped
  and the peer resends it, which is what the stack did with all of them before.
  It does **not** do SACK, window scaling, timestamps or urgent data. SACK is the
  one worth wanting and it improves on reassembly rather than replacing it: without
  the queue there is nothing to acknowledge selectively, and with it a loss already
  costs one round trip instead of a window's worth. TIME_WAIT is 2 s rather than two
  maximum segment lifetimes, because 16 connections cannot be held for minutes. Each
  of those is a throughput limit, never a correctness shortcut on the wire; they are
  listed in the header so the next reader does not have to find out.
- **The source address must be the one the upper layer checksummed.** IPv4 used to
  stamp `nif->ip` on every packet, which broke every loopback TCP connection: TCP had
  computed its checksum over `127.0.0.1`. `net_ip_send_from` and
  `net_udp_send_from` take the source, and `net_source_for` picks the loopback
  address for a loopback destination.
- **Sockets** (`kernel/socket.h`): a socket is a file whose ops forward to a
  per-protocol table, so `sys_socket.c` knows nothing about either protocol.
  Blocking calls sleep on the socket and the protocol's change notification wakes
  them; readiness reaches epoll through `vfs_poll_notify`, which is what makes Go's
  netpoller work.
- **Policy for an untrusted network** (net.h, tcp.h): every length checked against
  the frame; fragments and martian sources dropped; ARP believes only replies it
  asked for; ICMP echo and the UDP echo service share a 32/s token bucket; no
  replies to broadcasts (smurf, fraggle) or to other small services' ports; a TCP
  reset is believed only at exactly the expected sequence number (blind reset
  injection); a SYN flood is bounded by the listener's backlog, not by the
  connection table.
- **DNS needs no resolver in the kernel.** The lease's DNS option becomes
  `/etc/resolv.conf` (`kernel/genfs.c`) and Go's own pure-Go resolver does the
  rest in user space over the UDP sockets it already has. `minikern.dns=`
  overrides the lease and survives it, because a lease arriving later would
  otherwise overwrite exactly the thing the override is for.
- **Not yet**: IPv6, and SACK (see above).
- **QEMU wiring:** `-nic user` for DHCP and egress; `hostfwd=` to reach the guest's
  ports from the host; and, for the guest's *outbound* TCP, an address on the real
  network, because slirp does not forward guest TCP to `10.0.2.2` (§10).

---

## 10. Pitfalls already met

- **QEMU orangepi-pc refuses `-m 512`.** Run it with its 1 GiB and let the kernel clamp.
- **A uImage enters in HYP mode, an ELF in SVC.** Any code before `start.S`'s mode
  switch must be valid in both.
- **`_start` is referenced by nothing**, so a library that holds it needs
  `alwayslink = True`, or the linker drops it from the archive and the image boots
  into garbage.
- **Bazel 9 has no builtin `CcToolchainConfigInfo`.** Load it from `@rules_cc//cc:defs.bzl`.
- **`-serial stdio` piped through `head` shows nothing when QEMU is killed.** Use
  `-serial file:<log>` in scripts.
- **Fedora's `arm-linux-gnu` libgcc is hard-float**, so a soft-float kernel cannot link it
  ("uses VFP register arguments"). The kernel carries its own `__aeabi_uldivmod` /
  `__aeabi_ldivmod` and links no libgcc at all.
- **QEMU's SID block answers 32-bit reads only.** A byte read is a synchronous
  external abort (DFSR `0x008`). Treat every H3 register block as word-only.
- **A small-page descriptor is `1x`**: bit 0 is XN. A type test of `== 0b10` misses
  every non-executable page (`pgtable_test` caught it).
- **TinyGo 0.39 refuses Go ≥ 1.26**, and on this machine `/usr/bin/go` (Fedora 1.26.9)
  comes before `~/go/go_current` (1.25.5) in a minimal `PATH`. The rule puts
  `TINYGO_GOROOT` first and sets `GOTOOLCHAIN=local`, `GOWORK=off`.
- **QEMU's SID is not memory-mapped**: reading the efuse words directly returns zeros
  (the first MAC came out `02:00:00:00:00:00`). Use the PRCTL/RDKEY key-read
  interface, which silicon has too.
- **Idling with IRQs masked** (the PSCI `idle`) stops the tick and the network. When
  there is nothing to run, idle in `sched_idle_loop`, which opens IRQs around `wfi`.
- **`pkill -f <pattern>` matches the shell that runs it** when the pattern is in that
  shell's own command line, which kills the run. Use `pkill -x <name>` or a PID.
- **Nothing large goes on a kernel stack.** `tcp_reset_peer` built a whole
  `struct tcp_conn` (16 KiB of queues) as a local to borrow its addressing, on an
  8 KiB kernel stack: any peer could have wedged the kernel by sending a segment to a
  closed port. The toolchain now passes **`-Wframe-larger-than=1024`**, so the next
  one is a build error rather than a hang.
- **Unsigned sequence arithmetic saturates or it underflows.** `tcp_unsent` computed
  `tx_len - in_flight`, and a sent FIN occupies a sequence number that is not in the
  send queue: the subtraction wrapped to 4 GiB and the connection flooded the peer
  with 8 KiB of stale buffer. Any "how much is left" helper over sequence numbers
  needs the `a > b ? a - b : 0` form.
- **An upper layer's checksum pins the IP source address.** See §9: TCP over
  loopback failed its own checksum until the IP layer stopped stamping `nif->ip` on
  everything.
- **A transient send failure must not fail a connect.** `tcp_connect` returned the
  SYN's send result, so dialling an address whose MAC was not yet resolved failed
  immediately with `EAGAIN` instead of waiting for ARP and the retransmission timer.
- **A netboot window must fit the client's receive ring.** A 64-block (64 KiB)
  window arrives as one burst and overran a 32-entry EMAC ring: 33 of 576 blocks
  survived. The window is 16 blocks and the ring is 64.
- **QEMU's slirp invents hosts.** A probe to an address in slirp's subnet that
  nothing owns still reaches the host, and the reply appears to come *from that
  address* — so a netboot sweep found a "server" at 10.0.2.31 that no ARP could
  resolve. It is also why the client must abandon a server that offers an image and
  then delivers nothing.
- **slirp forwards guest UDP to `10.0.2.2` but not guest TCP.** A UDP datagram to
  `10.0.2.2:<port>` reaches a host service on loopback; a TCP SYN to the same
  address is never answered. For the guest's outbound TCP, point it at the host's
  address on the real network, which slirp NATs properly (`ip -4 -o addr show scope
  global`). `guestfwd=tcp:<addr>:<port>-cmd:<program>` is the other way.
- **`ncat --exec` does not echo TCP on this machine** (it does for UDP), which cost
  an afternoon chasing a kernel bug that was not there. The tests use
  `test/tools/netecho`, a Go program, and verify the host side independently first.
- **`filter-dump` cannot attach to a board's built-in NIC**, so there is no easy
  pcap of the EMAC. `minikern.netstat=1` and the TCP counters exist for that reason.
- **A feature can be unusable on every board it was written for, silently.**
  `minikern.vddcpux=<pin>` exists to unlock the operating points above 816 MHz. The
  pin that does that is on **PL** on every H3 board that has one, and `gpio_parse`
  accepted only PA..PH — so the single pin name anyone would ever pass was rejected as
  a typo, and a rejected name just means "three operating points", which looks like
  normal operation. Worse, a unit test **asserted** `PL0` was invalid, with the
  comment "PL is past PH on this chip": true of the main pin controller, wrong about
  the chip, and it pinned the bug in place. Meanwhile the NEO this kernel targets has
  no such regulator at all, so the feature had no board it worked on. A test that
  encodes an assumption protects the assumption, not the user.
- **A driver can be complete and still never have been switched on.** `ths.c` wrote
  the thermal sensor's control registers without ungating its bus clock, releasing its
  reset or starting its module clock — none of which QEMU models, so nothing could show
  it. On a board the block would have been unclocked and held in reset: every write
  ignored, every reading refused, and the governor running for ever on the worst-case
  assumption **while looking like it worked**. What found it was reading the device tree
  of an implementation that runs on these boards and noticing it declares two clocks
  and a reset for a node this driver touched neither of. A peripheral's clocks and
  resets are part of its contract, and "the registers are right" is not the same as
  "the block is awake". The same cross-check independently confirmed this kernel's
  existing EMAC numbers (reset `0x2c0` bit 17, PHY reset `0x2c8` bit 2) and exposed the
  disputed temperature constants in §3.
- **A fake proves the policy and nothing about the registers.** The frequency
  governor was tested end to end against a fake clock, so the policy was right and
  the adapter's arithmetic had never run: varying only N caps `PLL_CPUX` at 768 MHz,
  and **three of the chip's five published operating points had been failing with
  EINVAL** the whole time. Nothing noticed because nothing had ever driven the real
  port. QEMU models the clock block (`allwinner-h3-ccu`), so it could have been
  caught from the first day — `info qtree` before assuming a device is absent, which
  is the same lesson the watchdog taught.
- **Worst-case handling has to cover a reading that is wrong, not just one that is
  missing.** A sensor that could not be read was treated as critical; a sensor that
  answered with an impossible number was believed. This chip's conversion turns the
  all-ones sample of an absent or unclocked sensor into **-264 °C**, which the
  governor answered by going to the top of the table with nothing watching the
  temperature — and it would have looked exactly like a working governor.
- **Zeroing memory is a cache operation.** `dma_init` took its 1 MiB window from
  `page_alloc`, which zeroes through the kernel's **cacheable** linear map, and then
  mapped the same physical memory non-cacheable for the EMAC. Nothing cleaned the
  cache in between, and 1 MiB is larger than the A7's L1 + the H3's L2, so the CPU
  was left holding dirty lines full of zeroes for memory a device now owned: they
  are written back whenever the cache chooses to evict them, over a packet, long
  after the handover. The symptom would have been exactly the kind of bug that eats
  a week — intermittent RX/TX corruption on silicon, impossible to reproduce in
  QEMU, which models no caches at all. The window is now cleaned and invalidated
  (`mmu_flush_dcache_range`, DCCIMVAC by VA) **before** the attributes change, and
  its cacheable alias is then unmapped, because two mappings of one page with
  different cacheability is a mismatched-attributes alias that the architecture does
  not keep coherent (ARM ARM B2.9). `minikern.selftest=dma-alias` proves the alias
  is really gone — the only half of this QEMU can judge — and that an accidental
  access faults instead of corrupting a frame.
- **PSCI starts a secondary core in the mode the primary was entered in.** On
  QEMU that is HYP, and from HYP a CP15 write to `c1/c0/0` or `c2/c0/0` lands on
  **HSCTLR and HTTBR** — the hypervisor's own registers — not the ones the code
  means. A secondary that skipped the HYP→SVC drop therefore configured the wrong
  MMU and died on the first instruction fetch after enabling it, with PSCI
  reporting success and the core simply never arriving. The drop is now one macro
  (`drop_to_svc`) that the primary and every secondary use, and it also hands the
  counter and the coprocessors down to PL1, which each core must do for itself.
  *Found by writing a character to the UART's physical address at each step of the
  bring-up:* the core printed its way to the MMU enable and no further, which is
  the only kind of evidence available before there is a console.
- **Per-core state outlives the core.** A core that powers itself off
  (`minikern.cpuoff=1`) comes back through `_secondary_start` with its MMU off and
  `mmu_init_cpu` pointing TTBR0 at the empty table — while `proc.c`'s per-core record
  of *which address space it has installed* still named the one from before. The next
  switch to that same process therefore skipped the TTBR0 write, and the core ran user
  code with no user mapping: a prefetch abort on its first instruction, in SVC with
  interrupts masked, which looks like a wedged machine rather than a fault. Anything a
  core caches **about its own registers** has to be invalidated by whatever sets those
  registers from nothing. It is the vfork bug below from the other direction: there the
  record was guessed, here it outlived what it described.
- **A "did it come back?" check must not be satisfiable by "it never left".** The same
  work proved its wake path by requiring every core to have more than zero kernel
  entries — which every core already had from the one entry it made before it ever
  parked, so a wake that started nothing passed. Measure the thing that only happens
  if the cycle runs: entries in the thousands, and more power-offs than there are
  cores.
- **A secondary cannot use the primary's boot table.** `mmu_init` gives the low
  half to user space, so by the time a core is started there is no identity
  mapping for the physical address it is entered at. It gets its own table
  (`smp_l1`), mapping RAM identically and high, and joins the kernel's regime
  after the jump.
- **A check against random data must compare, not inspect.** The XFS write test
  asserted that the sixteen bytes before its range did not *contain* the byte it
  was about to write — over random file contents, which is a one-in-sixteen
  failure for no reason, and the NBD test had made the same mistake before it.
  Record the neighbours first and assert they are **unchanged**. Both were caught
  by running the suite in the exported tree (§11) rather than by the monorepo,
  because a fresh random file is what finally hits a 6% flake.
- **A test that does nothing must not pass.** `net_sanitized_test.sh` found its
  include roots by walking up to a directory named `minikern`, and picked the test
  mains out of its arguments with `/test/`. Both assumed the monorepo's path.
  Exported to a standalone repository, where the project *is* the workspace root,
  the first spun forever (`dirname` reaches `/` and stays there) and the second
  matched nothing — so the script compiled no suites, ran no tests and **exited 0**.
  A `$(rootpaths)` path is relative to the runfiles root, so match it at a path
  boundary (`(^|/)test/`), derive a root by stripping a known suffix rather than by
  recognising a name, and fail when the source list comes out empty. Found by
  running the suites in the exported tree (§11) rather than trusting the export.

---

## 11. Publishing

The project is public at **`github.com/mateuszmierzwinski/arm_minikern`** (MIT,
`LICENSE`), while this monorepo stays the place it is written. The public tree is
**generated, never edited**: `scripts/export_github.sh` copies the project, rewrites
the labels (`//:x` → `//:x`), the output paths and the Go
import paths, writes a `MODULE.bazel` declaring only what this project uses — no
gazelle, because every Go package here is standard library only — plus `version.bzl`
with the series read from the monorepo's, then **builds and runs every test in the
staged tree before it will push**. That verification is the point: the two bugs above
were invisible from inside the monorepo, and one of them made a test suite silently
vacuous.

`MEMORY.md` and the export script itself stay behind. `CLAUDE.md` goes out with its
first nine lines replaced, since they are the only part that assumes the monorepo.
A change meant for the public repository is made here and re-exported.
