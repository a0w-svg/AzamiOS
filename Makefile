# ==============================================================================
# AzamiOS — Build System (Rewrite v7.0)
# Target: x86_64, Limine boot protocol
# Toolchain: x86_64-elf cross-compiler (GCC) + NASM
#
# Key make targets:
#   make            — build kernel.elf
#   make run        — build + launch in QEMU (UART → serial, 4 CPUs)
#   make run-debug  — same + GDB server on :1234
#   make gdb        — attach GDB to a waiting run-debug instance
#   make clean      — remove all build artefacts
#   make iso        — build bootable ISO image (requires xorriso + limine)
# ==============================================================================

# ── Toolchain ─────────────────────────────────────────────────────────────────
CROSS_PREFIX ?= $(HOME)/opt/cross-x86_64/bin/x86_64-elf-
CC    := $(CROSS_PREFIX)gcc
LD    := $(CROSS_PREFIX)ld
AR    := $(CROSS_PREFIX)ar
GDB   := $(CROSS_PREFIX)gdb
NASM  := nasm

# ── Build directories ─────────────────────────────────────────────────────────
BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
KERNEL_ELF := $(BUILD_DIR)/kernel.elf

# ── Compiler flags ────────────────────────────────────────────────────────────
CFLAGS := \
    -std=c11 \
    -ffreestanding \
    -fno-stack-protector \
    -fno-pie \
    -fno-pic \
    -mno-red-zone \
    -mno-mmx \
    -mno-sse \
    -mno-sse2 \
    -mcmodel=kernel \
    -m64 \
    -Wall \
    -Wextra \
    -Wshadow \
    -Wno-unused-parameter \
    -O2 \
    -g \
    -fno-omit-frame-pointer \
    -I. \
    -Iinclude \
    -Iarch/x86_64 \
    -Ikernel

LDFLAGS := \
    -T scripts/kernel.ld \
    -nostdlib \
    --no-warn-rwx-segments \
    -z max-page-size=0x1000

NASM_FLAGS := -f elf64 -g -F dwarf

# ── Source files ──────────────────────────────────────────────────────────────

# Boot layer
BOOT_ASM_SRCS := arch/x86_64/boot/entry.asm

# Architecture C sources
ARCH_C_SRCS := \
    arch/x86_64/boot/limine_req.c \
    arch/x86_64/cpu/gdt.c \
    arch/x86_64/cpu/idt.c \
    arch/x86_64/cpu/pic.c \
    arch/x86_64/cpu/lapic.c \
    arch/x86_64/cpu/smp.c \
    arch/x86_64/mm/vmm.c

# Architecture ASM sources
ARCH_ASM_SRCS := \
    arch/x86_64/cpu/gdt_flush.asm \
    arch/x86_64/cpu/isr.asm \
    arch/x86_64/cpu/switch_to.asm \
    arch/x86_64/syscall/syscall_entry.asm \
    arch/x86_64/lib/uaccess.asm

# Kernel C sources
KERNEL_C_SRCS := \
    kernel/main.c \
    kernel/panic.c \
    kernel/lib/string.c \
    kernel/mm/pmm.c \
    kernel/mm/kmalloc.c \
    kernel/syscall/syscall.c \
    kernel/sched/sched.c \
    kernel/sched/elf.c \
    kernel/ipc/ipc.c \
    kernel/security/security.c \
    kernel/object/object.c \
    fs/vfs.c \
    fs/devfs.c \
    fs/ext2/ext2.c \
    hal/hal.c \
    hal/device.c \
    hal/driver.c \
    hal/irp.c \
    hal/irq.c \
    hal/pci.c \
    drivers/block/block.c \
    drivers/block/ata.c \
    drivers/input/input.c \
    drivers/char/uart.c \
    drivers/char/console.c \
    drivers/char/lpt.c \
    drivers/misc/bga.c \
    drivers/misc/rtc.c \
    drivers/acpi/acpi.c \
    drivers/acpi/ioapic.c \
    drivers/acpi/power.c \
    drivers/sound/sound.c \
    drivers/sound/ac97.c \
    drivers/sound/pcspeaker.c \
    drivers/char/memdevs.c \
    drivers/net/e1000.c \
    drivers/net/rtl8139.c \
    drivers/net/virtio_net.c \
    drivers/block/virtio_blk.c \
    drivers/video/virtio_gpu.c \
    drivers/video/virtio_gpu_cmd.c \
    drivers/video/fbdev.c \
    hal/virtio_pci.c \
    hal/virtqueue.c \
    kernel/net/net.c \
    fs/pipe.c \
    fs/fat32.c \
    fs/procfs.c

