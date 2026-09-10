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
#   make linux      — build stock Linux binaries (musl + BusyBox) for the initrd
#   make linux-test — boot a headless VM and run the Linux-ABI probe
# ==============================================================================

# ── Toolchain ─────────────────────────────────────────────────────────────────
CROSS_PREFIX ?= $(HOME)/opt/cross-x86_64/bin/x86_64-elf-
CC    := $(CROSS_PREFIX)gcc
LD    := $(CROSS_PREFIX)ld
AR    := $(CROSS_PREFIX)ar
GDB   := $(CROSS_PREFIX)gdb
NASM  := nasm

# ── Parallel build ───────────────────────────────────────────────────────────
# Compile across all host CPUs by default (recursive sub-makes inherit this).
# `make -j1` on the command line still wins (last -j takes effect), and
# `make PARALLEL=0` disables it entirely for clean serial logs.
# The recipe graph is parallel-safe: per-object compiles are independent, and
# the multi-step recipes (iso:, hdd.img:) run their own steps sequentially.
PARALLEL ?= 1
ifeq ($(PARALLEL),1)
NPROC := $(shell nproc 2>/dev/null || echo 4)
MAKEFLAGS += -j$(NPROC)
endif

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
    -O3 \
    -g \
    -fno-omit-frame-pointer \
    -fno-strict-aliasing \
    -fno-delete-null-pointer-checks \
    -ffunction-sections \
    -fdata-sections \
    -falign-functions=16 \
    -falign-loops=16 \
    -falign-jumps=16 \
    -fno-semantic-interposition \
    -pipe \
    -I. \
    -Iinclude \
    -Iarch/x86_64 \
    -Ikernel

LDFLAGS := \
    -T scripts/kernel.ld \
    -nostdlib \
    --no-warn-rwx-segments \
    -z max-page-size=0x1000 \
    --gc-sections

NASM_FLAGS := -f elf64 -g -F dwarf

# Header dependency tracking. Without it a change to a header rebuilds nothing,
# and the link happily mixes objects compiled against different versions of the
# same struct — a silently corrupt kernel rather than a build error. -MMD emits
# a .d file next to each object listing the headers it read; -MP adds phony
# targets so deleting a header does not wedge the build.
CFLAGS += -MMD -MP

# ── Source files ──────────────────────────────────────────────────────────────

# Boot layer
BOOT_ASM_SRCS := arch/x86_64/boot/entry.asm

# Architecture C sources
ARCH_C_SRCS := \
    arch/x86_64/boot/limine_req.c \
    arch/x86_64/cpu/cpu.c \
    arch/x86_64/cpu/hwaccel.c \
    arch/x86_64/cpu/pmu.c \
    arch/x86_64/cpu/mitigations.c \
    arch/x86_64/cpu/mce.c \
    arch/x86_64/cpu/gdt.c \
    arch/x86_64/cpu/idt.c \
    arch/x86_64/cpu/pic.c \
    arch/x86_64/cpu/lapic.c \
    arch/x86_64/cpu/smp.c \
    arch/x86_64/mm/vmm.c \
    arch/x86_64/mm/tlb.c

# Architecture ASM sources
ARCH_ASM_SRCS := \
    arch/x86_64/cpu/gdt_flush.asm \
    arch/x86_64/cpu/isr.asm \
    arch/x86_64/cpu/hwprobe.asm \
    arch/x86_64/cpu/switch_to.asm \
    arch/x86_64/syscall/syscall_entry.asm \
    arch/x86_64/lib/uaccess.asm

