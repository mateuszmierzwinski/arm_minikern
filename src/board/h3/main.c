/* main.c — the composition root for the NanoPi NEO. The only file that
 * names concrete adapters: it builds them, hands them to the core as ports,
 * and runs the boot sequence. */
#include <stdbool.h>
#include <stdint.h>

#include "arch/arm/cpu.h"
#include "arch/arm/gic.h"
#include "arch/arm/layout.h"
#include "arch/arm/mmu.h"
#include "arch/arm/psci.h"
#include "arch/arm/smp.h"
#include "arch/arm/timer.h"
#include "arch/arm/trap.h"
#include "board/h3/emac.h"
#include "board/h3/h3.h"
#include "board/h3/rtc.h"
#include "board/h3/sid.h"
#include "board/h3/uart.h"
#include "board/h3/version.h"
#include "core/bootinfo.h"
#include "core/cmdline.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/devfs.h"
#include "kernel/dma.h"
#include "kernel/exec.h"
#include "kernel/fault.h"
#include "kernel/irq.h"
#include "board/h3/cpux.h"
#include "core/gpio.h"
#include "board/h3/ths.h"
#include "board/h3/wdt.h"
#include "kernel/klock.h"
#include "kernel/thermal.h"
#include "kernel/watchdog.h"
#include "kernel/xfsfs.h"
#include "kernel/mem.h"
#include "kernel/net.h"
#include "kernel/nbd.h"
#include "kernel/netboot.h"
#include "kernel/panic.h"
#include "kernel/proc.h"
#include "kernel/random.h"
#include "kernel/sched.h"
#include "kernel/selftest.h"
#include "kernel/signal.h"
#include "kernel/syscall.h"
#include "kernel/time.h"
#include "kernel/tty.h"
#include "kernel/vfs.h"

#ifndef MINIKERN_VERSION
#define MINIKERN_VERSION "0.0"
#endif

/* 16 KiB — the most an ATAG list may span from r2 before we stop reading. */
#define BOOTINFO_MAX_WORDS 4096u
/* 1 MiB — far above any real board's tree (the NEO's is ~25 KiB). */
#define BOOTINFO_MAX_FDT 0x100000u
/* minikern.env.NAME=VALUE puts NAME=VALUE in init's environment. */
#define ENV_PREFIX "minikern.env."
#define ENV_MAX 16u
#define ENV_TEXT_BYTES 512u

void kmain(uint32_t r0, uint32_t machine, uint32_t r2, uint32_t entry_cpsr);

static void report_cpu(uint32_t entry_cpsr) {
    uint32_t hz = arm_cntfrq();
    kprintf("cpu: midr %08x, entered in %s mode, timer %u.%03u MHz\n", arm_midr(),
            arm_mode_name(entry_cpsr), hz / 1000000u, (hz / 1000u) % 1000u);
}

/* Parses the block by its kind; returns KERR_OK or the parser's error. */
static int parse_bootinfo(const uint32_t *words, enum bootinfo_source kind, struct bootinfo *info) {
    if (kind == BOOTINFO_ATAGS) {
        return bootinfo_from_atags(words, BOOTINFO_MAX_WORDS, info);
    }
    if (kind == BOOTINFO_FDT) {
        return bootinfo_from_fdt(words, BOOTINFO_MAX_FDT, info);
    }
    return KERR_OK;
}

/* The kernel's pointer to the boot block at physical r2, or NULL when r2 is
 * not in RAM (QEMU's ELF boot passes 0). */
static const uint32_t *bootinfo_block(uint32_t r2) {
    return phys_in_ram(r2, 8) ? phys_to_virt(r2) : NULL;
}

/* Reads what the loader passed in r2; unknown or malformed forms leave the
 * board defaults, and a loader that did not name the memory base gets ours. */
static struct bootinfo read_bootinfo(uint32_t r2) {
    const uint32_t *words = bootinfo_block(r2);
    enum bootinfo_source kind = bootinfo_classify(words);
    struct bootinfo info = {.source = kind, .cmdline = ""};
    if (parse_bootinfo(words, kind, &info) != KERR_OK) {
        kprintf("boot: malformed %s at %08x, ignored\n", kind == BOOTINFO_FDT ? "device tree" : "ATAG list", r2);
        info = (struct bootinfo){.source = BOOTINFO_NONE, .cmdline = ""};
    }
    if (info.mem_size == 0) {
        info.mem_base = H3_DRAM_BASE;
    }
    bootinfo_clamp_memory(&info, NANOPI_NEO_DRAM_SIZE);
    return info;
}

