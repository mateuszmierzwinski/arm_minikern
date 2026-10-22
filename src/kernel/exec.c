#include "kernel/exec.h"

#include "arch/arm/layout.h"
#include "arch/arm/mmu.h"
#include "core/console.h"
#include "core/elf.h"
#include "core/errno.h"
#include "core/ustack.h"
#include "kernel/devfs.h"
#include "kernel/mem.h"
#include "kernel/proc.h"
#include "kernel/random.h"
#include "kernel/vfs.h"

#define PAGE 4096u
#define USTACK_BUILD_BYTES 4096u
#define CPSR_USER 0x10u  /* user mode, IRQ and FIQ enabled, ARM state */

/* AT_HWCAP for a Cortex-A7 (Linux's HWCAP_* bit values): HALF THUMB
 * FAST_MULT VFP EDSP NEON VFPv3 TLS VFPv4 IDIVA IDIVT VFPD32 LPAE. TLS is
 * the one that matters: with it musl uses TPIDRURO and set_tls, and never
 * the kuser helper page we do not provide (CLAUDE.md §7). */
#define HWCAP_A7 (0x2u | 0x4u | 0x10u | 0x40u | 0x80u | 0x1000u | 0x2000u | 0x8000u | 0x10000u | \
                  0x20000u | 0x40000u | 0x80000u | 0x100000u)

static uint32_t page_down(uint32_t a) {
    return a & ~(PAGE - 1u);
}

static uint32_t page_up(uint32_t a) {
    return (a + PAGE - 1u) & ~(PAGE - 1u);
}

/* Copies the file bytes of one segment into its (faulted-in) pages. */
static int fill_segment(struct mm *mm, const struct elf_load *l, const uint8_t *file) {
    for (uint32_t done = 0; done < l->filesz;) {
        uint32_t va = l->vaddr + done;
        uint32_t room = PAGE - (va & (PAGE - 1u));
        uint32_t n = l->filesz - done < room ? l->filesz - done : room;
        uint8_t *k = mm_user_page(mm, va, PT_READ);
        if (k == NULL) {
            return KERR_NOMEM;
        }
        for (uint32_t i = 0; i < n; i++) {
            k[i] = file[l->offset + done + i];
        }
        if (l->prot & PT_EXEC) {
            mmu_sync_icache(k, n);
        }
        done += n;
    }
    return KERR_OK;
}

/* Regions for every segment, then their contents; the bss is lazy. */
static int load_segments(struct mm *mm, const struct elf_image *img, const uint8_t *file) {
    uint32_t top = 0;
    for (uint32_t i = 0; i < img->nloads; i++) {
        const struct elf_load *l = &img->loads[i];
        uint32_t start = page_down(l->vaddr), end = page_up(l->vaddr + l->memsz);
        int rc = vma_insert(&mm->vmas, start, end, l->prot, 0);
        if (rc == KERR_OK) {
            rc = fill_segment(mm, l, file);
        }
        if (rc != KERR_OK) {
            return rc;
        }
        top = end > top ? end : top;
    }
    mm->brk_start = mm->brk = top;
    return KERR_OK;
}

static uint32_t auxv_for(const struct elf_image *img, struct auxv_pair *aux) {
    const struct auxv_pair list[] = {
        {AT_PHDR, img->phdr_vaddr}, {AT_PHENT, img->phent}, {AT_PHNUM, img->phnum}, {AT_PAGESZ, PAGE},
        {AT_BASE, 0},  {AT_FLAGS, 0}, {AT_ENTRY, img->entry}, {AT_UID, 0}, {AT_EUID, 0}, {AT_GID, 0},
        {AT_EGID, 0},  {AT_HWCAP, HWCAP_A7}, {AT_HWCAP2, 0}, {AT_CLKTCK, 100}, {AT_SECURE, 0},
    };
    uint32_t n = sizeof list / sizeof list[0];
    for (uint32_t i = 0; i < n; i++) {
        aux[i] = list[i];
    }
    return n;
}

/* The stack region and the start state at its top; returns the initial sp. */
static int build_stack(struct mm *mm, const struct elf_image *img, const struct ustack_args *base, uint32_t *sp) {
    static uint8_t buf[USTACK_BUILD_BYTES];
    struct auxv_pair aux[USTACK_MAX_AUXV];
    struct ustack_args a = *base;
    uint32_t used;
    int rc = vma_insert(&mm->vmas, MM_STACK_TOP - MM_STACK_SIZE, MM_STACK_TOP, PT_READ | PT_WRITE, VMA_GROWSDOWN);
    a.auxv = aux;
    a.nauxv = auxv_for(img, aux);
    random_fill(a.random, sizeof a.random);
    a.platform = "v7l";
    if (rc == KERR_OK) {
        rc = ustack_build(buf, sizeof buf, MM_STACK_TOP, &a, sp, &used);
    }
    if (rc == KERR_OK) {
        rc = mm_copy_out(mm, MM_STACK_TOP - used, buf + sizeof buf - used, used);
    }
    return rc;
}

