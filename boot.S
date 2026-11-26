/* DoloOS v0.2 — boot.S
 *
 * GRUB loads us in 32-bit protected mode (that's what multiboot2 does).
 * This file:
 *   1. carries the multiboot2 header GRUB searches for,
 *   2. checks we really were booted by a multiboot2 loader,
 *   3. checks the CPU can do 64-bit (long mode),
 *   4. builds tiny page tables, switches the CPU into long mode,
 *   5. jumps into 64-bit code and calls kernel_main() in kernel.c,
 *   6. carries the interrupt stubs (irq1_stub for the PS/2
 *      keyboard, isr_ignore for everything else) that kernel.c's
 *      interrupt table points at.
 *
 * After this file runs, everything else is 64-bit C.
 */

.set MULTIBOOT2_MAGIC, 0xE85250D6
.set MULTIBOOT2_ARCH, 0            /* 0 = i386 protected mode */
.set MULTIBOOT2_HEADER_LEN, multiboot_header_end - multiboot_header
/* checksum must make magic + arch + length add up to zero */
.set MULTIBOOT2_CHECKSUM, -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_HEADER_LEN)

/* --- the multiboot2 header: GRUB scans the first 32 KiB of the
 * kernel file for this magic number, so it must sit near the top. --- */
.section .multiboot_header, "a"
.align 8
multiboot_header:
    .long MULTIBOOT2_MAGIC
    .long MULTIBOOT2_ARCH
    .long MULTIBOOT2_HEADER_LEN
    .long MULTIBOOT2_CHECKSUM
    /* end tag: type 0, flags 0, size 8 */
    .short 0
    .short 0
    .long 8
multiboot_header_end:

/* --- stack: 16 KiB in .bss, grows down from stack_top --- */
.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:

/* --- page tables: identity-map the first 2 MB of memory, so the
 * kernel (loaded at 1 MB), the stack and VGA memory (0xB8000) are
 * all reachable once paging is turned on.
 *
 * NOTE (v0.2): the plan was to map 0-16 MB with eight 2 MB huge
 * pages in P2, so the page-frame allocator could own 2-16 MB.
 * That map triple-faults at the first instruction after CR0.PG
 * is set (QEMU -d int: #PF, CR2=0x80000011 — the CR0 value —
 * at the lgdt, with correct page tables verified in the QEMU
 * monitor). Mapping only P2[0] boots fine; adding P2[1] alone
 * already faults. The 16 MB map is DEFERRED to v0.3 — see
 * README.md "Known issue". The allocator below therefore owns
 * the free memory between the end of the kernel and 2 MB. --- */
.align 4096
p4_table:
    .skip 4096
p3_table:
    .skip 4096
p2_table:
    .skip 4096

.section .text
.global _start
.code32
_start:
    /* GRUB hands us: eax = multiboot2 magic, ebx = info pointer */
    movl $stack_top, %esp

    /* 1. did a multiboot2 loader really boot us? */
    cmpl $0x36D76289, %eax
    jne .no_multiboot

    /* 2. does this CPU do 64-bit? */
    call check_cpuid
    call check_long_mode

    /* 3. build the page tables:
     *    P4[0] -> P3, P3[0] -> P2, P2[0] = one 2 MB "huge page"
     *    covering addresses 0x0 - 0x200000 (2 MB).
     *    Flags 0b11 = present + writable; the 0x80 bit in the P2
     *    entry marks it as a huge page. (See the note above the
     *    page tables: the 16 MB map is deferred to v0.3.) */
    movl $p3_table, %eax
    orl $0b11, %eax
    movl %eax, p4_table

    movl $p2_table, %eax
    orl $0b11, %eax
    movl %eax, p3_table

    movl $0x00000083, p2_table     /* one 2 MB huge page, addr 0 */

    /* 4. enable PAE, point CR3 at the P4 table, set the long-mode
     *    bit in the EFER register, then turn paging on. */
    movl %cr4, %eax
    orl $1 << 5, %eax              /* CR4.PAE */
    movl %eax, %cr4

    movl $p4_table, %eax
    movl %eax, %cr3

    movl $0xC0000080, %ecx         /* EFER model-specific register */
    rdmsr
    orl $1 << 8, %eax              /* EFER.LME = long mode enable */
    wrmsr

    movl %cr0, %eax
    orl $1 << 31, %eax             /* CR0.PG = paging on */
    movl %eax, %cr0

    /* 5. load the 64-bit GDT and far-jump into long mode.
     *    Selector 0x08 = the code segment in gdt64 below. */
    lgdt (gdt64_pointer)
    ljmp $0x08, $long_mode_start

