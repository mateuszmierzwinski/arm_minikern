/* The kernel's layout. It is linked at its high virtual address
 * (KERNEL_VIRT_BASE, layout.h) and loaded at its physical one: each section's
 * load address (AT) is its virtual address minus KERNEL_VIRT_OFFSET. The ELF
 * entry point is _start's *physical* address, because the boot loader jumps
 * there with the MMU off; the image rule reads it back for mkimage, so this
 * file is the only place the load address is named. 0x40008000 is DRAM base
 * + 32 KiB, the conventional ARM Linux load address. */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start_phys)

KERNEL_VIRT_OFFSET = 0x80000000;
KERNEL_VIRT_BASE = 0xC0008000;
BOOT_STACK_SIZE = 0x4000;

PHDRS {
    text PT_LOAD FLAGS(5);   /* R X */
    data PT_LOAD FLAGS(6);   /* R W */
}

SECTIONS {
    . = KERNEL_VIRT_BASE;
    __kernel_start = .;
    .text : AT(ADDR(.text) - KERNEL_VIRT_OFFSET) {
        KEEP(*(.text.boot))
        . = ALIGN(32);
        KEEP(*(.text.vectors))
        *(.text .text.*)
    } :text
    _start_phys = _start - KERNEL_VIRT_OFFSET;
    .rodata : AT(ADDR(.rodata) - KERNEL_VIRT_OFFSET) { *(.rodata .rodata.*) } :text
    .ARM.exidx : AT(ADDR(.ARM.exidx) - KERNEL_VIRT_OFFSET) { *(.ARM.exidx*) } :text
    . = ALIGN(4096);
    __text_end = .;
    .data : AT(ADDR(.data) - KERNEL_VIRT_OFFSET) { *(.data .data.*) } :data
    .bss (NOLOAD) : {
        . = ALIGN(8);
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } :data
    .pgtable (NOLOAD) : { *(.pgtable) } :data
    .stack (NOLOAD) : {
        . = ALIGN(16);
        . += BOOT_STACK_SIZE;
        __stack_top = .;
    } :data
    . = ALIGN(4096);
    __kernel_end = .;
    /DISCARD/ : { *(.comment) *(.note.*) *(.ARM.attributes) }
}