/* The trampolines a handler returns through when it has no sa_restorer
 * (Go on ARM sets none): rt_sigreturn, in ARM and in Thumb. Linux keeps
 * the same code in its per-process "sigpage". */
static int map_sigpage(struct mm *mm) {
    static const uint32_t code[] = {
        0xe3a070adu,  /* mov r7, #173 (rt_sigreturn) */
        0xef000000u,  /* svc #0 */
        0, 0,
        0xdf0027adu,  /* thumb: movs r7, #173; svc #0 */
    };
    uint32_t *k;
    int rc = vma_insert(&mm->vmas, MM_SIGPAGE, MM_SIGPAGE + PAGE, PT_READ | PT_EXEC, 0);
    if (rc != KERR_OK || (k = mm_user_page(mm, MM_SIGPAGE, PT_READ)) == NULL) {
        return rc != KERR_OK ? rc : KERR_NOMEM;
    }
    for (uint32_t i = 0; i < sizeof code / sizeof code[0]; i++) {
        k[i] = code[i];
    }
    mmu_sync_icache(k, sizeof code);
    return KERR_OK;
}

/* fds 0, 1 and 2: the console, as a terminal-less init would inherit. */
static int open_console_fds(struct proc *p) {
    for (uint32_t fd = 0; fd < 3; fd++) {
        p->fds[fd] = devfs_console_file(fd == 0 ? O_RDONLY : O_WRONLY);
        if (p->fds[fd] == NULL) {
            return KERR_NOMEM;
        }
    }
    return KERR_OK;
}

/* Reads the whole executable: tarfs files are memory-backed, in place. */
static int open_image(const char *path, const uint8_t **data, uint32_t *size, struct file **f) {
    int alias = 0;
    int rc = vfs_open(path, O_RDONLY, f, &alias);
    if (rc != KERR_OK) {
        return rc == VFS_ALIAS ? KERR_ACCES : rc;
    }
    if (((*f)->mode & S_IFMT) != S_IFREG || (*f)->data == NULL) {
        file_put(*f);
        return KERR_ACCES;
    }
    *data = (*f)->data;
    *size = (*f)->size;
    return KERR_OK;
}

/* The first thread: a user-mode trapframe at entry with the start sp. */
static int start_thread(struct proc *p, uint32_t entry, uint32_t sp) {
    struct task *t = sched_new_task(p);
    struct trapframe *tf;
    if (t == NULL) {
        return KERR_NOMEM;
    }
    tf = sched_trapframe(t);
    *tf = (struct trapframe){.pc = entry, .sp_usr = sp, .spsr = CPSR_USER};
    proc_attach_thread(p, t);
    sched_start(t);
    return KERR_OK;
}

/* Builds the process image in p from file. */
static int load_program(struct proc *p, const uint8_t *data, uint32_t size, const struct ustack_args *args,
                        uint32_t *entry, uint32_t *sp) {
    struct elf_image img;
    int rc = elf_parse(data, size, USER_TOP, &img);
    if (rc == KERR_OK) {
        rc = load_segments(p->mm, &img, data);
    }
    if (rc == KERR_OK) {
        rc = build_stack(p->mm, &img, args, sp);
    }
    if (rc == KERR_OK) {
        rc = map_sigpage(p->mm);
    }
    *entry = img.entry;
    return rc;
}

int exec_start(const char *path, const char *const *argv, uint32_t argc, const char *const *envp, uint32_t envc,
               struct proc **out) {
    struct ustack_args args = {.argv = argv, .argc = argc, .envp = envp, .envc = envc};
    const uint8_t *data;
    uint32_t size, entry = 0, sp = 0;
    struct file *f;
    struct proc *p;
    int rc = open_image(path, &data, &size, &f);
    if (rc != KERR_OK) {
        return rc;
    }
    p = proc_create(argc > 0 ? argv[0] : path);
    rc = p == NULL ? KERR_NOMEM : load_program(p, data, size, &args, &entry, &sp);
    file_put(f);
    if (rc == KERR_OK) {
        p->start_stack = sp;
        rc = open_console_fds(p);
    }
    if (rc == KERR_OK) {
        rc = start_thread(p, entry, sp);
    }
    *out = p;
    return rc;
}
