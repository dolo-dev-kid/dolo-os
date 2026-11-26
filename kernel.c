/* DoloOS v0.2 — kernel.c
 *
 * What v0.1 had: VGA printing + serial printing, then halt.
 * What v0.2 adds:
 *
 *   - Interrupts: an Interrupt Descriptor Table (IDT) and a remap
 *     of the 8259 PIC, so hardware can interrupt the CPU. IRQ1
 *     (the PS/2 keyboard) lands on vector 33.
 *   - A keyboard driver: the IRQ1 handler reads scancodes from
 *     port 0x60, turns them into ASCII with a US-layout table
 *     (Shift and Caps Lock work, Backspace and Enter work) and
 *     drops the characters into a small ring buffer.
 *   - A shell: a "dolo> " prompt that reads lines from the ring
 *     buffer and runs commands — help, about, ver, mem, clear.
 *   - Memory management, started: a page-frame allocator. It
 *     hands out 4 KB pages from the free memory between the end
 *     of the kernel and the top of the 2 MB identity map (which
 *     boot.S sets up) and tracks them with a bitmap. The `mem`
 *     command shows how many pages are free. No heap (malloc)
 *     yet — that's v0.3, along with the 16 MB map (see the
 *     "Known issue" in README.md for why the map is still 2 MB).
 *
 * The interrupt handler itself does almost nothing: it only reads
 * the scancode and enqueues a character. All the real work
 * (echoing, running commands) happens in the shell's main loop,
 * so an interrupt can never catch the kernel halfway through
 * printing.
 */

#include <stdint.h>
#include <stddef.h>

#define DOLOOS_VERSION "0.2"

/* ---------- tiny helpers for talking to hardware ports ---------- */

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* ---------- VGA text mode (the screen) ---------- */

static volatile uint16_t *const VGA = (uint16_t *)0xB8000;
static const int VGA_COLS = 80;
static const int VGA_ROWS = 25;
static int vga_row = 0;
static int vga_col = 0;

/* color byte: background (high nibble) + foreground (low nibble).
 * 0x0E = gold/yellow text on black. 0x07 = light grey on black. */
#define VGA_GOLD 0x0E
#define VGA_GREY 0x07

/* Move every line up one row and blank the bottom row. */
static void vga_scroll(void) {
    for (int row = 1; row < VGA_ROWS; row++) {
        for (int col = 0; col < VGA_COLS; col++) {
            VGA[(row - 1) * VGA_COLS + col] = VGA[row * VGA_COLS + col];
        }
    }
    for (int col = 0; col < VGA_COLS; col++) {
        VGA[(VGA_ROWS - 1) * VGA_COLS + col] = (uint16_t)VGA_GREY << 8 | ' ';
    }
    vga_row = VGA_ROWS - 1;
}

static void vga_clear(void) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        VGA[i] = (uint16_t)VGA_GREY << 8 | ' ';
    }
    vga_row = 0;
    vga_col = 0;
}

static void vga_putc(char c, uint8_t color) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else {
        VGA[vga_row * VGA_COLS + vga_col] = (uint16_t)color << 8 | (uint8_t)c;
        vga_col++;
        if (vga_col >= VGA_COLS) {
            vga_col = 0;
            vga_row++;
        }
    }
    if (vga_row >= VGA_ROWS) {
        vga_scroll();
    }
}

/* Erase one character to the left (for Backspace). Only works
 * inside the current line — at column 0 there is nothing to erase. */
static void vga_backspace(void) {
    if (vga_col > 0) {
        vga_col--;
        VGA[vga_row * VGA_COLS + vga_col] = (uint16_t)VGA_GREY << 8 | ' ';
    }
}

static void vga_puts(const char *s, uint8_t color) {
    for (size_t i = 0; s[i] != '\0'; i++) {
        vga_putc(s[i], color);
    }
}

/* ---------- serial port COM1 (for the boot log) ---------- */

#define COM1 0x3F8

