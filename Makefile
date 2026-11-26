# DoloOS v0.2 — Makefile
#
#   make        build the kernel (doloos.kernel)
#   make iso    build a bootable ISO (doloos.iso) with GRUB
#   make run    boot the ISO in QEMU, serial output to boot.log
#   make test   boot the ISO in QEMU and drive the shell with
#               scripted keystrokes (checks help/about/ver/mem/
#               clear/backspace/unknown-command), then report
#   make clean  remove everything built

CC      := gcc
LD      := ld

# -ffreestanding : no standard library, we are the OS now
# -m64           : 64-bit code (boot.S switches the CPU into long mode)
# -mno-red-zone  : the 128-byte red zone is not safe in a kernel
# -mno-sse etc   : the kernel never sets up SSE, so never emit it
# -fno-stack-protector : Ubuntu gcc defaults it on; kernels can't use it
CFLAGS  := -std=gnu99 -ffreestanding -O2 -Wall -Wextra \
           -m64 -mno-red-zone -mno-sse -mno-sse2 -mno-mmx \
           -fno-stack-protector
LDFLAGS := -n -T linker.ld

KERNEL  := doloos.kernel
ISO     := doloos.iso
OBJS    := boot.o kernel.o

all: $(KERNEL)

boot.o: boot.S
	$(CC) $(CFLAGS) -c boot.S -o boot.o

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

iso: $(KERNEL)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/doloos.kernel
	cp grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) iso

# Boot in QEMU with no window; serial output lands in boot.log.
# The kernel halts forever after printing, so timeout stops QEMU.
run: iso
	timeout 10 qemu-system-x86_64 -cdrom $(ISO) -serial file:boot.log -display none || true
	@echo "--- boot.log ---"
	@cat boot.log

# Full shell test: scripted keystrokes through the QEMU monitor.
test: iso
	python3 boot-test-v02.py

clean:
	rm -rf *.o $(KERNEL) $(ISO) iso boot.log screen.ppm screen.png

.PHONY: all iso run clean
