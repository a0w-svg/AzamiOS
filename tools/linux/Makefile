# ==============================================================================
# AzamiOS — Stock Linux Binary Port Pipeline
# File: tools/linux/Makefile
#
# Builds real, unmodified Linux software with a real Linux toolchain and drops
# the resulting binaries into the AzamiOS initrd. Nothing here is patched for
# AzamiOS: the binaries produced are byte-for-byte what the same commands would
# produce for a Linux box, and they run because the kernel implements the Linux
# x86_64 syscall ABI (see docs/LINUX-BINARIES.md).
#
# Targets:
#   make -C tools/linux            — toolchain + busybox + probe (everything)
#   make -C tools/linux toolchain  — build musl and its musl-gcc wrapper only
#   make -C tools/linux busybox    — build BusyBox, static, against musl
#   make -C tools/linux probe      — build the Linux-ABI conformance probe
#   make -C tools/linux install    — stage the results into the initrd tree
#   make -C tools/linux clean      — drop build output, keep the tarballs
#   make -C tools/linux distclean  — drop everything, including the tarballs
#
# Static linking is deliberate. A dynamic binary would need ld-linux plus the
# shared libc on the image and would exercise the loader's PT_INTERP path; the
# static case is the one that isolates "does the kernel's ABI hold up".
# ==============================================================================

MUSL_VER    := 1.2.5
BUSYBOX_VER := 1.36.1

MUSL_URL    := https://musl.libc.org/releases/musl-$(MUSL_VER).tar.gz
# busybox.net serves a certificate that does not match its own hostname, so the
# canonical download is unusable without disabling verification. Buildroot's
# mirror carries the identical tarball over a valid certificate.
BUSYBOX_URL := https://sources.buildroot.net/busybox/busybox-$(BUSYBOX_VER).tar.bz2

TOP        := $(CURDIR)
SRC_DIR    := $(TOP)/src
DL_DIR     := $(TOP)/dl
MUSL_DIR   := $(TOP)/musl
OUT_DIR    := $(TOP)/out
LOG_DIR    := $(TOP)/log

MUSL_SRC    := $(SRC_DIR)/musl-$(MUSL_VER)
BUSYBOX_SRC := $(SRC_DIR)/busybox-$(BUSYBOX_VER)

MUSL_GCC   := $(MUSL_DIR)/bin/musl-gcc
BUSYBOX    := $(OUT_DIR)/bin/busybox
PROBE      := $(OUT_DIR)/bin/azami-abi-probe

# Where `install` stages the binaries. The initrd is built from this tree by
# the top-level `iso` target.
DESTDIR ?= $(TOP)/../../userland/build

# BusyBox includes a handful of applets that reach for Linux UAPI headers
# (linux/kd.h and friends). musl deliberately ships none, so point the compiler
# at the host's copy *after* musl's own include path — musl's headers must
# still win every conflict. Where the host has no UAPI headers at all, the two
# applets that need them are switched off instead.
UAPI_INC := $(shell test -f /usr/include/linux/kd.h && echo -idirafter /usr/include)
UAPI_ASM := $(shell test -f /usr/include/x86_64-linux-gnu/asm/types.h && \
                    echo -idirafter /usr/include/x86_64-linux-gnu || \
                    (test -f /usr/include/asm/types.h && echo -idirafter /usr/include))
HAVE_UAPI := $(if $(and $(UAPI_INC),$(UAPI_ASM)),y,n)

BB_CC := $(MUSL_GCC) $(UAPI_INC) $(UAPI_ASM)

NPROC := $(shell nproc 2>/dev/null || echo 4)

.PHONY: all toolchain busybox probe install clean distclean fetch

all: toolchain busybox probe
	@echo ""
	@echo "  ✓  Linux binaries ready in $(OUT_DIR)"
	@echo "     Stage them into the initrd with: make -C tools/linux install"

# ── Download ─────────────────────────────────────────────────────────────────
fetch: $(DL_DIR)/musl-$(MUSL_VER).tar.gz $(DL_DIR)/busybox-$(BUSYBOX_VER).tar.bz2

$(DL_DIR)/musl-$(MUSL_VER).tar.gz:
	@mkdir -p $(DL_DIR)
	@echo "  ↓  Fetching musl $(MUSL_VER)"
	@curl -fsSL -o $@.part $(MUSL_URL) && mv $@.part $@

$(DL_DIR)/busybox-$(BUSYBOX_VER).tar.bz2:
	@mkdir -p $(DL_DIR)
	@echo "  ↓  Fetching BusyBox $(BUSYBOX_VER)"
	@curl -fsSL -o $@.part $(BUSYBOX_URL) && mv $@.part $@

$(MUSL_SRC)/configure: $(DL_DIR)/musl-$(MUSL_VER).tar.gz
	@mkdir -p $(SRC_DIR)
	@tar -xzf $< -C $(SRC_DIR)
	@touch $@

$(BUSYBOX_SRC)/Makefile: $(DL_DIR)/busybox-$(BUSYBOX_VER).tar.bz2
	@mkdir -p $(SRC_DIR)
	@tar -xjf $< -C $(SRC_DIR)
	@touch $@