static void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* disable interrupts */
    outb(COM1 + 3, 0x80);   /* enable divisor latch (set baud rate) */
    outb(COM1 + 0, 0x03);   /* divisor low byte: 38400 baud */
    outb(COM1 + 1, 0x00);   /* divisor high byte */
    outb(COM1 + 3, 0x03);   /* 8 data bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xC7);   /* enable FIFO, clear it, 14-byte threshold */
    outb(COM1 + 4, 0x0B);   /* IRQs on, RTS + DSR set */
}

static void serial_putc(char c) {
    /* wait until the transmitter is empty (line status bit 5) */
    while ((inb(COM1 + 5) & 0x20) == 0) {
        /* spin */
    }
    outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s) {
    for (size_t i = 0; s[i] != '\0'; i++) {
        serial_putc(s[i]);
    }
}

/* ---------- print to BOTH the screen and the serial log ---------- */

static void kputc(char c, uint8_t color) {
    vga_putc(c, color);
    serial_putc(c);
}

static void kprint(const char *s, uint8_t color) {
    vga_puts(s, color);
    serial_puts(s);
}

static void kprintln(const char *s, uint8_t color) {
    kprint(s, color);
    kputc('\n', color);
}

static void kprint_num(uint64_t n, uint8_t color) {
    char buf[21];
    int i = 0;
    if (n == 0) {
        kprint("0", color);
        return;
    }
    while (n > 0) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (i > 0) {
        i--;
        kputc(buf[i], color);
    }
}

/* ---------- the interrupt table (IDT) ----------
 *
 * The CPU needs a table of 256 entries, one per interrupt vector.
 * Each entry says where to jump (a stub in boot.S) and which code
 * segment to use. We point every entry at isr_ignore except
 * vector 33 (IRQ1, the keyboard), which points at irq1_stub.
 *
 * Why remap the PIC? Out of reset, the 8259 PIC maps hardware
 * IRQs 0-15 onto CPU vectors 8-15 — which collide with the CPU's
 * own exception vectors (a double fault is vector 8). So we
 * reprogram it: IRQs 0-7 become vectors 32-39, IRQs 8-15 become
 * 40-47. The keyboard is IRQ1, so it arrives as vector 33.
 */

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;         /* interrupt stack table: 0 = use current stack */
    uint8_t  type_attr;   /* 0x8E = present, ring 0, 64-bit interrupt gate */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idt_p;

extern void irq1_stub(void);
extern void isr_ignore(void);

static void idt_set_gate(int n, void *handler) {
    uint64_t addr = (uint64_t)handler;
    idt[n].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[n].selector    = 0x08;   /* kernel code segment from boot.S's GDT */
    idt[n].ist         = 0;
    idt[n].type_attr   = 0x8E;
    idt[n].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[n].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[n].zero        = 0;
}

static void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, isr_ignore);
    }
    idt_set_gate(33, irq1_stub);   /* IRQ1 = PS/2 keyboard */
    idt_p.limit = sizeof(idt) - 1;
    idt_p.base  = (uint64_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idt_p));
}

static void pic_remap(void) {
    /* ICW1: start init, expect ICW4 */
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    /* ICW2: vector offsets — master 32, slave 40 */
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    /* ICW3: master has a slave on IRQ2; slave's cascade identity is 2 */
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    /* ICW4: 8086/88 mode */
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    /* masks: only IRQ1 (keyboard) unmasked on the master;
     * everything masked on the slave (we use no slave IRQs). */
    outb(0x21, 0xFD);
    outb(0xA1, 0xFF);
}

/* ---------- the PS/2 keyboard driver ----------
 *
 * The keyboard talks in "scancodes" (set 1, US layout): one byte
 * per key press, and the same byte + 0x80 when the key is
 * released. The IRQ handler reads the scancode from port 0x60
 * and turns it into an ASCII character with the tables below.
 * Shift keys and Caps Lock change what a letter becomes;
 * Backspace and Enter become their control characters; the
 * 0xE0 prefix (arrow keys, etc.) is ignored for now.
 *
 * Characters land in a 256-byte ring buffer. The shell reads
 * them out with kbd_pop(). If the buffer is full, new keys are
 * dropped rather than overwriting — better to lose a keystroke
 * than to corrupt the line being typed.
 */