static const char *source_name(enum bootinfo_source s) {
    static const char *const names[] = {"none", "atags", "fdt"};
    return names[s];
}

static void report_boot(const struct bootinfo *info, uint32_t r2) {
    kprintf("boot: info %s at %08x\n", source_name(info->source), r2);
    kprintf("mem: %u MiB at %08x\n", info->mem_size >> 20, info->mem_base);
    kprintf("cmdline: \"%s\"\n", info->cmdline);
    if (info->initrd_end > info->initrd_start) {
        kprintf("initrd: %u bytes at %08x\n", info->initrd_end - info->initrd_start, info->initrd_start);
    }
}

/* Runs minikern.selftest=<name> when the command line asks for one. */
static void maybe_selftest(const char *cmdline) {
    size_t len = 0;
    const char *name = cmdline_value(cmdline, "minikern.selftest", &len);
    if (name != NULL) {
        selftest_run(name, (unsigned)len);
    }
}

static struct power_port power;
static const char *boot_cmdline = "";

static bool halt_requested(void) {
    return cmdline_equals(boot_cmdline, "minikern.halt", "poweroff");
}

/* The end of the boot with nothing to run: power off when asked (tests),
 * otherwise idle forever. */
static void finish(void) {
    if (halt_requested()) {
        kprintf("minikern: powering off\n");
        power.poweroff(power.self);
        kprintf("minikern: firmware refused power-off\n");
    }
    for (;;) {
        power.idle(power.self);
    }
}

/* Translation and exceptions: everything after this may fault safely. */
static void bring_up_cpu(void) {
    static const struct trap_handlers no_handlers;
    mmu_init();
    trap_init(&no_handlers);
    arm_vfp_enable();
}

/* The trap layer's return-to-user hook: a thread another core has killed
 * stops here rather than going back to user mode, then preemption, then
 * signals. */
static void return_to_user(struct trapframe *tf) {
    if (sched_killed()) {
        proc_exit_thread(0);
    }
    sched_preempt_point();
    signal_deliver(tf);
}

static void irq_entry(struct trapframe *tf) {
    (void)tf;
    irq_dispatch();
}

static uint32_t chip_id[4];

/* The chip's unique ID: entropy now, the MAC address later. */
static void seed_from_chip_id(void) {
    if (h3_sid_read(chip_id) != KERR_OK) {
        kprintf("sid: chip ID unreadable\n");
    }
    kprintf("sid: %08x %08x %08x %08x\n", chip_id[0], chip_id[1], chip_id[2], chip_id[3]);
    random_seed(chip_id, sizeof chip_id);
}

/* Interrupts, the tick, the scheduler and every trap handler. */
static void bring_up_kernel(uint32_t epoch) {
    static const struct trap_handlers handlers = {
        .syscall = syscall_dispatch, .irq = irq_entry, .fault = fault_handle,
        .undef = fault_undefined, .return_to_user = return_to_user,
    };
    struct irq_chip_port gic = gic_init(io_virt(H3_GIC_DIST), io_virt(H3_GIC_CPU));
    uint64_t now = timer_count();
    irq_init(&gic);
    trap_init(&handlers);
    sched_init();
    syscall_init();
    time_init(epoch);
    random_seed(&now, sizeof now);
    seed_from_chip_id();
}

static void bring_up_console_input(const struct console_port *uart) {
    struct console_input_port in = tty_input_port();
    devfs_set_console(uart, &in);
    h3_uart0_enable_rx(tty_receive);
}