.no_multiboot:
    movl $err_no_multiboot, %esi
    call vga_print_error
    jmp halt_loop

/* --- error helper: prints a zero-terminated string at %esi to the
 * VGA text screen in white-on-red, then never returns. --- */
vga_print_error:
    movl $0xB8000, %edi
    movb $0x4F, %ah                /* white text on red background */
1:
    lodsb
    testb %al, %al
    jz 2f
    stosw
    jmp 1b
2:
    ret

/* --- check the CPU has the CPUID instruction (flip the ID flag in
 * EFLAGS and see if it sticks). --- */
check_cpuid:
    pushfl
    pop %eax
    movl %eax, %ecx
    xorl $1 << 21, %eax            /* flip bit 21 (ID) */
    pushl %eax
    popfl
    pushfl
    pop %eax
    pushl %ecx
    popfl
    cmpl %ecx, %eax
    je .no_cpuid
    ret
.no_cpuid:
    movl $err_no_cpuid, %esi
    call vga_print_error
    jmp halt_loop

/* --- check the CPU supports 64-bit long mode (CPUID leaf
 * 0x80000001, EDX bit 29). --- */
check_long_mode:
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb .no_long_mode
    movl $0x80000001, %eax
    cpuid
    testl $1 << 29, %edx           /* LM bit */
    jz .no_long_mode
    ret
.no_long_mode:
    movl $err_no_long_mode, %esi
    call vga_print_error
    jmp halt_loop

halt_loop:
    hlt
    jmp halt_loop

/* --- 64-bit GDT: a null entry, a 64-bit code segment and a data
 * segment. In long mode the CPU mostly ignores segment bases, but
 * it still needs a code segment with the L (64-bit) bit set. --- */
.section .rodata
gdt64:
    .quad 0x0000000000000000        /* null */
    .quad 0x00209A0000000000        /* code: present, exec/read, 64-bit */
    .quad 0x0000920000000000        /* data: present, read/write */
gdt64_pointer:
    .word gdt64_pointer_end - gdt64 - 1
    .long gdt64
gdt64_pointer_end:

.section .rodata
err_no_multiboot:
    .asciz "ERROR: not booted by a multiboot2 loader"
err_no_cpuid:
    .asciz "ERROR: CPU has no CPUID instruction"
err_no_long_mode:
    .asciz "ERROR: CPU does not support 64-bit long mode"

/* --- we are in long mode now: set up the data segments and the
 * 64-bit stack, then hand over to C. --- */
.section .text
.code64
long_mode_start:
    movw $0x10, %ax                /* 0x10 = data segment in gdt64 */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    movq $stack_top, %rsp
    call kernel_main
    jmp halt_loop

/* --- interrupt stubs (64-bit). The CPU jumps here when an
 * interrupt fires; the IDT in kernel.c points at these.
 *
 * irq1_stub is IRQ1, the PS/2 keyboard (vector 33 after the PIC
 * remap in kernel.c). It saves every register C code is allowed
 * to clobber, calls the C handler keyboard_irq_handler(), tells
 * the PIC the interrupt is handled (EOI = end of interrupt),
 * restores the registers and returns from the interrupt with
 * iretq — which is NOT the same as ret: it also restores the
 * flags and the code segment the CPU saved on entry.
 *
 * isr_ignore is the "nothing to do" handler every other IDT
 * entry points at, so a stray interrupt just returns instead
 * of crashing the kernel. --- */
.global irq1_stub
.global isr_ignore
irq1_stub:
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    call keyboard_irq_handler
    movb $0x20, %al
    outb %al, $0x20                /* EOI to the master PIC */
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rax
    iretq

isr_ignore:
    iretq

/* Tell the linker the stack must NOT be executable. Without this
 * note, ld warns about an RWX load segment. */
.section .note.GNU-stack,"",@progbits