#define KBD_BUF_SIZE 256
static volatile uint8_t kbd_buf[KBD_BUF_SIZE];
static volatile int kbd_head = 0;
static volatile int kbd_tail = 0;

static void kbd_push(char c) {
    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next == kbd_tail) {
        return;   /* buffer full — drop the keystroke */
    }
    kbd_buf[kbd_head] = (uint8_t)c;
    kbd_head = next;
}

static int kbd_pop(char *c) {
    if (kbd_head == kbd_tail) {
        return 0;   /* empty */
    }
    *c = (char)kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return 1;
}

/* scancode set 1 -> ASCII, no Shift held. Index = scancode.
 * 0 means "not a printable key" (Shift, Ctrl, F-keys, ...). */
static const char kbd_map[128] = {
    /* 0x00-0x0F */ 0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,   0,
    /* 0x10-0x1F */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0,   0,   'a', 's',
    /* 0x20-0x2F */ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\','z', 'x', 'c', 'v',
    /* 0x30-0x3F */ 'b', 'n', 'm', ',', '.', '/', 0,   0,   0,   ' ', 0,   0,   0,   0,   0,   0,
    /* 0x40-0x4F */ 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    /* 0x50-0x5F */ 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    /* 0x60-0x6F */ 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    /* 0x70-0x7F */ 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

/* same table, with Shift held */
static const char kbd_map_shift[128] = {
    /* 0x00-0x0F */ 0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0,   0,
    /* 0x10-0x1F */ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0,   0,   'A', 'S',
    /* 0x20-0x2F */ 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    /* 0x30-0x3F */ 'B', 'N', 'M', '<', '>', '?', 0,   0,   0,   ' ', 0,   0,   0,   0,   0,   0,
    /* 0x40-0x4F */ 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    /* 0x50-0x5F */ 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    /* 0x60-0x6F */ 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    /* 0x70-0x7F */ 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

static volatile int kbd_shift = 0;   /* bitmask: 1 = left Shift, 2 = right Shift */
static volatile int kbd_caps = 0;    /* Caps Lock toggled on */
static volatile int kbd_ext = 0;     /* last byte was the 0xE0 prefix */

static char kbd_translate(uint8_t sc, int shift, int caps) {
    char c;
    if (sc >= 128) {
        return 0;
    }
    c = shift ? kbd_map_shift[sc] : kbd_map[sc];
    /* Caps Lock flips the case of letters only (and cancels
     * Shift on letters, like a real keyboard does). */
    if (caps) {
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        } else if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
    }
    return c;
}

/* Called from irq1_stub in boot.S. Runs with interrupts already
 * off for this vector; all it does is read the scancode and
 * enqueue one character (or none). */
void keyboard_irq_handler(void) {
    uint8_t status = inb(0x64);
    if ((status & 0x01) == 0) {
        return;   /* output buffer empty — nothing to read */
    }
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) {
        kbd_ext = 1;   /* next byte belongs to an extended key — ignore it */
        return;
    }
    if (sc & 0x80) {
        /* key release: only Shift matters, and only to turn it off */
        uint8_t key = (uint8_t)(sc & 0x7F);
        if (!kbd_ext) {
            if (key == 0x2A) kbd_shift &= ~1;
            if (key == 0x36) kbd_shift &= ~2;
        }
        kbd_ext = 0;
        return;
    }
    kbd_ext = 0;

    if (sc == 0x2A) { kbd_shift |= 1; return; }   /* left Shift down */
    if (sc == 0x36) { kbd_shift |= 2; return; }   /* right Shift down */
    if (sc == 0x3A) { kbd_caps = !kbd_caps; return; }  /* Caps Lock toggles */
    if (sc == 0x0E) { kbd_push('\b'); return; }    /* Backspace */
    if (sc == 0x1C) { kbd_push('\n'); return; }    /* Enter */

    char c = kbd_translate(sc, kbd_shift != 0, kbd_caps);
    if (c) {
        kbd_push(c);
    }
}