# ── Object file lists ─────────────────────────────────────────────────────────
BOOT_OBJS   := $(patsubst %.asm, $(OBJ_DIR)/%.o, $(BOOT_ASM_SRCS))
ARCH_C_OBJS := $(patsubst %.c,   $(OBJ_DIR)/%.o, $(ARCH_C_SRCS))
ARCH_A_OBJS := $(patsubst %.asm, $(OBJ_DIR)/%.o, $(ARCH_ASM_SRCS))
KERN_OBJS   := $(patsubst %.c,   $(OBJ_DIR)/%.o, $(KERNEL_C_SRCS))

ALL_OBJS := \
    $(BOOT_OBJS) \
    $(ARCH_C_OBJS) \
    $(ARCH_A_OBJS) \
    $(KERN_OBJS)

# ── Userspace (unchanged from original; just kept so `make run` still
#    can build initrd if desired) ──────────────────────────────────────────────
UTIL_LIST := ls help cat write time clear ifconfig ping arp lsmod reload cpu \
             whoami fps acpi reboot shutdown about notepad files mount lpc \
             testarch grep find sed awk du df head tail wc sort uniq python
UTIL_TARGETS := $(foreach u,$(UTIL_LIST),userland/apps/$(u)/$(u))

# ── Primary targets ───────────────────────────────────────────────────────────
.PHONY: all run run-debug gdb iso clean userspace doc

all: doc $(KERNEL_ELF) hdd.img

$(KERNEL_ELF): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $^ -o $@
	@echo ""
	@echo "  ✓  Kernel linked: $@"
	@echo "     Size: $$(wc -c < $@ | tr -d ' ') bytes"

doc:
	@echo "  ↓  Generating documentation..."
	@python3 scripts/autodoc.py

# ── Compilation rules ─────────────────────────────────────────────────────────
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASM_FLAGS) $< -o $@

# ── QEMU run targets ──────────────────────────────────────────────────────────
QEMU := qemu-system-x86_64
DISPLAY_FLAG ?= 
QEMU_FLAGS := \
    -M q35 \
    -m 512M \
    -smp 4 \
    -cpu max,+rdrand,+rdseed \
    -serial stdio \
    $(DISPLAY_FLAG) \
    -no-reboot \
    -no-shutdown \
    -drive file=hdd.img,format=raw,if=none,id=drv0 \
    -device ide-hd,drive=drv0,bus=ide.0 \
    -audiodev pa,id=snd0 \
    -device AC97,audiodev=snd0

# Limine-based ISO run (headless serial terminal by default)
run: iso
	$(QEMU) $(QEMU_FLAGS) -cdrom $(BUILD_DIR)/AzamiOS.iso

# Graphical GUI window run (opens QEMU display + serial output)
run-gui: iso
	$(QEMU) $(QEMU_FLAGS) DISPLAY_FLAG="-vga std" -cdrom $(BUILD_DIR)/AzamiOS.iso || $(QEMU) -M q35 -m 512M -smp 4 -cpu max,+rdrand,+rdseed -serial stdio -vga std -cdrom $(BUILD_DIR)/AzamiOS.iso

# Direct ELF run (for quick iteration, no Limine — uses -kernel quirk)
run-direct: $(KERNEL_ELF)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

run-debug: iso
	$(QEMU) $(QEMU_FLAGS) -cdrom $(BUILD_DIR)/AzamiOS.iso -s -S
	@echo "Waiting for GDB on port 1234..."

gdb: $(KERNEL_ELF)
	$(GDB) \
	    -ex "set architecture i386:x86-64" \
	    -ex "target remote localhost:1234" \
	    -ex "symbol-file $(KERNEL_ELF)" \
	    -ex "break kernel_main" \
	    -ex "continue"

