/* DoloOS v0.1 — linker.ld
 *
 * Tells the linker where each part of the kernel goes in memory.
 *
 * GRUB loads the kernel at address 1 MB (0x100000) — the classic
 * spot, just above the first megabyte of memory that the BIOS and
 * VGA hardware reserve. The multiboot2 header MUST come first in
 * the file, because GRUB only searches the first 32 KiB for it.
 */

ENTRY(_start)

/* Separate load segments so code is read+execute and data is
 * read+write (instead of one big RWX segment). */
PHDRS {
    text PT_LOAD FLAGS(5);   /* R + X */
    data PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS {
    . = 1M;

    .multiboot_header : {
        *(.multiboot_header)
    } :text

    .text : {
        *(.text)
    } :text

    .rodata : {
        *(.rodata)
    } :text

    .data : {
        *(.data)
    } :data

    .bss : {
        *(COMMON)
        *(.bss)
    } :data

    /* First address past the kernel image. The page-frame
     * allocator in kernel.c owns the free memory from here
     * (rounded up to a page) to the top of the 2 MB map. */
    kernel_end = .;
}