/* ---------- memory management: the page-frame allocator ----------
 *
 * The kernel hands out memory in 4 KB pages. The allocator owns
 * the free memory from the end of the kernel image (rounded up
 * to a page boundary) to the top of the 2 MB identity map, and
 * tracks each page with one bit in a bitmap: 0 = free, 1 = used.
 * That is roughly 1 MB, a couple of hundred pages — small, but a
 * real allocator. It is small because boot.S only maps the first
 * 2 MB right now (the 16 MB map triple-faults; see README.md
 * "Known issue" and the note above the page tables in boot.S).
 * Once the map grows in v0.3, the allocator's end moves with it
 * and this same code hands out the bigger region. No heap yet:
 * nothing in the kernel calls pmm_alloc() to build structures
 * on top of it. That is v0.3's job.
 */

extern char kernel_end[];   /* first address past the kernel, linker.ld */

#define PMM_END    0x200000UL  /* top of the 2 MB identity map */
#define PMM_PAGE   4096
/* Worst case: the kernel image is tiny, so the free region below
 * 2 MB can never hold more than 512 pages. The bitmap is sized
 * for that; the real count is computed at boot. */
#define PMM_MAX_PAGES  512

static uint8_t pmm_bitmap[PMM_MAX_PAGES / 8];   /* 64 bytes, in .bss */
static uint64_t pmm_start;   /* first allocatable page, set by pmm_init */
static size_t pmm_pages;     /* how many pages the region holds */

static void pmm_init(void) {
    pmm_start = ((uint64_t)kernel_end + PMM_PAGE - 1) & ~(uint64_t)(PMM_PAGE - 1);
    pmm_pages = (size_t)((PMM_END - pmm_start) / PMM_PAGE);
    if (pmm_pages > PMM_MAX_PAGES) {
        pmm_pages = PMM_MAX_PAGES;
    }
    for (size_t i = 0; i < sizeof(pmm_bitmap); i++) {
        pmm_bitmap[i] = 0;
    }
}

/* Returns the address of a free 4 KB page, or 0 if none is free. */
static uint64_t pmm_alloc(void) {
    for (size_t i = 0; i < pmm_pages; i++) {
        if ((pmm_bitmap[i / 8] & (uint8_t)(1u << (i % 8))) == 0) {
            pmm_bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
            return pmm_start + i * PMM_PAGE;
        }
    }
    return 0;
}

static void pmm_free(uint64_t addr) {
    if (addr < pmm_start || addr >= pmm_start + pmm_pages * PMM_PAGE) {
        return;
    }
    size_t i = (size_t)((addr - pmm_start) / PMM_PAGE);
    pmm_bitmap[i / 8] &= (uint8_t)~(1u << (i % 8));
}

static size_t pmm_free_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < pmm_pages; i++) {
        if ((pmm_bitmap[i / 8] & (uint8_t)(1u << (i % 8))) == 0) {
            n++;
        }
    }
    return n;
}

/* ---------- the shell ---------- */

#define SHELL_BUF 128
static char shell_line[SHELL_BUF];
static int shell_len = 0;

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static void shell_prompt(void) {
    kprint("dolo> ", VGA_GOLD);
}