# ── musl ─────────────────────────────────────────────────────────────────────
# musl-gcc is a wrapper around the host gcc that swaps in musl's headers and
# startup files. It bakes its own prefix in at configure time, so moving this
# directory afterwards breaks it — rebuild rather than relocate.
toolchain: $(MUSL_GCC)

$(MUSL_GCC): $(MUSL_SRC)/configure
	@mkdir -p $(LOG_DIR)
	@echo "  ⚙  Building musl $(MUSL_VER) (log: $(LOG_DIR)/musl.log)"
	@cd $(MUSL_SRC) && ./configure --prefix=$(MUSL_DIR) \
	    --disable-shared --enable-wrapper=gcc > $(LOG_DIR)/musl.log 2>&1
	@$(MAKE) -C $(MUSL_SRC) -j$(NPROC) >> $(LOG_DIR)/musl.log 2>&1
	@$(MAKE) -C $(MUSL_SRC) install >> $(LOG_DIR)/musl.log 2>&1
	@echo "  ✓  musl-gcc: $(MUSL_GCC)"

# ── BusyBox ──────────────────────────────────────────────────────────────────
busybox: $(BUSYBOX)

$(BUSYBOX): $(MUSL_GCC) $(BUSYBOX_SRC)/Makefile
	@mkdir -p $(LOG_DIR) $(OUT_DIR)/bin
	@echo "  ⚙  Configuring BusyBox $(BUSYBOX_VER) (UAPI headers: $(HAVE_UAPI))"
	@$(MAKE) -C $(BUSYBOX_SRC) defconfig > $(LOG_DIR)/busybox.log 2>&1
	@sed -i -e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
	        -e 's/^CONFIG_PIE=y/# CONFIG_PIE is not set/' \
	        $(BUSYBOX_SRC)/.config
	@# tc(8) needs iproute2's private headers, which no libc ships.
	@sed -i -e 's/^CONFIG_TC=y/# CONFIG_TC is not set/' \
	        -e 's/^CONFIG_FEATURE_TC_INGRESS=y/# CONFIG_FEATURE_TC_INGRESS is not set/' \
	        $(BUSYBOX_SRC)/.config
ifeq ($(HAVE_UAPI),n)
	@sed -i -e 's/^CONFIG_KBD_MODE=y/# CONFIG_KBD_MODE is not set/' \
	        -e 's/^CONFIG_LOADFONT=y/# CONFIG_LOADFONT is not set/' \
	        -e 's/^CONFIG_SETFONT=y/# CONFIG_SETFONT is not set/' \
	        $(BUSYBOX_SRC)/.config
endif
	@echo "  ⚙  Building BusyBox (log: $(LOG_DIR)/busybox.log)"
	@$(MAKE) -C $(BUSYBOX_SRC) CC="$(BB_CC)" HOSTCC=gcc -j$(NPROC) \
	    >> $(LOG_DIR)/busybox.log 2>&1
	@cp $(BUSYBOX_SRC)/busybox $@
	@echo "  ✓  BusyBox: $@ ($$(wc -c < $@) bytes, $$($@ --list | wc -l) applets)"

# ── ABI probe ────────────────────────────────────────────────────────────────
probe: $(PROBE)

$(PROBE): tests/abi-probe.c $(MUSL_GCC)
	@mkdir -p $(OUT_DIR)/bin
	@$(MUSL_GCC) -static -O2 -o $@ $< -lpthread
	@echo "  ✓  ABI probe: $@"

# ── Install into the initrd staging tree ─────────────────────────────────────
# The applet symlinks go under /usr/bin and /usr/sbin, never /bin or /sbin.
# AzamiOS's own utilities live in /bin and /sbin under .elf names and the
# shell's PATH puts those first, so the two userlands coexist: native tools keep
# priority, BusyBox fills in everything AzamiOS does not ship, and `busybox sh`
# gives a complete Linux shell either way.
install: $(BUSYBOX)
	@mkdir -p $(DESTDIR)/bin $(DESTDIR)/usr/bin $(DESTDIR)/usr/sbin
	@cp $(BUSYBOX) $(DESTDIR)/bin/busybox
	@if [ -x $(PROBE) ]; then cp $(PROBE) $(DESTDIR)/usr/bin/azami-abi-probe; fi
	@$(BUSYBOX) --list-full | while read -r applet; do \
	    name=$${applet##*/}; \
	    case "$$applet" in \
	        sbin/*|usr/sbin/*) dir=usr/sbin ;; \
	        *)                 dir=usr/bin  ;; \
	    esac; \
	    ln -sf ../../bin/busybox $(DESTDIR)/$$dir/$$name; \
	done
	@echo "  ✓  Installed BusyBox + $$($(BUSYBOX) --list | wc -l) applets into $(DESTDIR)"

# ── Housekeeping ─────────────────────────────────────────────────────────────
clean:
	rm -rf $(OUT_DIR) $(MUSL_DIR) $(SRC_DIR) $(LOG_DIR)

distclean: clean
	rm -rf $(DL_DIR)