# Kernel C sources
KERNEL_C_SRCS := \
    kernel/main.c \
    kernel/panic.c \
    kernel/signal.c \
    kernel/ptrace.c \
    kernel/perf/perf.c \
    kernel/perf/ktrace.c \
    kernel/lib/string.c \
    kernel/lib/random.c \
    kernel/mm/pmm.c \
    kernel/mm/kmalloc.c \
    kernel/mm/vma.c \
    kernel/syscall/syscall.c \
    kernel/sched/sched.c \
    kernel/sched/elf.c \
    kernel/ipc/ipc.c \
    kernel/ipc/sysvipc.c \
    kernel/ipc/mqueue.c \
    kernel/ipc/posix_sem.c \
    kernel/ktimer.c \
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
    drivers/block/ahci.c \
    drivers/block/nvme.c \
    drivers/input/input.c \
    drivers/char/uart.c \
    drivers/char/console.c \
    drivers/char/lpt.c \
    drivers/misc/bga.c \
    drivers/misc/rtc.c \
    drivers/misc/hpet.c \
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
    drivers/base/core.c \
    drivers/base/bus.c \
    drivers/base/platform.c \
    drivers/base/pci_bus.c \
    drivers/base/uevent.c \
    drivers/gpu/drm/drm_mode.c \
    drivers/gpu/drm/drm_gem.c \
    drivers/gpu/drm/drm_ioctl.c \
    drivers/gpu/drm/drm_drv.c \
    drivers/gpu/drm/drm_vblank.c \
    drivers/gpu/drm/bochs_drv.c \
    drivers/gpu/drm/vmwgfx_drv.c \
    drivers/gpu/drm/simpledrm.c \
    drivers/gpu/drm/virtgpu_drm.c \
    drivers/input/virtio_input.c \
    drivers/char/virtio_console.c \
    drivers/net/ne2k_pci.c \
    drivers/watchdog/i6300esb.c \
    drivers/input/evdev.c \
    drivers/i2c/i2c-core.c \
    drivers/i2c/i2c-i801.c \
    drivers/misc/virtio_balloon.c \
    hal/virtio_pci.c \
    hal/virtqueue.c \
    kernel/net/net.c \
    kernel/net/net_buf.c \
    kernel/net/arp.c \
    kernel/net/ipv4.c \
    kernel/net/icmp.c \
    kernel/net/udp.c \
    kernel/net/tcp.c \
    kernel/net/socket.c \
    kernel/net/unix_socket.c \
    kernel/net/dhcp.c \
    fs/pipe.c \
    fs/fat32.c \
    fs/procfs.c \
    fs/tmpfs.c \
    kernel/security/acl.c \
    drivers/misc/virtio_rng.c \
    drivers/net/pcnet.c \
    drivers/block/fdc.c \
    drivers/sound/hda.c \
    drivers/sound/sb16.c \
    drivers/block/loop.c \
    fs/sysfs.c \
    fs/devpts.c \
    fs/squashfs/squashfs.c \
    drivers/char/pty.c

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

# Generated by -MMD; each one names the headers its object depends on. They are
# pulled in at the very bottom of this file: an `include` contributes rules, and
# the first rule make sees sets the default goal — including them here would
# make a stray .o the default target instead of `all`.
DEPFILES := $(ALL_OBJS:.o=.d)

# ── Userspace (unchanged from original; just kept so `make run` still
#    can build initrd if desired) ──────────────────────────────────────────────
UTIL_LIST := ls help cat write time clear ifconfig ping arp lsmod reload cpu \
             whoami fps acpi reboot shutdown about notepad files mount lpc \
             testarch grep find sed awk du df head tail wc sort uniq python
UTIL_TARGETS := $(foreach u,$(UTIL_LIST),userland/apps/$(u)/$(u))

# ── Primary targets ───────────────────────────────────────────────────────────
.PHONY: all run run-debug gdb iso clean userspace doc \
        linux linux-install linux-test linux-clean

# `doc` regenerates docs/*.md from source comments; it is not a build input, so
# it is no longer a prerequisite of `all` (run `make doc` to refresh it).
all: $(KERNEL_ELF) hdd.img

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

# Acceleration. KVM is ~10-20x faster than emulation but needs /dev/kvm access
# (`sudo usermod -aG kvm $$USER`, then re-login). If it is readable we use it
# with the host CPU model; otherwise we fall back to multi-threaded TCG, which
# still spreads the guest's 4 vCPUs across host cores.
ifeq ($(shell test -r /dev/kvm && test -w /dev/kvm && echo y),y)
  QEMU_ACCEL ?= -enable-kvm -cpu host
else
  QEMU_ACCEL ?= -accel tcg,thread=multi,tb-size=512 -cpu max,+rdrand,+rdseed
endif