# ── ISO generation (auto-bootstrapping local Limine and mkisofs/genisoimage) ──
LIMINE_DIR := $(shell [ -d tools/limine ] && echo tools/limine || echo /usr/share/limine)
MKISOFS := $(shell command -v xorriso >/dev/null 2>&1 && echo "xorriso -as mkisofs" || (command -v mkisofs >/dev/null 2>&1 && echo "mkisofs" || echo "genisoimage"))

tools/limine:
	@echo "  ↓  Bootstrapping local Limine bootloader..."
	@mkdir -p tools
	@git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1 tools/limine
	@$(MAKE) -C tools/limine >/dev/null 2>&1 || true

iso: $(KERNEL_ELF) | tools/limine
	@mkdir -p $(BUILD_DIR)/iso_root/boot/limine
	@mkdir -p $(BUILD_DIR)/iso_root/EFI/BOOT
	@$(MAKE) -C userland ARCH=x86_64 >/dev/null
	@mke2fs -F -t ext2 -d userland/build $(BUILD_DIR)/initrd.ext2 16M >/dev/null 2>&1 || true
	cp $(KERNEL_ELF) $(BUILD_DIR)/iso_root/boot/kernel.elf
	cp $(BUILD_DIR)/initrd.ext2 $(BUILD_DIR)/iso_root/boot/initrd.ext2 2>/dev/null || true
	cp limine.conf   $(BUILD_DIR)/iso_root/boot/limine/limine.conf
	@if [ -f tools/limine/limine-bios-cd.bin ]; then \
	    cp tools/limine/limine-bios-cd.bin tools/limine/limine-bios.sys $(BUILD_DIR)/iso_root/boot/limine/; \
	elif [ -f $(LIMINE_DIR)/limine-bios-cd.bin ]; then \
	    cp $(LIMINE_DIR)/limine-bios-cd.bin $(LIMINE_DIR)/limine-bios.sys $(BUILD_DIR)/iso_root/boot/limine/ 2>/dev/null || true; \
	fi
	@if [ -f tools/limine/BOOTX64.EFI ]; then \
	    cp tools/limine/BOOTX64.EFI $(BUILD_DIR)/iso_root/EFI/BOOT/; \
	elif [ -f $(LIMINE_DIR)/BOOTX64.EFI ]; then \
	    cp $(LIMINE_DIR)/BOOTX64.EFI $(BUILD_DIR)/iso_root/EFI/BOOT/ 2>/dev/null || true; \
	fi
	$(MKISOFS) \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot \
	    -boot-load-size 4 \
	    -boot-info-table \
	    -R -J \
	    -o $(BUILD_DIR)/AzamiOS.iso \
	    $(BUILD_DIR)/iso_root 2>/dev/null || \
	    echo "  ⚠  ISO build failed — verify mkisofs or genisoimage is installed"
	@if [ -x tools/limine/limine ]; then \
	    tools/limine/limine bios-install $(BUILD_DIR)/AzamiOS.iso 2>/dev/null || true; \
	elif command -v limine >/dev/null 2>&1; then \
	    limine bios-install $(BUILD_DIR)/AzamiOS.iso 2>/dev/null || true; \
	fi
	@echo "  ✓  ISO: $(BUILD_DIR)/AzamiOS.iso"

hdd.img:
	@echo "  ↓  Generating persistent storage disk (hdd.img)..."
	@mkdir -p hdd_root
	@printf "Welcome to AzamiOS Persistent Storage!\n\nThis file is saved directly to the SATA drive (AHCI).\nEdit this text and press Ctrl+S to save it persistently!\n" > hdd_root/notes.txt
	@truncate -s 4096 hdd_root/notes.txt
	@mke2fs -F -t ext2 -d hdd_root hdd.img 2M >/dev/null 2>&1 || true

# ── Userspace (pass-through to original targets) ─────────────────────────────
userland/libc/libc.a:
	$(MAKE) -C userland/libc ARCH=x86_64

userland/apps/shell/shell: userland/libc/libc.a
	$(MAKE) -C userland/apps/shell ARCH=x86_64

userspace: userland/apps/shell/shell

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) kernel.log
	@echo "  ✓  Build directory cleaned"