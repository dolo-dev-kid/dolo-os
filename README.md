# DoloOS v0.2

A real operating system kernel — small, but real. It boots on a virtual
computer (QEMU), prints to the screen, **and now it talks back**: type
on the keyboard and a shell answers. No Linux underneath it, no
Windows, nothing. This code *is* the operating system.

> **DoloOS v0.2**
> **Create Nothing But Bangas**

---

## Wait — what even is a kernel?

Your computer is hardware: a CPU, memory, a screen, a keyboard. On its
own the hardware does nothing useful. A **kernel** is the first program
that runs when a computer turns on. Its job is to:

1. Take control of the hardware,
2. Set up the rules (memory, CPU modes, devices),
3. Run the programs you actually want (apps, games, a shell).

Everything else — your desktop, your apps, your music — sits on top of
the kernel. Windows, Linux, macOS and Android are all kernels plus a
huge pile of programs built on top of them. DoloOS v0.2 is the second
step: a kernel that boots, prints, **and listens to the keyboard**.
That's how every OS starts.

---

## How does a computer boot into DoloOS?

Turning on a computer is a relay race. Each runner hands off to the next:

```
Power on
  → BIOS / UEFI  (firmware built into the motherboard)
  → GRUB         (the bootloader, lives on the disk)
  → DoloOS kernel (this code!)
```

1. **BIOS/UEFI** wakes up the hardware and looks for something to boot
   from — in our case, the ISO file.
2. **GRUB** (the bootloader) reads the ISO, finds our kernel, and checks
   for a special number at the top of the kernel file called the
   **multiboot2 header**. That header is a promise: "I am a kernel, load
   me at this address, start me here." GRUB loads the kernel into memory
   at address 1 MB and jumps to it.