QEMU_FLAGS := \
    -M q35 \
    -m 1536M \
    -smp 4 \
    $(QEMU_ACCEL) \
    -serial stdio \
    -vga std \
    $(DISPLAY_FLAG) \
    -no-reboot \
    -no-shutdown \
    -drive file=hdd.img,format=raw,if=none,id=drv0,cache=writeback \
    -device ide-hd,drive=drv0,bus=ide.0 \
    -netdev user,id=net0,net=10.0.2.0/24,dhcpstart=10.0.2.15 \
    -device e1000,netdev=net0 \
    -audiodev pa,id=snd0 \
    -device AC97,audiodev=snd0 \
    -device intel-hda -device hda-duplex,audiodev=snd0 \
    -device virtio-rng-pci

# Limine-based ISO run (GUI window + serial terminal)
run: iso
	$(QEMU) $(QEMU_FLAGS) -cdrom $(BUILD_DIR)/AzamiOS.iso

# Graphical GUI window run
run-gui: run

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
	@scripts/check_uapi_sync.sh
	@# Strip userland binaries for a smaller/faster-loading initrd.
	@# Off by default (adds a few seconds); enable with `make iso STRIP_INITRD=1`.
	@if [ "$(STRIP_INITRD)" = "1" ]; then \
	   before=$$(du -sm userland/build | cut -f1); \
	   find userland/build -type f ! -name '*.a' ! -name '*.o' -exec sh -c \
	     'for f; do $(CROSS_PREFIX)strip --strip-unneeded "$$f" 2>/dev/null || true; done' _ {} + ; \
	   after=$$(du -sm userland/build | cut -f1); \
	   echo "  ↓  Stripped userland binaries: $${before}M -> $${after}M"; \
	 fi
	@$(MAKE) --no-print-directory linux-install
	@rm -f $(BUILD_DIR)/initrd.ext2
	@sz=$$(du -sm userland/build | cut -f1); sz=$$((sz + sz/8 + 16)); \
	 echo "  ↓  Building initrd.ext2 ($${sz}M for $$(du -sh userland/build | cut -f1) of content)"; \
	 mke2fs -q -F -t ext2 -b 4096 -d userland/build $(BUILD_DIR)/initrd.ext2 $${sz}M




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

# ── Stock Linux userland ─────────────────────────────────────────────────────
# AzamiOS implements the Linux x86_64 syscall ABI, so software built by an
# ordinary Linux toolchain runs unmodified. `make linux` builds musl and a
# static BusyBox and leaves them in tools/linux/out; the next `make iso` picks
# them up automatically. See docs/LINUX-BINARIES.md.
linux:
	@$(MAKE) -C tools/linux

# Called from `iso`. A no-op until `make linux` has been run, so the ISO build
# never depends on a network fetch.
linux-install:
	@if [ -x tools/linux/out/bin/busybox ]; then \
	    $(MAKE) --no-print-directory -C tools/linux install \
	        DESTDIR=$(CURDIR)/userland/build; \
	 fi

# Boot headless with the Linux-ABI probe as PID 1 and print its verdict. This
# is the regression test for the ABI itself: it exercises the parts of the
# kernel only a stock-libc binary reaches (auxv layout, TLS setup,
# CLONE_CHILD_CLEARTID, link counts) and reports pass/fail counts on serial.
linux-test: $(KERNEL_ELF)
	@$(MAKE) --no-print-directory -C tools/linux probe
	@scripts/linux-test.sh

linux-clean:
	@$(MAKE) -C tools/linux clean

hdd.img:
	@echo "  ↓  Generating persistent storage disk (hdd.img)..."
	@mkdir -p hdd_root
	@printf "Welcome to AzamiOS Persistent Storage!\n\nThis file is saved directly to the SATA drive (AHCI).\nEdit this text and press Ctrl+S to save it persistently!\n" > hdd_root/notes.txt
	@truncate -s 4096 hdd_root/notes.txt
	@mke2fs -F -t ext2 -d hdd_root hdd.img 32M >/dev/null 2>&1 || true

# ── Userspace (pass-through to original targets) ─────────────────────────────
userland/libc/libc.a:
	$(MAKE) -C userland/libc ARCH=x86_64

userspace:
	$(MAKE) -C userland ARCH=x86_64

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) kernel.log
	@echo "  ✓  Build directory cleaned"

# ── Header dependencies (must stay last; see DEPFILES above) ─────────────────
-include $(DEPFILES)