/* True when the command-line word starts with prefix. */
static bool cmdline_word_has_prefix(const char *word, size_t len, const char *prefix) {
    for (size_t i = 0; prefix[i] != '\0'; i++) {
        if (i >= len || word[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

/* A number-valued command-line option, or fallback. */
static uint32_t cmdline_number(const char *key, uint32_t fallback) {
    size_t len = 0;
    const char *v = cmdline_value(boot_cmdline, key, &len);
    uint32_t n = 0;
    if (v == NULL || len == 0) {
        return fallback;
    }
    for (size_t i = 0; i < len && v[i] >= '0' && v[i] <= '9'; i++) {
        n = n * 10u + (uint32_t)(v[i] - '0');
    }
    return n;
}

/* minikern.dns=<ip>: the resolver /etc/resolv.conf should name, instead of
 * the one the lease advertised. */
static void override_dns(void) {
    size_t len = 0;
    const char *v = cmdline_value(boot_cmdline, "minikern.dns", &len);
    ip4_t dns = 0;
    if (v == NULL) {
        return;
    }
    if (ip4_parse(v, (uint32_t)len, &dns) != KERR_OK) {
        kprintf("net: minikern.dns=\"%.*s\" is not an address, ignored\n", (int)len, v);
        return;
    }
    knet_set_dns(dns);
    kprintf("net: resolver set to %.*s by the command line\n", (int)len, v);
}

/* Splits "<ip>[:<port>][/<export>]" into its three parts. The export name
 * may be empty, which asks the server for its default volume. */
static int parse_nbd_target(const char *s, size_t len, ip4_t *ip, uint16_t *port, char *name, uint32_t cap) {
    size_t at = 0;
    uint32_t i = 0;
    while (at < len && s[at] != ':' && s[at] != '/') {
        at++;
    }
    if (ip4_parse(s, (uint32_t)at, ip) != KERR_OK) {
        return KERR_INVAL;
    }
    if (at < len && s[at] == ':') {
        uint32_t n = 0;
        for (at++; at < len && s[at] >= '0' && s[at] <= '9'; at++) {
            n = n * 10u + (uint32_t)(s[at] - '0');
        }
        if (n == 0 || n > 65535u) {
            return KERR_INVAL;
        }
        *port = (uint16_t)n;
    }
    at += at < len && s[at] == '/' ? 1u : 0u;
    for (; at < len && i + 1 < cap; at++, i++) {
        name[i] = s[at];
    }
    name[i] = '\0';
    return at == len ? KERR_OK : KERR_INVAL;
}

/* minikern.nbd=<ip>[:<port>][/<export>]: attaches a network block device as
 * /dev/nbd0 before init runs, so a program can format, mount, read or
 * write it. A failure is reported and the boot carries on: the volume is
 * not the root filesystem. */
static void attach_nbd(void) {
    size_t len = 0;
    const char *v = cmdline_value(boot_cmdline, "minikern.nbd", &len);
    char name[32];
    ip4_t ip = 0;
    uint16_t port = KNBD_DEFAULT_PORT;
    if (v == NULL) {
        return;
    }
    if (parse_nbd_target(v, len, &ip, &port, name, sizeof name) != KERR_OK) {
        kprintf("nbd: minikern.nbd=\"%.*s\" is not <ip>[:<port>][/<export>], ignored\n", (int)len, v);
        return;
    }
    (void)knbd_connect(ip, port, name);
}

/* The other three cores. They idle once they are up (arch/arm/smp.h): this
 * is the bring-up proved on its own, before the kernel is made safe to run
 * on more than one. minikern.smp=off leaves them alone. */
static void bring_up_cores(void) {
    uint32_t online;
    if (cmdline_equals(boot_cmdline, "minikern.smp", "off")) {
        kprintf("smp: not starting the other cores (minikern.smp=off)\n");
        return;
    }
    online = smp_start_secondaries();
    kprintf("smp: %u of %u cores up (cpu %u is running the kernel)\n", online, SMP_MAX_CPUS,
            smp_cpu_id());
}

/* The network: the EMAC under the stack, then DHCP in the background.
 * minikern.net=off leaves it down. */
/* One hex digit, or -1. */
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Replaces the hardware address with `minikern.mac=`, if it is given.
 *
 * The address normally comes from the chip's own serial number, so a board
 * keeps it across reboots and two boards never share one. **Under QEMU
 * every machine has the same serial number**, so two guests on one segment
 * get one address between them and neither can talk to the other — which is
 * how the two-guest netboot test failed before this existed. On real
 * hardware it is the operator's override and nothing more. */
static void override_mac(uint8_t mac[6]) {
    size_t len = 0;
    const char *v = cmdline_value(boot_cmdline, "minikern.mac", &len);
    uint8_t parsed[6];
    uint32_t at = 0;
    if (v == NULL) {
        return;
    }
    for (uint32_t i = 0; i < 6u; i++) {
        int hi = at < len ? hex_digit(v[at]) : -1;
        int lo = at + 1u < len ? hex_digit(v[at + 1u]) : -1;
        if (hi < 0 || lo < 0) {
            kprintf("net: minikern.mac=\"%.*s\" is not six hex pairs, ignored\n", (int)len, v);
            return;
        }
        parsed[i] = (uint8_t)((hi << 4) | lo);
        at += 2u + (at + 2u < len && v[at + 2u] == ':' ? 1u : 0u);
    }
    for (uint32_t i = 0; i < 6u; i++) {
        mac[i] = parsed[i];
    }
}

/* A run of decimal digits, or `missing` when there are none. */
static uint32_t digits_in(const char *text, size_t len, uint32_t missing) {
    uint32_t v = 0, seen = 0;
    for (size_t i = 0; i < len && text[i] >= '0' && text[i] <= '9'; i++) {
        v = v * 10u + (uint32_t)(text[i] - '0');
        seen++;
    }
    return seen == 0 ? missing : v;
}

/* An address-valued option, or 0 when it is absent or not an address. */
static ip4_t option_address(const char *key) {
    size_t len = 0;
    const char *v = cmdline_value(boot_cmdline, key, &len);
    ip4_t addr = 0;
    if (v != NULL && ip4_parse(v, (uint32_t)len, &addr) != KERR_OK) {
        kprintf("net: %s=\"%.*s\" is not an address, ignored\n", key, (int)len, v);
        return 0;
    }
    return addr;
}

/* Brings the interface up from `minikern.ip=<addr>/<prefix>` when it is
 * given, with `minikern.gw=` and `minikern.dns=` beside it, and says so.
 * False when no address was given, which is the ordinary DHCP path.
 *
 * An address that will not parse stops the network rather than quietly
 * falling back to DHCP: a board told to use one address and silently using
 * another is worse than a board with no network. */
static bool start_static_address(void) {
    size_t len = 0;
    const char *v = cmdline_value(boot_cmdline, "minikern.ip", &len);
    ip4_t addr = 0, gateway = 0, dns = 0;
    uint32_t prefix = 24, at = 0;
    char ip[16];
    if (v == NULL) {
        return false;
    }
    while (at < len && v[at] != '/') {
        at++;
    }
    if (ip4_parse(v, at, &addr) != KERR_OK) {
        kprintf("net: minikern.ip=\"%.*s\" is not an address, leaving the network down\n", (int)len, v);
        return true;
    }
    prefix = at < len ? digits_in(v + at + 1u, len - at - 1u, 24u) : 24u;
    gateway = option_address("minikern.gw");
    dns = option_address("minikern.dns");
    knet_start_static(addr, prefix, gateway, dns); /* which prints the result */
    (void)ip;
    return true;
}

static void bring_up_network(void) {
    struct netdev_port dev = h3_emac_netdev();
    struct emac_link link;
    struct frame_sink sink;
    uint8_t mac[6];
    uint32_t xid;
    int rc;
    if (cmdline_equals(boot_cmdline, "minikern.net", "off") || dma_init() != KERR_OK) {
        return;
    }
    h3_emac_mac_from_id(chip_id, mac);
    override_mac(mac);
    sink = knet_init(mac, &dev);
    rc = h3_emac_init(mac, &sink, &link);
    if (rc != KERR_OK) {
        kprintf("net: eth0 not up (error %d)\n", rc);
        return;
    }
    kprintf("net: eth0 %02x:%02x:%02x:%02x:%02x:%02x, phy %04x:%04x at %u, link up %s %s duplex\n", mac[0],
            mac[1], mac[2], mac[3], mac[4], mac[5], link.phy_id >> 16, link.phy_id & 0xffffu, link.phy_addr,
            link.speed_100 ? "100M" : "10M", link.full_duplex ? "full" : "half");
    if (start_static_address()) {
        return;
    }
    random_fill(&xid, sizeof xid);
    knet_start(xid);
}

#define CPUFREQ_INTERVAL_MS 200u  /* decide five times a second */

/* The wall clock the kernel starts with: the board's own clock when it
 * holds a real date, and `minikern.epoch=` otherwise.
 *
 * The command line wins when it is given, because an operator who says what
 * time it is has a reason — and because that is how the tests pin a time
 * (P8.9). An unset clock reads as zeroes and is refused by the conversion
 * rather than turned into 1970-something (board/h3/rtc.h). */
static uint64_t boot_epoch(void) {
    uint64_t given = cmdline_number("minikern.epoch", 0);
    uint64_t rtc = h3_rtc_now();
    if (given != 0) {
        return given;
    }
    if (rtc != 0) {
        kprintf("rtc: %llu seconds since 1970\n", (unsigned long long)rtc);
    }
    return rtc;
}

/* Starts the watchdog if the command line asks.
 *
 * Off by default, and for a plain reason rather than an unverifiable one:
 * a board that reboots itself is the right answer for something running
 * unattended and the wrong one for something being debugged, and the kernel
 * cannot tell which it is. QEMU models this watchdog, so unlike the thermal
 * sensor and the PLLs this path is tested (`minikern.selftest=wedge`). */
static void bring_up_watchdog(void) {
    struct watchdog_port port = {.start = h3_wdt_start, .pet = h3_wdt_pet, .stop = h3_wdt_stop};
    uint32_t seconds = (uint32_t)cmdline_number("minikern.watchdog", 0);
    uint32_t got;
    if (seconds == 0) {
        return;
    }
    got = watchdog_start(&port, seconds);
    if (got == 0) {
        kprintf("watchdog: %u s is not an interval this chip offers, not started\n", seconds);
        return;
    }
    kprintf("watchdog: the board reboots if the kernel stops ticking for %u s\n", got);
}

/* The H3's published operating points, lowest first, as mainline's table
 * gives them — not invented here (the operator's instruction).
 *
 * The points above 816 MHz need the higher supply, so they are offered only
 * when the board has said which pin switches it (`minikern.vddcpux=`).
 * Offering them without that would ask for a frequency the core cannot be
 * fed at, which is the one mistake here that does not announce itself. */
static struct cpufreq_policy h3_cpu_policy(bool can_raise_vdd) {
    struct cpufreq_policy p = {
        .opps = {{480000, 1040}, {648000, 1040}, {816000, 1100}, {912000, 1200}, {1008000, 1300}},
        .nopps = can_raise_vdd ? 5u : 3u,
        .throttle_mC = 80000,
        .critical_mC = 95000,
        .hysteresis_mC = 5000,
        .load_up_pct = 80,
        .load_down_pct = 20,
    };
    return p;
}

/* Takes the VDD_CPUX pin from the command line, if it is there. True when
 * the supply can be raised, which is what decides whether the operating
 * points above 816 MHz may be offered at all.
 *
 * `drive` is false when the governor is running against fakes: the pin still
 * decides which points exist, because that decision is the thing worth
 * testing, but no register is touched. */
static bool use_vdd_pin(bool drive) {
    struct gpio_pin pin;
    size_t len = 0;
    const char *v = cmdline_value(boot_cmdline, "minikern.vddcpux", &len);
    if (v == NULL) {
        return false;
    }
    if (!gpio_parse(v, (uint32_t)len, &pin)) {
        kprintf("cpufreq: minikern.vddcpux=\"%.*s\" is not a pin, ignored\n", (int)len, v);
        return false;
    }
    if (drive) {
        h3_cpuclock_use_vdd_pin(pin);
    }
    kprintf("cpufreq: VDD_CPUX switches on P%c%u\n", 'A' + pin.port, pin.pin);
    return true;
}

/* Starts the frequency governor if the command line asks.
 *
 * It is **off by default**, and that is a statement about evidence rather
 * than about confidence: QEMU models neither the thermal sensor nor the
 * PLL, so the adapters have never run anywhere (board/h3/ths.h). `test`
 * runs the same governor and policy against a ramping fake sensor, which
 * proves everything except the two register writes; `on` drives the real
 * hardware and wants the board in front of you. */
static void bring_up_cpufreq(void) {
    struct cpufreq_policy policy;
    struct thermal_port sensor;
    struct cpuclock_port clock;
    bool fake = cmdline_equals(boot_cmdline, "minikern.cpufreq", "test");
    int rc;
    if (!fake && !cmdline_equals(boot_cmdline, "minikern.cpufreq", "on")) {
        return;
    }
    policy = h3_cpu_policy(use_vdd_pin(!fake));
    sensor = fake ? h3_thermal_fake_port() : h3_thermal_port();
    clock = fake ? h3_cpuclock_fake_port() : h3_cpuclock_port();
    rc = thermal_start(&sensor, &clock, &policy, CPUFREQ_INTERVAL_MS);
    kprintf("cpufreq: %s governor %s\n", fake ? "test" : "hardware",
            rc == KERR_OK ? "started" : "refused");
}

/* Reports how init ended, then ends the boot the way the command line asks. */
static void init_exited(void *self, const struct proc *p) {
    (void)self;
    watchdog_stop(); /* a power-off must not be interrupted by a reboot */
    /* Whatever the filesystem still holds goes to the device before the
     * machine stops (kernel/xfsfs.h). */
    (void)xfsfs_flush_all();
    if (cmdline_equals(boot_cmdline, "minikern.netstat", "1")) {
        knet_report();
        knbd_report();
        klock_report();
        thermal_report();
    }
    if ((p->exit_status & 0x7f) != 0) {
        kprintf("minikern: init killed by signal %d\n", p->exit_status & 0x7f);
    } else {
        kprintf("minikern: init exited with status %d\n", (p->exit_status >> 8) & 0xff);
    }
    if (halt_requested()) {
        finish();
    }
}

/* The init path from minikern.init=, copied into path (defaults to /init). */
static void init_path(char *path, uint32_t cap) {
    size_t len = 0;
    const char *v = cmdline_value(boot_cmdline, "minikern.init", &len);
    const char *src = v != NULL && len > 0 ? v : "/init";
    uint32_t n = v != NULL && len > 0 ? (uint32_t)len : 5u;
    for (uint32_t i = 0; i < n && i + 1 < cap; i++) {
        path[i] = src[i];
    }
    path[n < cap ? n : cap - 1] = '\0';
}

/* Applies minikern.rootfs.sha256, if it is there.
 *
 * A pin that cannot be parsed stops the boot rather than being ignored: the
 * whole point of it is that this board runs one known image, and a boot that
 * looked protected and was not would be worse than no pin. */
static bool pin_root_digest(void) {
    size_t len = 0;
    const char *v = cmdline_value(boot_cmdline, "minikern.rootfs.sha256", &len);
    if (v == NULL) {
        return true;
    }
    if (knetboot_expect(v, len) != KERR_OK) {
        kprintf("netboot: minikern.rootfs.sha256 must be 64 hex characters, refusing to boot\n");
        return false;
    }
    kprintf("netboot: accepting only the image whose sha256 is %.*s\n", (int)len, v);
    return true;
}

/* Applies minikern.netboot.key, if it is there. Like the digest pin, an
 * unusable value stops the boot: the whole point is that this board takes
 * its filesystem from one machine, and a boot that looked protected and
 * was not would be worse than none. */
static bool trust_offer_key(void) {
    size_t len = 0;
    const char *v = cmdline_value(boot_cmdline, "minikern.netboot.key", &len);
    if (v == NULL) {
        return true;
    }
    if (knetboot_require_key(v, len) != KERR_OK) {
        kprintf("netboot: minikern.netboot.key must be 64 hex characters, refusing to boot\n");
        return false;
    }
    kprintf("netboot: accepting only offers signed by %.*s\n", (int)len, v);
    return true;
}

/* The root filesystem: the netbooted one when minikern.netboot asked for
 * it and it arrived, else the initrd the loader passed. Returns false when
 * there is no root to mount. */
static bool mount_root(const struct bootinfo *info) {
    size_t len = 0;
    const char *v = cmdline_value(boot_cmdline, "minikern.netboot", &len);
    const uint8_t *image;
    uint32_t bytes;
    ip4_t pinned = 0;
    if (v != NULL && (len == 0 || ip4_parse(v, (uint32_t)len, &pinned) == KERR_OK)) {
        if (!pin_root_digest() || !trust_offer_key()) {
            return false; /* it was asked for and is unusable: do not boot */
        }
        if (knetboot_fetch(chip_id, pinned, &image, &bytes) == KERR_OK) {
            vfs_mount_root(image, bytes);
            return true;
        }
        kprintf("netboot: falling back to the initrd\n");
    } else if (v != NULL) {
        kprintf("netboot: minikern.netboot=\"%.*s\" is not an address, ignored\n", (int)len, v);
    }
    if (info->initrd_end <= info->initrd_start) {
        return false;
    }
    vfs_mount_root(phys_to_virt(info->initrd_start), info->initrd_end - info->initrd_start);
    return true;
}

/* Mounts the root filesystem and starts init; false when there is none. */
/* init's environment: the fixed entries, plus one for every
 * minikern.env.NAME=VALUE on the command line. */
static uint32_t build_env(const char **envp, uint32_t cap) {
    static char text[ENV_TEXT_BYTES];
    static const char *const fixed[] = {"HOME=/", "PATH=/bin:/", "TERM=linux"};
    const char *word;
    size_t at = 0, len = 0, used = 0;
    uint32_t n = 0;
    for (; n < 3u && n < cap; n++) {
        envp[n] = fixed[n];
    }
    for (word = cmdline_next(boot_cmdline, &at, &len); word != NULL && n < cap;
         word = cmdline_next(boot_cmdline, &at, &len)) {
        size_t prefix = sizeof ENV_PREFIX - 1u;
        if (len <= prefix || !cmdline_word_has_prefix(word, len, ENV_PREFIX) ||
            used + (len - prefix) + 1u > sizeof text) {
            continue;
        }
        envp[n++] = &text[used];
        for (size_t i = prefix; i < len; i++) {
            text[used++] = word[i];
        }
        text[used++] = '\0';
    }
    return n;
}

static bool start_init(const struct bootinfo *info) {
    static const struct proc_exit_hook hook = {.exited = init_exited};
    static char path[64];
    const char *envp[ENV_MAX];
    const char *argv[] = {path};
    uint32_t envc;
    struct proc *p;
    int rc;
    if (!mount_root(info)) {
        kprintf("minikern: no root filesystem, nothing to run\n");
        return false;
    }
    override_dns();
    attach_nbd();
    init_path(path, sizeof path);
    proc_set_exit_hook(&hook);
    envc = build_env(envp, ENV_MAX);
    rc = exec_start(path, argv, 1, envp, envc, &p);
    if (rc != KERR_OK) {
        kprintf("minikern: cannot start %s: error %d\n", path, rc);
        return false;
    }
    kprintf("minikern: started %s as pid %u\n", path, p->pid);
    return true;
}

void kmain(uint32_t r0, uint32_t machine, uint32_t r2, uint32_t entry_cpsr) {
    struct console_port uart;
    h3_uart0_init(); /* own the console rather than inherit it (board/h3/uart.h) */
    uart = h3_uart0_console();
    struct bootinfo info;
    (void)r0;
    power = psci_power_port();
    console_attach(&uart);
    kprintf("\nminikern %s.%u (NanoPi NEO / Allwinner H3), machine %u\n", MINIKERN_VERSION,
            minikern_build, machine);
    report_cpu(entry_cpsr);
    info = read_bootinfo(r2);
    boot_cmdline = info.cmdline;
    panic_attach(&power, halt_requested());
    bring_up_cpu();
    mem_init(&info, r2, info.block_size);
    report_boot(&info, r2);
    kprintf("mem: %u KiB free\n", mem_free_pages() * 4u);
    bring_up_kernel(boot_epoch());
    bring_up_console_input(&uart);
    bring_up_cores();
    bring_up_network();
    syscall_set_trace(cmdline_equals(boot_cmdline, "minikern.strace", "1"));
    sched_set_hangcheck((uint32_t)cmdline_number("minikern.hangcheck", 0));
    bring_up_cpufreq();
    bring_up_watchdog();
    maybe_selftest(boot_cmdline);
    kprintf("minikern: boot complete\n");
    if (!start_init(&info) && halt_requested()) {
        finish();
    }
    /* An idle core may power itself off rather than idle in `wfi`, which
     * on this chip saves more than stepping the clock (arch/arm/smp.h). It
     * is off unless asked for: an emulator spends no power, so there is
     * nothing here to measure. */
    smp_allow_parking(cmdline_equals(boot_cmdline, "minikern.cpuoff", "1"));
    /* The last thing the boot path does: until now it has been in the
     * kernel without the lock, and a second core in there would race it
     * (klock.h rule 4). */
    smp_release();
    sched_idle_loop();
}