3. **boot.S** (assembly) takes over. The CPU is still in 32-bit mode,
   so this file switches it into 64-bit "long mode": it builds page
   tables (the CPU's map of memory), turns on paging, loads a 64-bit
   segment table (GDT), and jumps to `kernel_main()`. It also carries
   the interrupt stubs the keyboard handler needs.
4. **kernel.c** (C) runs. It sets up the interrupt table (IDT) and
   remaps the interrupt controller (PIC) so the keyboard's interrupt
   (IRQ1) reaches the kernel, starts a page-frame allocator, prints
   the banner, and then becomes the shell: a `dolo> ` prompt that
   reads what you type and runs commands.

---

## Using the shell

Boot it and you'll see:

```
DoloOS v0.2
Create Nothing But Bangas
Kernel up. v0.2 - it boots and types.
memory: 245 pages, self-test OK
Type 'help' and press Enter.
dolo>
```

Type a command and press Enter. The shell echoes what you type as
you type it.

| Command | What it does |
|---|---|
| `help` | Lists all the commands. |
| `about` | What DoloOS is and who built it. |
| `ver` | Prints the version (`DoloOS v0.2 - kernel 0.2, shell 1.0`). |
| `mem` | Shows memory: how many 4 KB pages the page-frame allocator has free, out of how many it owns. |
| `clear` | Clears the screen (and notes it in the serial log). |

Anything else gets `unknown command: <what you typed>` and a hint to
try `help`. Backspace deletes the last character you typed.

---

## How the keyboard works

A keyboard doesn't hand the CPU letters — it sends **scancodes**,
one number per key press, over the old PS/2 port. The kernel has to
turn those numbers into letters itself. The chain in v0.2:

1. You press a key. The keyboard sends a scancode to port `0x60`
   and raises **IRQ1** (interrupt request 1).
2. The **PIC** (the chip that manages interrupts) was remapped by the
   kernel so IRQ1 lands on interrupt vector 33 instead of colliding
   with the CPU's own error vectors.
3. The CPU looks up vector 33 in the **IDT** (interrupt descriptor
   table) and jumps to `irq1_stub` in `boot.S`, which saves the
   registers, calls the C handler, tells the PIC "handled" (EOI),
   restores the registers, and returns with `iretq`.
4. The C handler `keyboard_irq_handler()` reads the scancode from
   port `0x60`, looks it up in a US-layout table (Shift and Caps
   Lock change which letter you get; Backspace becomes `\b`, Enter
   becomes `\n`), and drops the character into a 256-byte ring
   buffer. That's all it does — the real work happens in the shell
   loop, so an interrupt can never catch the kernel halfway through
   printing.
5. The shell reads characters out of the ring buffer, echoes them,
   and runs the command when it sees Enter.

The keyboard only works when interrupts are on (`sti` in
`kernel_main`, after the IDT and PIC are set up). Before that point
the kernel is deaf — which is fine, because nothing is asking for
input yet.

---

## Memory management, started

v0.2 also starts memory management: a **page-frame allocator**. The
kernel hands out memory in 4 KB pages. The allocator owns the free
memory between the end of the kernel image and the top of the 2 MB
identity map — about 1 MB, 245 pages on this build — and tracks each
page with one bit in a bitmap (0 = free, 1 = used). At boot it runs a
self-test: grab a page, write both ends of it, read them back, give
it back. `mem` shows how many pages are free.

There is no heap yet (`malloc` for the kernel) — nothing in the
kernel calls `pmm_alloc()` to build structures on top of the
allocator. That is v0.3's job, along with growing the memory map
past 2 MB (see "Known issue" below).

---

## The files

| File | What it does |
|---|---|
| `boot.S` | Multiboot2 header + the assembly that switches the CPU from 32-bit to 64-bit long mode. Also checks the CPU can actually do 64-bit, prints a red error to the screen if anything is wrong, and carries the interrupt stubs (`irq1_stub` for the keyboard, `isr_ignore` for everything else). |
| `kernel.c` | The kernel itself. VGA + serial printing, the IDT and PIC setup, the PS/2 keyboard driver (scancode → ASCII, Shift/Caps Lock/Backspace/Enter), the shell (`dolo> `, help/about/ver/mem/clear), and the page-frame allocator. |
| `linker.ld` | Tells the linker where everything goes in memory: kernel at 1 MB, multiboot header first, code separate from data. Also defines `kernel_end`, the first address past the kernel, which the allocator uses. |
| `Makefile` | Builds it all. `make` = build the kernel, `make iso` = make a bootable ISO, `make run` = boot it in QEMU and show the log, `make test` = boot it in QEMU and drive the keyboard automatically (see below). |
| `grub.cfg` | GRUB's menu: one entry, "DoloOS v0.2", that loads the kernel with multiboot2. |
| `boot-test-v02.py` | The automated test: boots QEMU, types `help`, `about`, `ver`, `mem`, `clear`, a backspace test and an unknown command through the QEMU monitor's `sendkey`, then checks the serial log and the screen dump. 13 checks, all must pass. |
| `doloos.iso` | The bootable disk image `make iso` produces. This is what QEMU (or a real computer) boots from. |
| `boot.log` | Proof it booted — everything the kernel printed to the serial port, saved to a file by QEMU. |
| `screen.png` | Proof of what the screen looked like — a VGA screendump from the test run. |

---

## Build it

You need: `gcc`, `binutils` (as/ld), `grub-mkrescue`, `xorriso`,
`qemu-system-x86_64`. On Ubuntu:

```sh
sudo apt-get install -y gcc binutils grub-common xorriso qemu-system-x86
```

Then:

```sh
make        # builds doloos.kernel
make iso    # builds doloos.iso (bootable, with GRUB)
```

A clean build prints the compile lines and nothing else — no errors,
no warnings.

---

## Boot it

```sh
make run
```

That boots the ISO in QEMU with no window and saves what the kernel
prints to `boot.log`. You should see:

```
DoloOS v0.2
Create Nothing But Bangas
Kernel up. v0.2 - it boots and types.
memory: 245 pages, self-test OK
Type 'help' and press Enter.
dolo>
```

### How to type in QEMU

**With a window** (the easy way — click into the window and type):

```sh
qemu-system-x86_64 -cdrom doloos.iso -serial stdio
```

The gold `dolo> ` prompt is on the VGA screen; the same words also
go out the serial port. Click inside the QEMU window to give it the
keyboard, type a command, press Enter. (To get your mouse back, press
Ctrl+Alt.)

**Without a window** (headless, like `make run`): the QEMU monitor
can send keys for you. Start QEMU with a monitor socket:

```sh
qemu-system-x86_64 -cdrom doloos.iso -serial file:boot.log \
    -display none -monitor unix:/tmp/mon.sock,server,nowait
```

then, from another terminal, send keys with `sendkey` (one key at a
time, with a small pause between them):

```sh
# type "help" and press Enter
echo "sendkey h" | socat - unix:/tmp/mon.sock
echo "sendkey e" | socat - unix:/tmp/mon.sock
echo "sendkey l" | socat - unix:/tmp/mon.sock
echo "sendkey p" | socat - unix:/tmp/mon.sock
echo "sendkey ret" | socat - unix:/tmp/mon.sock
```

`boot-test-v02.py` does exactly this, automatically, and checks the
results — that's what `make test` runs.

### Run the automated test

```sh
make test
```

It boots QEMU, types `help`, `about`, `ver`, `mem`, `clear`, a
backspace test (`helx` + backspace + `p` → `help`) and an unknown
command (`xyzzy`), then checks the serial log and the screen dump.
All 13 checks must pass. On this machine they do:

```
PASS - banner version
PASS - banner tagline
PASS - memory self-test
PASS - help hint
PASS - shell prompt
PASS - help output
PASS - help lists clear
PASS - about output
PASS - ver output
PASS - mem output
PASS - clear ran
PASS - unknown command
PASS - help ran twice (backspace test)
```

(On a real computer you could burn the ISO to a USB stick and boot it
for real — v0.2 only prints and reads the keyboard, so it's safe to
try, but save that for later.)

---

## Known issue — the 16 MB memory map (deferred to v0.3)

The plan for v0.2 was to identity-map the first 16 MB with eight
2 MB huge pages in the P2 table, so the allocator could own 2–16 MB.
That map **triple-faults**: QEMU resets in a loop as soon as paging
is turned on. What was established while debugging it:

- The page tables are correct — verified byte-for-byte in the QEMU
  monitor (`xp` on physical memory): P4[0] → P3, P3[0] → P2,
  P2[0..7] = `0x83, 0x200083, … 0xE00083`.
- The GDT pointer and the registers (CR0/CR3/CR4/EFER) are correct.
- Mapping only P2[0] (2 MB) boots fine. Adding P2[1] alone —
  mapping just 2–4 MB — already faults.
- The fault is a page fault (`#PF`, error 0) at the first
  instruction after `mov %rax, %cr0` turns paging on, whatever
  that instruction is, with CR2 = `0x80000011` (which is also the
  value just written to CR0). The faulting linear address
  `0x80000011` fails its page walk at P3[2], which is not present.
- The fault is not the `lgdt`, not the `loop` that fills P2, and
  not a bad GDT pointer — all three were ruled out by bisecting
  (ECX = 1 vs 2 vs 8) and by single-stepping with QEMU's GDB stub.

The root cause is still unidentified, so v0.2 ships with the
known-good 2 MB map and the allocator owns the free memory below
2 MB instead. v0.3's job is to find the cause (candidates: the
long-mode transition's interaction with the extra mappings, or a
QEMU/SeaBIOS quirk) and grow the map — the allocator code already
takes its region from `kernel_end` to `PMM_END`, so only the map
and one constant need to change.

---

## Roadmap — what's next

**v0.2 — talk back. DONE.** Keyboard input (IDT + IRQ1 + scancode
driver), a shell (`help`, `about`, `ver`, `mem`, `clear`), and the
start of memory management (a page-frame allocator). All 13
automated test checks pass.

**v0.3 — more memory, more useful.**
- Find and fix the 16 MB map fault (see "Known issue" above), then
  grow the allocator to own 2–16 MB.
- A heap (`malloc`/`free` for the kernel) built on the allocator.
- A clock/timer (the PIT), so the kernel knows time passes.

**Later:** reading files from disk, loading and running separate
programs, and eventually a graphical screen instead of text. Every
real OS is just these steps, repeated for years.

---

## Learn more

- The OSDev Wiki — https://wiki.osdev.org — the bible for this stuff.
  Start with "Bare Bones" and "Setting Up Long Mode".
- The Multiboot2 spec — https://www.gnu.org/software/grub/manual/multiboot2/
- Intel's manuals (volume 3, system programming) for how paging and
  long mode actually work, when you're ready to go deep.

Built by Wesley Robinson (Dolo45) with Muse, November 2026.
Create Nothing But Bangas.