static void shell_exec(const char *cmd) {
    if (cmd[0] == '\0') {
        return;   /* empty line — just a fresh prompt */
    }
    if (streq(cmd, "help")) {
        kprintln("Commands:", VGA_GREY);
        kprintln("  help  - show this list", VGA_GREY);
        kprintln("  about - what DoloOS is", VGA_GREY);
        kprintln("  ver   - show the version", VGA_GREY);
        kprintln("  mem   - show memory (page-frame allocator)", VGA_GREY);
        kprintln("  clear - clear the screen", VGA_GREY);
    } else if (streq(cmd, "about")) {
        kprintln("DoloOS is a real operating system kernel, built", VGA_GREY);
        kprintln("from scratch by Wesley Robinson (Dolo45). No", VGA_GREY);
        kprintln("Linux, no Windows underneath - this code IS the", VGA_GREY);
        kprintln("operating system. v0.2 adds a keyboard driver", VGA_GREY);
        kprintln("and this shell. Create Nothing But Bangas.", VGA_GOLD);
    } else if (streq(cmd, "ver")) {
        kprintln("DoloOS v0.2 - kernel 0.2, shell 1.0", VGA_GREY);
    } else if (streq(cmd, "mem")) {
        kprint("Memory: ", VGA_GREY);
        kprint_num(pmm_free_count(), VGA_GREY);
        kprint(" of ", VGA_GREY);
        kprint_num(pmm_pages, VGA_GREY);
        kprintln(" pages free (4 KB each)", VGA_GREY);
    } else if (streq(cmd, "clear")) {
        vga_clear();
        serial_puts("[screen cleared]\n");
    } else {
        kprint("unknown command: ", VGA_GREY);
        kprintln(cmd, VGA_GREY);
        kprintln("Try 'help'.", VGA_GREY);
    }
}

static void shell_run(void) {
    shell_prompt();
    for (;;) {
        char c;
        /* Wait for a key. The cli/sti/hlt dance is the safe way
         * to sleep: check the buffer with interrupts off, then
         * re-enable them and halt in the same breath, so a key
         * that arrives in between can still wake us up. */
        __asm__ volatile ("cli");
        int have = kbd_pop(&c);
        if (!have) {
            __asm__ volatile ("sti");
            __asm__ volatile ("hlt");
            continue;
        }
        __asm__ volatile ("sti");

        if (c == '\n') {
            shell_line[shell_len] = '\0';
            kputc('\n', VGA_GREY);
            shell_exec(shell_line);
            shell_len = 0;
            shell_prompt();
        } else if (c == '\b') {
            if (shell_len > 0) {
                shell_len--;
                vga_backspace();
                serial_puts("\b \b");
            }
        } else {
            if (shell_len < SHELL_BUF - 1) {
                shell_line[shell_len++] = c;
                kputc(c, VGA_GREY);
            }
        }
    }
}

/* ---------- the kernel's front door ---------- */

void kernel_main(void) {
    vga_clear();
    serial_init();

    /* the screen gets color, the serial log gets the same words */
    vga_puts("DoloOS v0.2\n", VGA_GOLD);
    vga_puts("Create Nothing But Bangas\n", VGA_GREY);
    vga_puts("Kernel up. v0.2 - it boots and types.\n", VGA_GREY);
    serial_puts("DoloOS v0.2\n");
    serial_puts("Create Nothing But Bangas\n");
    serial_puts("Kernel up. v0.2 - it boots and types.\n");

    /* memory: allocator up, then a quick self-test — grab a page,
     * write both ends of it, read them back, give it back. */
    pmm_init();
    uint64_t test_page = pmm_alloc();
    int mem_ok = 0;
    if (test_page) {
        volatile uint8_t *p = (uint8_t *)test_page;
        p[0] = 0xD0;
        p[PMM_PAGE - 1] = 0x45;
        mem_ok = (p[0] == 0xD0 && p[PMM_PAGE - 1] == 0x45);
        pmm_free(test_page);
    }
    kprint("memory: ", VGA_GREY);
    kprint_num(pmm_pages, VGA_GREY);
    kprint(" pages, self-test ", VGA_GREY);
    kprintln(mem_ok ? "OK" : "FAILED", mem_ok ? VGA_GREY : VGA_GOLD);

    /* interrupts: table up, PIC remapped, keyboard unmasked,
     * then turn interrupts on for real. */
    idt_init();
    pic_remap();
    __asm__ volatile ("sti");

    kprintln("Type 'help' and press Enter.", VGA_GREY);

    /* from here on the kernel IS the shell. It never returns. */
    shell_run();

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
