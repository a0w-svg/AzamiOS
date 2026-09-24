CROSS_PREFIX ?= $(HOME)/opt/cross-x86_64/bin/x86_64-elf-
CC = $(CROSS_PREFIX)gcc
LD = $(CROSS_PREFIX)ld
AR = $(CROSS_PREFIX)ar

# ccache wraps the compiler, not the flags, so it's a pure win on any rebuild
# (a `make clean` re-run, switching branches). Auto-detected; `make CCACHE=0`
# opts out. See the top-level Makefile for the same wiring on the kernel side.
CCACHE ?= 1
ifeq ($(CCACHE),1)
  CCACHE_BIN := $(shell command -v ccache 2>/dev/null)
  ifneq ($(CCACHE_BIN),)
    CC := ccache $(CC)
  endif
endif

# `make DEBUG=1` swaps -O3 for -O0 across every app for fast iteration /
# accurate gdb single-stepping; default is the release tuning this build
# already used.
DEBUG ?= 0

# ── Boot self-test suites ────────────────────────────────────────────────────
# init.elf runs several verification suites at boot, each gated on a marker
# file in /etc — see the access() checks at the top of apps/init/main.c, whose
# messages already describe them as off-by-default ("touch /etc/run-... to
# re-enable"). The image build used to install three of those markers
# unconditionally, which made the opt-in an opt-out nobody had opted into: a
# measured boot spent 10 s in the native GCC smoke test and another 13 s in
# the userspace and dynamic-linking suites, against about 7 s of actual kernel
# and desktop startup. Roughly four fifths of every boot was self-test.
#
# They are worth having — they are how the toolchain and the dynamic linker
# get exercised on real hardware — so this switches them on with
# `make SELFTESTS=1` rather than deleting them. The POSIX suite
# (/etc/run-posix-selftest) and the hardware benchmark (/etc/run-hwtest) were
# already opt-in and stay that way; SELFTESTS=1 does not turn them on.
SELFTESTS ?= 0
ifeq ($(DEBUG),1)
  OPT_FLAGS := -O0
else
  OPT_FLAGS := -O3
endif

# -fstack-protector-strong is paired with -mstack-protector-guard=global
# because libc/stdlib.c defines __stack_chk_guard as a plain global (seeded
# per-process from getentropy() in __libc_init — see there), not a value
# tucked inside the %fs-based TLS block GCC's default x86_64 guard expects;
# `global` makes GCC address that symbol directly instead.
CFLAGS = -std=c11 -ffreestanding -fstack-protector-strong -mstack-protector-guard=global \
         -fno-pie -fno-pic -m64 \
         -Wall -Wextra -Wformat=2 -Wformat-security -Wno-unused-parameter \
         $(OPT_FLAGS) -fno-semantic-interposition -pipe \
         -ffunction-sections -fdata-sections \
         -I. -Ilibc/include -Iapps/shared -Iapps/azwm -Ilibgame/include

# Header dependency tracking (libc/Makefile already does this for libc itself;
# this is the same fix for the app layer). Without it, editing a shared header
# like apps/shared/ui_kit.h or apps/azwm/protocol.h rebuilt nothing here until
# the next full `make clean` — every app silently kept linking whatever it last
# compiled against, headers included. -MMD emits a .d file next to each object
# naming the headers it read; -MP adds phony targets so deleting a header does
# not wedge the build. See "Header dependencies" near the bottom of this file
# for where these get pulled back in.
CFLAGS += -MMD -MP

LDFLAGS = -T ../scripts/user.ld -nostdlib -z max-page-size=0x1000 --gc-sections

BUILD_DIR = build
BIN_DIR   = $(BUILD_DIR)/bin
SBIN_DIR  = $(BUILD_DIR)/sbin
OBJ_DIR   = $(BUILD_DIR)/obj

SBIN_APPS = \
    $(SBIN_DIR)/init.elf \
    $(SBIN_DIR)/sessiond.elf \
    $(SBIN_DIR)/azwm.elf \
    $(SBIN_DIR)/wallpaper.elf \
    $(SBIN_DIR)/taskbar.elf \
    $(SBIN_DIR)/poweroff.elf \
    $(SBIN_DIR)/reboot.elf \
    $(SBIN_DIR)/lspci.elf \
    $(SBIN_DIR)/ifconfig.elf \
    $(SBIN_DIR)/ping.elf \
    $(SBIN_DIR)/dmesg.elf \
    $(SBIN_DIR)/netstat.elf \
    $(SBIN_DIR)/dhcpcd.elf \
    $(SBIN_DIR)/notifyd.elf \
    $(SBIN_DIR)/lockscreen.elf

BIN_APPS_SIMPLE = \
    $(BIN_DIR)/testarch.elf \
    $(BIN_DIR)/hwtest.elf \
    $(BIN_DIR)/strace.elf \
    $(BIN_DIR)/ktrace.elf \
    $(BIN_DIR)/sem_test.elf \
    $(BIN_DIR)/kexec_test.elf \
    $(BIN_DIR)/lock.elf \
    $(BIN_DIR)/ptraceperf.elf \
    $(BIN_DIR)/ls.elf \
    $(BIN_DIR)/cat.elf \
    $(BIN_DIR)/echo.elf \
    $(BIN_DIR)/play.elf \
    $(BIN_DIR)/gui_test.elf \
    $(BIN_DIR)/launcher.elf \
    $(BIN_DIR)/about.elf \
    $(BIN_DIR)/calculator.elf \
    $(BIN_DIR)/clock.elf \
    $(BIN_DIR)/sysmon.elf \
    $(BIN_DIR)/texteditor.elf \
    $(BIN_DIR)/filemanager.elf \
    $(BIN_DIR)/terminal.elf \
    $(BIN_DIR)/fetch.elf \
    $(BIN_DIR)/pwd.elf \
    $(BIN_DIR)/uname.elf \
    $(BIN_DIR)/head.elf \
    $(BIN_DIR)/wc.elf \
    $(BIN_DIR)/grep.elf \
    $(BIN_DIR)/mkdir.elf \
    $(BIN_DIR)/rm.elf \
    $(BIN_DIR)/touch.elf \
    $(BIN_DIR)/cp.elf \
    $(BIN_DIR)/mv.elf \
    $(BIN_DIR)/tail.elf \
    $(BIN_DIR)/date.elf \
    $(BIN_DIR)/uptime.elf \
    $(BIN_DIR)/df.elf \
    $(BIN_DIR)/free.elf \
    $(BIN_DIR)/env.elf \
    $(BIN_DIR)/which.elf \
    $(BIN_DIR)/sleep.elf \
    $(BIN_DIR)/kill.elf \
    $(BIN_DIR)/chmod.elf \
    $(BIN_DIR)/clear.elf \
    $(BIN_DIR)/hexdump.elf \
    $(BIN_DIR)/base64.elf \
    $(BIN_DIR)/md5sum.elf \
    $(BIN_DIR)/cut.elf \
    $(BIN_DIR)/sort.elf \
    $(BIN_DIR)/uniq.elf \
    $(BIN_DIR)/ps.elf \
    $(BIN_DIR)/top.elf \
    $(BIN_DIR)/tree.elf \
    $(BIN_DIR)/cal.elf \
    $(BIN_DIR)/watch.elf \
    $(BIN_DIR)/screenshot.elf \
    $(BIN_DIR)/paint.elf \
    $(BIN_DIR)/find.elf \
    $(BIN_DIR)/whoami.elf \
    $(BIN_DIR)/du.elf \
    $(BIN_DIR)/time.elf \
    $(BIN_DIR)/tar.elf \
    $(BIN_DIR)/strings.elf \
    $(BIN_DIR)/xargs.elf \
    $(BIN_DIR)/seq.elf \
    $(BIN_DIR)/stat.elf \
    $(BIN_DIR)/basename.elf \
    $(BIN_DIR)/dirname.elf \
    $(BIN_DIR)/yes.elf \
    $(BIN_DIR)/true.elf \
    $(BIN_DIR)/false.elf \
    $(BIN_DIR)/audioplayer.elf \
    $(BIN_DIR)/minesweeper.elf \
    $(BIN_DIR)/2048.elf \
    $(BIN_DIR)/snake.elf \
    $(BIN_DIR)/pasjans.elf \
    $(BIN_DIR)/getfacl.elf \
    $(BIN_DIR)/setfacl.elf \
    $(BIN_DIR)/xorg.elf \
    $(BIN_DIR)/xwm.elf \
    $(BIN_DIR)/xclock.elf \
    $(BIN_DIR)/xcalc.elf \
    $(BIN_DIR)/xeyes.elf \
    $(BIN_DIR)/xgui_demo.elf \
    $(BIN_DIR)/nc.elf \
    $(BIN_DIR)/curl.elf \
    $(BIN_DIR)/pkg.elf \
    $(BIN_DIR)/cowsay.elf \
    $(BIN_DIR)/httpd.elf \
    $(BIN_DIR)/config.elf \
    $(BIN_DIR)/test.elf \
    $(BIN_DIR)/id.elf \
    $(BIN_DIR)/tr.elf \
    $(BIN_DIR)/sed.elf \
    $(BIN_DIR)/ln.elf \
    $(BIN_DIR)/tee.elf \
    $(BIN_DIR)/paste.elf \
    $(BIN_DIR)/comm.elf \
    $(BIN_DIR)/cmp.elf \
    $(BIN_DIR)/fold.elf \
    $(BIN_DIR)/expand.elf \
    $(BIN_DIR)/unexpand.elf \
    $(BIN_DIR)/expr.elf \
    $(BIN_DIR)/printenv.elf \
    $(BIN_DIR)/split.elf \
    $(BIN_DIR)/tty.elf \
    $(BIN_DIR)/logname.elf \
    $(BIN_DIR)/nl.elf \
    $(BIN_DIR)/cksum.elf \
    $(BIN_DIR)/pathchk.elf \
    $(BIN_DIR)/nohup.elf \
    $(BIN_DIR)/groups.elf \
    $(BIN_DIR)/users.elf \
    $(BIN_DIR)/nslookup.elf \
    $(BIN_DIR)/who.elf \
    $(BIN_DIR)/hostname.elf \
    $(BIN_DIR)/sync.elf \
    $(BIN_DIR)/readlink.elf \
    $(BIN_DIR)/realpath.elf \
    $(BIN_DIR)/tsort.elf \
    $(BIN_DIR)/pr.elf \
    $(BIN_DIR)/od.elf \
    $(BIN_DIR)/timeout.elf \
    $(BIN_DIR)/truncate.elf \
    $(BIN_DIR)/mktemp.elf \
    $(BIN_DIR)/chroot.elf \
    $(BIN_DIR)/nice.elf \
    $(BIN_DIR)/renice.elf \
    $(BIN_DIR)/ionice.elf \
    $(BIN_DIR)/killall.elf \
    $(BIN_DIR)/pgrep.elf \
    $(BIN_DIR)/losetup.elf \
    $(BIN_DIR)/diff.elf \
    $(BIN_DIR)/patch.elf \
    $(BIN_DIR)/sha256sum.elf \
    $(BIN_DIR)/sha1sum.elf \
    $(BIN_DIR)/gzip.elf \
    $(BIN_DIR)/file.elf \
    $(BIN_DIR)/pstree.elf \
    $(BIN_DIR)/lsof.elf \
    $(BIN_DIR)/ipcs.elf \
    $(BIN_DIR)/ipcrm.elf \
    $(BIN_DIR)/taskset.elf \
    $(BIN_DIR)/iostat.elf \
    $(BIN_DIR)/lsblk.elf \
    $(BIN_DIR)/ip.elf \
    $(BIN_DIR)/sysctl.elf \
    $(BIN_DIR)/inotifywait.elf \
    $(BIN_DIR)/vmstat.elf \
    $(BIN_DIR)/getfattr.elf \
    $(BIN_DIR)/setfattr.elf \
    $(BIN_DIR)/autoaccept.elf \
    $(BIN_DIR)/setfont.elf \
    $(BIN_DIR)/imageviewer.elf \
    $(BIN_DIR)/notes.elf \
    $(BIN_DIR)/less.elf \
    $(BIN_DIR)/bc.elf \
    $(BIN_DIR)/hexedit.elf \
    $(BIN_DIR)/fontviewer.elf \
    $(BIN_DIR)/ide.elf


.PHONY: all clean libc libgame apps skeleton etc icons fonts music images templates toolchain

all: skeleton libc libgame apps toolchain dynlink etc icons fonts music images templates pkgrepo

skeleton:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR) $(SBIN_DIR) $(OBJ_DIR) $(BUILD_DIR)/etc $(BUILD_DIR)/boot $(BUILD_DIR)/dev $(BUILD_DIR)/proc $(BUILD_DIR)/sys $(BUILD_DIR)/root $(BUILD_DIR)/root/.config $(BUILD_DIR)/home/azami/Desktop $(BUILD_DIR)/home/azami/Music $(BUILD_DIR)/home/azami/Pictures $(BUILD_DIR)/home/azami/Templates $(BUILD_DIR)/home/azami/.notes $(BUILD_DIR)/home/azami/.config $(BUILD_DIR)/desktop $(BUILD_DIR)/usr/bin $(BUILD_DIR)/usr/sbin $(BUILD_DIR)/usr/lib $(BUILD_DIR)/usr/share/icons $(BUILD_DIR)/usr/share/wallpapers $(BUILD_DIR)/usr/share/fonts $(BUILD_DIR)/usr/share/themes $(BUILD_DIR)/usr/share/music $(BUILD_DIR)/usr/share/templates $(BUILD_DIR)/music $(BUILD_DIR)/lib $(BUILD_DIR)/lib64 $(BUILD_DIR)/var $(BUILD_DIR)/tmp
	@# ── Desktop theme files: /usr/share/themes/*.theme ──────────────────────
	@# Data, not code — moved out of ui_kit.h so a new palette is a file drop,
	@# not a recompile. Leading NN- keeps the historical theme_id order (see
	@# az_theme_t / az_theme_get() in libc/theme.c) legible in `ls`, though
	@# scan order is authoritative, not the filename.
	@printf "# AzamiOS Theme: Catppuccin Mocha\nname=Catppuccin Mocha\ncrust=0xFF11111B\nmantle=0xFF181825\nbase=0xFF1E1E2E\nsurface0=0xFF313244\nsurface1=0xFF45475A\nsurface2=0xFF585B70\noverlay0=0xFF6C7086\noverlay1=0xFF7F849C\ntext=0xFFCDD6F4\naccent=0xFFCBA6F7\naccent_sec=0xFFFAB387\nred=0xFFF38BA8\ngreen=0xFFA6E3A1\nyellow=0xFFF9E2AF\nblue=0xFF89B4FA\n" > $(BUILD_DIR)/usr/share/themes/00-mocha.theme
	@printf "# AzamiOS Theme: Catppuccin Latte\nname=Catppuccin Latte\ncrust=0xFFDCE0E8\nmantle=0xFFE6E9EF\nbase=0xFFEFF1F5\nsurface0=0xFFCCD0DA\nsurface1=0xFFBCC0CC\nsurface2=0xFFACB0BE\noverlay0=0xFF9CA0B0\noverlay1=0xFF8C8FA1\ntext=0xFF4C4F69\naccent=0xFF8839EF\naccent_sec=0xFFFE640B\nred=0xFFD20F39\ngreen=0xFF40A02B\nyellow=0xFFDF8E1D\nblue=0xFF1E66F5\n" > $(BUILD_DIR)/usr/share/themes/01-latte.theme
	@printf "# AzamiOS Theme: Nord Arctic\nname=Nord Arctic\ncrust=0xFF242933\nmantle=0xFF2E3440\nbase=0xFF3B4252\nsurface0=0xFF434C5E\nsurface1=0xFF4C566A\nsurface2=0xFF5A657D\noverlay0=0xFF7885A0\noverlay1=0xFF9AA7C0\ntext=0xFFECEFF4\naccent=0xFF88C0D0\naccent_sec=0xFF81A1C1\nred=0xFFBF616A\ngreen=0xFFA3BE8C\nyellow=0xFFEBCB8B\nblue=0xFF5E81AC\n" > $(BUILD_DIR)/usr/share/themes/02-nord.theme
	@printf "# AzamiOS Theme: Cyberpunk Neon\nname=Cyberpunk Neon\ncrust=0xFF05050A\nmantle=0xFF0D0D18\nbase=0xFF141424\nsurface0=0xFF202038\nsurface1=0xFF2E2E50\nsurface2=0xFF424270\noverlay0=0xFF6868A0\noverlay1=0xFF8F8FD0\ntext=0xFFF0F6FC\naccent=0xFF00FFCC\naccent_sec=0xFFFF007F\nred=0xFFFF2A6D\ngreen=0xFF05FFA1\nyellow=0xFFFFE600\nblue=0xFF00F0FF\n" > $(BUILD_DIR)/usr/share/themes/03-cyberpunk.theme
	@printf "# AzamiOS Theme: OLED Pure Dark\nname=OLED Pure Dark\ncrust=0xFF000000\nmantle=0xFF050505\nbase=0xFF0A0A0A\nsurface0=0xFF181818\nsurface1=0xFF242424\nsurface2=0xFF323232\noverlay0=0xFF555555\noverlay1=0xFF777777\ntext=0xFFFFFFFF\naccent=0xFF3B82F6\naccent_sec=0xFF10B981\nred=0xFFEF4444\ngreen=0xFF22C55E\nyellow=0xFFEAB308\nblue=0xFF60A5FA\n" > $(BUILD_DIR)/usr/share/themes/04-oled.theme

libc: skeleton libc/libc.a

# Canonical syscall-number/errno headers (include/azami/uapi/*.h) copied into
# libc's own include tree so #include "azami/uapi/..." resolves the same way
# whether libc is built here by the cross-compiler or self-hosted on a
# running AzamiOS (which only ever sees libc/include, never the repo root —
# see the `toolchain`/build.sh staging below). Never hand-edit the copies:
# they are regenerated from ../include/azami/uapi on every build and are not
# tracked in git (see .gitignore). scripts/check_uapi_sync.sh verifies a
# stale copy is never silently relied on.
UAPI_SRCS := $(wildcard ../include/azami/uapi/*.h)
UAPI_COPIES := $(patsubst ../include/azami/uapi/%,libc/include/azami/uapi/%,$(UAPI_SRCS))

libc/include/azami/uapi/%.h: ../include/azami/uapi/%.h
	@mkdir -p libc/include/azami/uapi
	cp $< $@

.PHONY: uapi-sync
uapi-sync: $(UAPI_COPIES)

# The archive's real inputs, so make knows when to recurse.
#
# This rule used to carry no prerequisites at all. Once libc.a existed make
# considered it up to date forever and never re-entered libc/, so editing a
# libc source changed nothing: every app kept linking the stale archive and the
# only way to pick up a fix was to delete libc.a by hand. Listing the sources
# here also means the apps, which depend on libc.a, relink exactly when the
# library actually changed and not on every build. libc/Makefile does its own
# -MMD header tracking behind this.
LIBC_SRCS := $(wildcard libc/*.c) $(wildcard libc/*.asm) $(wildcard libc/include/*.h) \
             $(wildcard libc/include/*/*.h) libc/Makefile

# One recipe, one target: naming both artefacts on a single non-pattern rule
# makes GNU make treat it as two independent rules with the same recipe, and
# under -j it then runs two sub-makes into libc/ at once — they raced on
# libc.a and left a truncated archive ("file format not recognized").
libc/libc.a: $(UAPI_COPIES) $(LIBC_SRCS)
	$(MAKE) -C libc

libc/crt0.o: libc/libc.a
	@:

# libc/Makefile's own default goal (`all`) already builds libc.so and
# crt0_dyn.o as a side effect of the same submake invocation that builds
# libc.a/crt0.o (see libc/Makefile: `all: libc.a crt0.o libc.so
# crt0_dyn.o`) — these two dummy rules just give them a place in *this*
# Makefile's dependency graph (mirroring libc/crt0.o above) so
# $(BUILD_DIR)/lib/libc.so and the dynamic app-build path below can depend
# on them without triggering a second, redundant `$(MAKE) -C libc`.
libc/libc.so: libc/libc.a
	@:
libc/crt0_dyn.o: libc/libc.a
	@:

LIBGAME_SRCS := $(wildcard libgame/*.c) $(wildcard libgame/include/game/*.h) libgame/Makefile

libgame: libgame/libgame.a

libgame/libgame.a: $(LIBGAME_SRCS) libc/libc.a
	$(MAKE) -C libgame

# ── On-device development environment ───────────────────────────────────────
#
# This target used to copy the entire host cross-toolchain into the image:
# x86_64-elf-gcc and every binutil, cc1, collect2, lto-wrapper, the gcc
# internal headers, the linker scripts, and a farm of symlinks recreating
# /home/<developer>/opt/cross-x86_64 inside the rootfs so that gcc could find
# its own guts at the absolute paths it was configured with. That was ~84 MB
# of a 512 MB root partition, it embedded one machine's home directory in the
# image, and none of it was built for AzamiOS — it was a Linux toolchain that
# happened to run under the Linux-ABI layer.
#
# The compiler on the image is now TinyCC, built from source by
# tools/linux/ports.mk and configured there for *this* filesystem's layout
# (/usr/include, /usr/lib, /usr/lib/tcc). It is ~0.5 MB including its runtime
# library and headers, it compiles and links in one pass with no external
# assembler or linker, and `tcc hello.c -o hello` on a booted AzamiOS
# produces a working native binary. See docs/TOOLCHAIN.md.
#
# What stays here is what a compiler needs to target this system and what the
# rest of the build depends on: the libc/libgame headers under /usr/include,
# the static libraries and startup object under /usr/lib and /lib, and the
# glibc loader copies that let dynamically linked Linux binaries run.
TCC_PORT_BIN  := ../tools/linux/out/bin/tcc
BASH_PORT_BIN := ../tools/linux/out/bin/bash
TCC_PORT_TREE := ../tools/linux/out/tcc/usr

toolchain: skeleton libc libgame
	@# Drop what older images staged here, so an incremental rebuild does not
	@# leave 84 MB of dead cross-toolchain behind in build/.
	@rm -rf $(BUILD_DIR)/home/a0wsvg $(BUILD_DIR)/lib $(BUILD_DIR)/lib64 \
	        $(BUILD_DIR)/usr/libexec $(BUILD_DIR)/usr/lib/gcc \
	        $(BUILD_DIR)/usr/lib/ldscripts $(BUILD_DIR)/usr/lib/bfd-plugins \
	        $(BUILD_DIR)/usr/x86_64-elf $(BUILD_DIR)/x86_64-elf 2>/dev/null || true
	@for tool in gcc g++ c++ cpp as ld ld.bfd ar ranlib nm objdump objcopy strip \
	             strings readelf size addr2line c++filt gcov gcov-dump gcov-tool \
	             elfedit lto-wrapper x86_64-elf-gcc x86_64-elf-as x86_64-elf-ld; do \
	    rm -f $(BUILD_DIR)/usr/bin/$$tool $(BUILD_DIR)/bin/$$tool; \
	done
	@mkdir -p $(BUILD_DIR)/lib64 $(BUILD_DIR)/lib/x86_64-linux-gnu $(BUILD_DIR)/lib \
	          $(BUILD_DIR)/usr/lib $(BUILD_DIR)/usr/bin $(BUILD_DIR)/bin \
	          $(BUILD_DIR)/usr/local/lib $(BUILD_DIR)/usr/local/bin \
	          $(BUILD_DIR)/usr/include $(BUILD_DIR)/usr/local/include

	@# ── Headers a compiler on the OS needs to target it ──────────────────
	@# Wiped and re-staged rather than copied over: the old target dumped
	@# gcc's entire internal include directory here too — 125 files, 3.5 MB,
	@# almost all of it x86 intrinsics headers full of __builtin_ia32_*
	@# calls that only gcc implements. Left in place they would also shadow
	@# the compiler's own <stdarg.h>/<stddef.h>, which is a hard error: this
	@# libc ships neither, and gcc's stdarg.h is written against
	@# __builtin_va_list, which tcc does not have.
	@rm -rf $(BUILD_DIR)/usr/include $(BUILD_DIR)/usr/local/include
	@mkdir -p $(BUILD_DIR)/usr/include $(BUILD_DIR)/usr/local/include
	@cp -r libc/include/* $(BUILD_DIR)/usr/include/ 2>/dev/null || true
	@cp -r libc/include/* $(BUILD_DIR)/usr/local/include/ 2>/dev/null || true
	@mkdir -p $(BUILD_DIR)/usr/include/game $(BUILD_DIR)/usr/local/include/game
	@cp -r libgame/include/game/* $(BUILD_DIR)/usr/include/game/ 2>/dev/null || true
	@cp -r libgame/include/game/* $(BUILD_DIR)/usr/local/include/game/ 2>/dev/null || true

	@# ── Startup object and static libraries, in every path a linker looks ─
	@# libm/libg/libnosys are the same archive under the names a build might
	@# ask for with -lm and friends; this libc implements all of it.
	@for d in $(BUILD_DIR)/usr/lib $(BUILD_DIR)/lib $(BUILD_DIR)/lib64 $(BUILD_DIR)/usr/local/lib; do \
	    mkdir -p "$$d" 2>/dev/null || true; \
	    cp libc/crt0.o    "$$d"/crt0.o    2>/dev/null || true; \
	    cp libc/libc.a    "$$d"/libc.a    2>/dev/null || true; \
	    cp libc/libc.a    "$$d"/libm.a    2>/dev/null || true; \
	    cp libc/libc.a    "$$d"/libg.a    2>/dev/null || true; \
	    cp libc/libc.a    "$$d"/libnosys.a 2>/dev/null || true; \
	    cp libgame/libgame.a "$$d"/libgame.a 2>/dev/null || true; \
	done

	@# ── crt1/crti/crtn, the names a stock compiler driver links by ───────
	@# tcc (like gcc) links crt1.o, crti.o and crtn.o into every executable
	@# unless told -nostdlib. AzamiOS's startup object is crt0.o and its
	@# libc has no .init/.fini fragments at all — crt0.asm calls
	@# __init_tls/__libc_init directly and never _init — so crt1.o is that
	@# same object under the expected name, and crti/crtn are empty objects
	@# that satisfy the link without contributing anything.
	@printf '' > $(OBJ_DIR)/empty-crt.s
	@for d in $(BUILD_DIR)/usr/lib $(BUILD_DIR)/lib; do \
	    cp libc/crt0.o "$$d"/crt1.o 2>/dev/null || true; \
	    $(CC) -c -x assembler $(OBJ_DIR)/empty-crt.s -o "$$d"/crti.o 2>/dev/null || true; \
	    $(CC) -c -x assembler $(OBJ_DIR)/empty-crt.s -o "$$d"/crtn.o 2>/dev/null || true; \
	done

	@# ── TinyCC, when tools/linux/ports.mk has built it ───────────────────
	@# Absent, the image simply has no compiler and `pkg install tcc` adds
	@# one at runtime — the package installs the identical files to these
	@# paths. Present, it is installed as cc/tcc and the OS can build C
	@# programs with nothing else mounted.
	@if [ -x $(TCC_PORT_BIN) ]; then \
	    cp $(TCC_PORT_BIN) $(BUILD_DIR)/usr/bin/tcc; \
	    mkdir -p $(BUILD_DIR)/usr/lib/tcc; \
	    cp -r $(TCC_PORT_TREE)/lib/tcc/* $(BUILD_DIR)/usr/lib/tcc/ 2>/dev/null || true; \
	    cp $(TCC_PORT_TREE)/lib/libtcc.a $(BUILD_DIR)/usr/lib/ 2>/dev/null || true; \
	    cp $(TCC_PORT_TREE)/include/libtcc.h $(BUILD_DIR)/usr/include/ 2>/dev/null || true; \
	    ln -sf /usr/bin/tcc $(BUILD_DIR)/usr/bin/cc; \
	    ln -sf /usr/bin/tcc $(BUILD_DIR)/bin/tcc; \
	    ln -sf /usr/bin/tcc $(BUILD_DIR)/bin/cc; \
	    echo "  ✓  On-device compiler: tcc ($$(wc -c < $(TCC_PORT_BIN)) bytes) -> /usr/bin/tcc, cc"; \
	else \
	    echo "  ·  No on-device compiler staged (build one with: make -C tools/linux ports)"; \
	fi

	@# ── glibc loader + shared libc, for dynamically linked Linux binaries ─
	@# Nothing to do with the compiler; this is the PT_INTERP path of the
	@# Linux-ABI layer (docs/LINUX-BINARIES.md).
	@cp -L /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 $(BUILD_DIR)/lib64/ld-linux-x86-64.so.2 2>/dev/null || true
	@cp -L /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 $(BUILD_DIR)/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 2>/dev/null || true
	@cp -L /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 $(BUILD_DIR)/lib/ld-linux-x86-64.so.2 2>/dev/null || true
	@cp -L /lib/x86_64-linux-gnu/libc.so.6 $(BUILD_DIR)/lib64/libc.so.6 2>/dev/null || true
	@cp -L /lib/x86_64-linux-gnu/libc.so.6 $(BUILD_DIR)/lib/x86_64-linux-gnu/libc.so.6 2>/dev/null || true
	@cp -L /lib/x86_64-linux-gnu/libc.so.6 $(BUILD_DIR)/lib/libc.so.6 2>/dev/null || true
	@cp -L /lib/x86_64-linux-gnu/libc.so.6 $(BUILD_DIR)/usr/lib/libc.so.6 2>/dev/null || true
	@cp -L /lib/x86_64-linux-gnu/libm.so.6 $(BUILD_DIR)/lib64/libm.so.6 2>/dev/null || true
	@cp -L /lib/x86_64-linux-gnu/libm.so.6 $(BUILD_DIR)/lib/x86_64-linux-gnu/libm.so.6 2>/dev/null || true
	@cp -L /lib/x86_64-linux-gnu/libm.so.6 $(BUILD_DIR)/lib/libm.so.6 2>/dev/null || true
	@cp -L /lib/x86_64-linux-gnu/libm.so.6 $(BUILD_DIR)/usr/lib/libm.so.6 2>/dev/null || true

# ── ld-azami.so and the dltest smoke test (see userland/ldso/ldso.c) ───────
# Every object below is ET_DYN, position-independent, and freestanding —
# none of it links against libc.a (see ldso.c's top-of-file comment on why).
# -ftls-model=initial-exec matches what ldso.c's relocation pass actually
# implements (R_X86_64_TPOFF64); the general-dynamic model's
# DTPMOD64/DTPOFF64 relocations, which -fpic would otherwise default TLS
# access to, are unsupported (see that file for why).
DYNLINK_CFLAGS = -std=c11 -ffreestanding -fno-stack-protector -fpic -fno-plt \
                 -m64 -Wall -Wextra -O2 -ftls-model=initial-exec

ldso/ldso.o: ldso/ldso.c libc/include/sys/syscall.h
	$(CC) $(DYNLINK_CFLAGS) -c $< -o $@

ldso/crt0.o: ldso/crt0.asm
	nasm -f elf64 $< -o $@

# IMPORTANT: `toolchain` (a normal, non-order-only prerequisite here — not
# "| toolchain") is deliberate, and the reasoning below is the second half
# of a two-part bug found empirically, not just in theory:
#
#   1. Under -j, GNU make gives no ordering guarantee between a target's own
#      prerequisites, so a plain (non-order-only) `toolchain` prerequisite
#      lets toolchain's `rm -rf $(BUILD_DIR)/lib` recipe race these rules'
#      `mkdir -p $(BUILD_DIR)/lib` and delete the very file they just
#      installed. That's what an order-only "| toolchain" fixes — and is
#      exactly what this file used to say here.
#
#   2. But order-only prerequisites are *never* consulted for "is this
#      target out of date" — only for ordering. Since toolchain is phony (it
#      has no mtime and its recipe unconditionally reruns on every `make`
#      invocation), a *second* `make all` run reproduces the bug the other
#      way around: ld-azami.so/dltest_lib.so already exist and are newer
#      than their real (ldso.o/dltest_lib.o) prerequisites, so make decides
#      they're up to date and never reruns their recipe — but toolchain's
#      rm -rf still deletes them anyway, since it always runs. The result:
#      a first `make all` produces a working image; every `make all` after
#      that silently ships an image with no ld-azami.so/dltest_lib.so/
#      libc.so at all. Confirmed by actually running `make all` twice in a
#      row and checking $(BUILD_DIR)/lib afterward.
#
# A plain `toolchain` prerequisite fixes both at once: since toolchain is
# phony, make treats it as unconditionally "newer" than any real target that
# names it as an ordinary prerequisite, forcing these rules' recipes to
# rerun on every invocation (fixing #2) while still guaranteeing toolchain's
# recipe finishes first (fixing #1) — the tiny, cheap relink cost is not
# worth reintroducing either bug to avoid.
$(BUILD_DIR)/lib/ld-azami.so: ldso/crt0.o ldso/ldso.o toolchain
	@mkdir -p $(BUILD_DIR)/lib
	$(LD) -shared -T ../scripts/user-pic.ld -nostdlib -z max-page-size=0x1000 \
	    ldso/crt0.o ldso/ldso.o -o $@

examples/dltest_lib.o: examples/dltest_lib.c
	$(CC) $(DYNLINK_CFLAGS) -c $< -o $@

$(BUILD_DIR)/lib/dltest_lib.so: examples/dltest_lib.o toolchain
	@mkdir -p $(BUILD_DIR)/lib
	$(LD) -shared --soname=dltest_lib.so -T ../scripts/user-pic.ld -nostdlib \
	    -z max-page-size=0x1000 examples/dltest_lib.o -o $@

examples/dltest_crt0.o: examples/dltest_crt0.asm
	nasm -f elf64 $< -o $@

examples/dltest_app.o: examples/dltest_app.c
	$(CC) $(DYNLINK_CFLAGS) -c $< -o $@

$(BIN_DIR)/dltest_app.elf: examples/dltest_crt0.o examples/dltest_app.o $(BUILD_DIR)/lib/dltest_lib.so
	@mkdir -p $(BIN_DIR)
	$(LD) -pie -T ../scripts/user-pic.ld --dynamic-linker=/lib/ld-azami.so \
	    -nostdlib -z max-page-size=0x1000 \
	    examples/dltest_crt0.o examples/dltest_app.o $(BUILD_DIR)/lib/dltest_lib.so -o $@

# ── libc.so install (see userland/libc/Makefile for the *.pic.o build) ─────
# Same "plain `toolchain` prerequisite" reasoning as ld-azami.so/
# dltest_lib.so above — this one more real target under $(BUILD_DIR)/lib
# subject to the exact same two-part race/idempotency bug.
$(BUILD_DIR)/lib/libc.so: libc/libc.so toolchain
	@mkdir -p $(BUILD_DIR)/lib
	cp libc/libc.so $@

# ── Dynamic PIE apps linked against libc.so (real DT_NEEDED, not eager
# static-libc.a linking) — userland/libc/crt0_dyn.asm is the entry point,
# libc/libc.so (built above) is the real runtime dependency. A small,
# representative subset of the app suite (not a full ~150-app conversion —
# see the top-level task's own guidance on why: real risk of breaking every
# existing app, so the static path stays the default everywhere else):
#   - dlopen_demo.elf: dlopen()/dlsym()/dlclose() exercised for real at
#     runtime (see examples/dlopen_demo.c) — the actual proof dlfcn.c works,
#     distinct from dltest_app.elf's eager-DT_NEEDED-only coverage.
#   - fetch_dyn.elf: apps/fetch/main.c, unmodified, rebuilt dynamic — a
#     printf/malloc-heavy real app, for side-by-side output comparison
#     against the existing static fetch.elf.
#   - thread_tls_test_dyn.elf: examples/thread_tls_test.c, unmodified,
#     rebuilt dynamic — exercises pthread_create() + combined static TLS
#     (libc.so's own __thread errno and pthread.c's TSD arrays) for real,
#     the single riskiest new mechanism in this whole change.
# Same -fpic (not -fpie) choice as DYNLINK_CFLAGS above/dltest_app.c: that's
# the combination already confirmed to link and run correctly for a -pie
# executable with this toolchain, so this reuses it rather than assuming
# -fpie is needed too.
DYNAPP_CFLAGS = -std=c11 -ffreestanding -fno-stack-protector -fpic -fno-plt \
                -m64 -Wall -Wextra -Wno-unused-parameter -O2 -ftls-model=initial-exec \
                -I. -Ilibc/include

DYNAPP_LDFLAGS = -pie -T ../scripts/user-pic.ld --dynamic-linker=/lib/ld-azami.so \
                 -nostdlib -z max-page-size=0x1000

examples/dlopen_demo.o: examples/dlopen_demo.c
	$(CC) $(DYNAPP_CFLAGS) -c $< -o $@

$(BIN_DIR)/dlopen_demo.elf: libc/crt0_dyn.o examples/dlopen_demo.o $(BUILD_DIR)/lib/libc.so $(BUILD_DIR)/lib/dltest_lib.so
	@mkdir -p $(BIN_DIR)
	$(LD) $(DYNAPP_LDFLAGS) libc/crt0_dyn.o examples/dlopen_demo.o $(BUILD_DIR)/lib/libc.so -o $@

$(OBJ_DIR)/dyn_fetch.o: apps/fetch/main.c | skeleton
	$(CC) $(DYNAPP_CFLAGS) -Iapps/shared -c $< -o $@

$(BIN_DIR)/fetch_dyn.elf: libc/crt0_dyn.o $(OBJ_DIR)/dyn_fetch.o $(BUILD_DIR)/lib/libc.so
	@mkdir -p $(BIN_DIR)
	$(LD) $(DYNAPP_LDFLAGS) libc/crt0_dyn.o $(OBJ_DIR)/dyn_fetch.o $(BUILD_DIR)/lib/libc.so -o $@

$(OBJ_DIR)/dyn_thread_tls_test.o: examples/thread_tls_test.c | skeleton
	$(CC) $(DYNAPP_CFLAGS) -c $< -o $@

$(BIN_DIR)/thread_tls_test_dyn.elf: libc/crt0_dyn.o $(OBJ_DIR)/dyn_thread_tls_test.o $(BUILD_DIR)/lib/libc.so
	@mkdir -p $(BIN_DIR)
	$(LD) $(DYNAPP_LDFLAGS) libc/crt0_dyn.o $(OBJ_DIR)/dyn_thread_tls_test.o $(BUILD_DIR)/lib/libc.so -o $@

# Same source, ordinary static build — a side-by-side baseline for the
# per-thread TLS boot test (see the init.c /etc/run-dynlink-test block):
# confirms the combined-static-TLS mechanism in ld-azami.so/libc.so's
# dynamic path behaves identically to the single-module static path already
# in production use, not just "passes in isolation".
$(OBJ_DIR)/thread_tls_test.o: examples/thread_tls_test.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/thread_tls_test.elf: $(OBJ_DIR)/thread_tls_test.o libc/libc.a libc/crt0.o | skeleton
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/thread_tls_test.o libc/libc.a -o $@

.PHONY: dynlink
dynlink: toolchain $(BUILD_DIR)/lib/ld-azami.so $(BUILD_DIR)/lib/libc.so $(BIN_DIR)/dltest_app.elf \
    $(BIN_DIR)/dlopen_demo.elf $(BIN_DIR)/fetch_dyn.elf $(BIN_DIR)/thread_tls_test_dyn.elf \
    $(BIN_DIR)/thread_tls_test.elf




GAME_APPS = \
    $(BIN_DIR)/3d_test.elf \
    $(BIN_DIR)/demo3d.elf \
    $(BIN_DIR)/breakout.elf

BIN_APPS = $(BIN_APPS_SIMPLE) $(BIN_DIR)/settings.elf $(GAME_APPS) $(BIN_DIR)/azami-sh.elf

apps: $(SBIN_APPS) $(BIN_APPS)
	@for f in $(BIN_DIR)/*.elf; do \
		base=$$(basename "$$f" .elf); \
		ln -sf "$$base.elf" "$(BIN_DIR)/$$base" 2>/dev/null || true; \
		ln -sf "/bin/$$base.elf" "$(BUILD_DIR)/usr/bin/$$base" 2>/dev/null || true; \
		ln -sf "/bin/$$base.elf" "$(BUILD_DIR)/usr/bin/$$base.elf" 2>/dev/null || true; \
	done
	@for f in $(SBIN_DIR)/*.elf; do \
		base=$$(basename "$$f" .elf); \
		ln -sf "$$base.elf" "$(SBIN_DIR)/$$base" 2>/dev/null || true; \
	done
	@# ── The system shell ─────────────────────────────────────────────────
	@# GNU bash, built from upstream source by tools/linux/ports.mk, is the
	@# shell: /bin/bash is the binary and /bin/sh and /bin/sh.elf point at
	@# it, so everything that already execs a shell by any of those three
	@# names — libc's system(), the terminal's fallback path, a #!/bin/sh
	@# script, the kernel's emergency init — gets bash without knowing.
	@#
	@# The small native shell is still built and still installed, as
	@# /bin/azami-sh.elf. It is what the image falls back to when the ports
	@# have not been built: an image with no shell at all would be a brick,
	@# and "the shell is missing" is not a failure worth risking to save
	@# 200 KB.
	@rm -f $(BIN_DIR)/sh.elf $(BIN_DIR)/sh $(BIN_DIR)/bash
	@if [ -x $(BASH_PORT_BIN) ]; then \
	    cp $(BASH_PORT_BIN) $(BIN_DIR)/bash; \
	    ln -sf bash $(BIN_DIR)/sh; \
	    ln -sf bash $(BIN_DIR)/sh.elf; \
	    ln -sf /bin/bash $(BUILD_DIR)/usr/bin/bash 2>/dev/null || true; \
	    ln -sf /bin/bash $(BUILD_DIR)/usr/bin/sh 2>/dev/null || true; \
	    ln -sf /bin/bash $(BUILD_DIR)/usr/bin/sh.elf 2>/dev/null || true; \
	    echo "  ✓  System shell: GNU bash ($$(wc -c < $(BASH_PORT_BIN)) bytes) -> /bin/{sh,sh.elf,bash}"; \
	else \
	    ln -sf azami-sh.elf $(BIN_DIR)/sh.elf; \
	    ln -sf azami-sh.elf $(BIN_DIR)/sh; \
	    ln -sf azami-sh.elf $(BIN_DIR)/bash; \
	    ln -sf /bin/azami-sh.elf $(BUILD_DIR)/usr/bin/sh 2>/dev/null || true; \
	    echo "  ·  System shell: native azami-sh (bash not built — make -C tools/linux ports)"; \
	fi
	@ln -sf test.elf $(BIN_DIR)/[ 2>/dev/null || true
	@ln -sf /bin/test.elf $(BUILD_DIR)/usr/bin/[ 2>/dev/null || true
	@ln -sf /bin/env.elf $(BUILD_DIR)/usr/bin/env 2>/dev/null || true
	@ln -sf pgrep.elf $(BIN_DIR)/pkill 2>/dev/null || true
	@ln -sf /bin/pgrep.elf $(BUILD_DIR)/usr/bin/pkill 2>/dev/null || true
	@ln -sf gzip.elf $(BIN_DIR)/gunzip 2>/dev/null || true
	@ln -sf /bin/gzip.elf $(BUILD_DIR)/usr/bin/gunzip 2>/dev/null || true
	@ln -sf sha256sum.elf $(BIN_DIR)/sha224sum 2>/dev/null || true
	@ln -sf sha256sum.elf $(BIN_DIR)/sha384sum 2>/dev/null || true
	@ln -sf sha256sum.elf $(BIN_DIR)/sha512sum 2>/dev/null || true
	@ln -sf /bin/sha256sum.elf $(BUILD_DIR)/usr/bin/sha224sum 2>/dev/null || true
	@ln -sf /bin/sha256sum.elf $(BUILD_DIR)/usr/bin/sha384sum 2>/dev/null || true
	@ln -sf /bin/sha256sum.elf $(BUILD_DIR)/usr/bin/sha512sum 2>/dev/null || true

etc: skeleton
	@echo "root:x:0:0:root:/root:/bin/bash" > $(BUILD_DIR)/etc/passwd
	@echo "azami:x:1000:1000:Azami User:/home/azami:/bin/bash" >> $(BUILD_DIR)/etc/passwd
	@echo "root:4813494d137e1631bba301d5acab6e7bb7aa74ce1185d456565ef51d737677b2:19000:0:99999:7:::" > $(BUILD_DIR)/etc/shadow
	@echo "azami:f58bf0947db3ff9d404be3b7fca4251831d8f2c4dd0b618cfcce7b7a2fcd11ac:19000:0:99999:7:::" >> $(BUILD_DIR)/etc/shadow
	@chmod 600 $(BUILD_DIR)/etc/shadow
	@echo "root:x:0:" > $(BUILD_DIR)/etc/group
	@echo "wheel:x:10:root,azami" >> $(BUILD_DIR)/etc/group
	@echo "users:x:100:azami" >> $(BUILD_DIR)/etc/group
	@echo "azamios" > $(BUILD_DIR)/etc/hostname
	@echo 'NAME="AzamiOS"' > $(BUILD_DIR)/etc/os-release
	@echo 'VERSION="7.0"' >> $(BUILD_DIR)/etc/os-release
	@echo 'ID=azamios' >> $(BUILD_DIR)/etc/os-release
	@echo 'PRETTY_NAME="AzamiOS v7.0 (x86_64)"' >> $(BUILD_DIR)/etc/os-release
	@echo "===================================================================" > $(BUILD_DIR)/etc/motd
	@echo "===================================================================" >> $(BUILD_DIR)/etc/motd
	@echo "/dev/sata0p2   /       ext2    defaults    1 1" > $(BUILD_DIR)/etc/fstab
	@echo "/dev/sata0p1   /boot   ext2    defaults    1 2" >> $(BUILD_DIR)/etc/fstab
	@echo "proc           /proc   procfs  defaults    0 0" >> $(BUILD_DIR)/etc/fstab
	@echo "dev            /dev    devfs   defaults    0 0" >> $(BUILD_DIR)/etc/fstab
	@echo "sys            /sys    sysfs   defaults    0 0" >> $(BUILD_DIR)/etc/fstab
	@echo "devpts         /dev/pts devpts defaults    0 0" >> $(BUILD_DIR)/etc/fstab
	@echo "tmpfs          /tmp    tmpfs   defaults    0 0" >> $(BUILD_DIR)/etc/fstab
	@echo "Europe/Warsaw" > $(BUILD_DIR)/etc/timezone
	@printf "127.0.0.1   localhost azamios\n::1         localhost ip6-localhost ip6-loopback\n" > $(BUILD_DIR)/etc/hosts
	@printf "nameserver 10.0.2.3\nnameserver 1.1.1.1\nnameserver 8.8.8.8\n" > $(BUILD_DIR)/etc/resolv.conf
	@printf "[theme]\ntheme_id=0\nname=Catppuccin Mocha\nwallpaper=/usr/share/wallpapers/default.raw\n\n[display]\nvsync=1\ncompositing=1\ncursor_aa=1\nfps=60\n\n[panel]\nposition=bottom\nheight=32\nautohide=0\nshow_clock=1\n" > $(BUILD_DIR)/etc/desktop.conf
	@printf "[terminal]\ntheme=catppuccin\nfont_size=12\nopacity=95\nscrollback=500\ncursor_blink=1\nhistory_size=100\nprompt=root@azamios:\w$$ \n" > $(BUILD_DIR)/etc/terminal.conf
	@printf "[audio]\nvolume=80\nmaster_mute=0\nsample_rate=44100\nchannels=2\ndefault_sink=/dev/dsp\nmusic_dir=/music\n" > $(BUILD_DIR)/etc/audio.conf
	@printf "[network]\ninterface=eth0\ndhcp=0\nip=10.0.2.15\nnetmask=255.255.255.0\ngateway=10.0.2.2\ndns=10.0.2.3\n" > $(BUILD_DIR)/etc/network.conf
	@printf "[power]\nprofile=1\nscreen_timeout=15\nacpi_pm=1\n" > $(BUILD_DIR)/etc/power.conf
	@printf "# AzamiOS Security & Auto-Accept Configuration\nipc_autoaccept=1\nadmin_autoaccept=1\ndhcp_autoaccept=1\ntrace_autoaccept=1\n" > $(BUILD_DIR)/etc/security.conf
	@printf "# AzamiOS Desktop Session Configuration\n[services]\nwallpaper=/sbin/wallpaper.elf\ntaskbar=/sbin/taskbar.elf\nnotifyd=/sbin/notifyd.elf\n\n[autostart]\nsettings=/bin/settings.elf\nterminal=/bin/terminal.elf\n" > $(BUILD_DIR)/etc/session.conf
	@printf "# Desktop Icons: Label|Path|Glyph|0xAARRGGBB|IsDir|FileType\nTerminal|/bin/terminal.elf|>_|0xFF89B4FA|0|0\nFiles|/bin/filemanager.elf|[_]|0xFFF9E2AF|0|0\nPhotos|/bin/imageviewer.elf|Pic|0xFF89DCEB|0|0\nNotes|/bin/notes.elf|Not|0xFFFFF275|0|0\nMusic|/music|Mus|0xFFF5C2E7|1|2\nAudio|/bin/audioplayer.elf|Snd|0xFFF5C2E7|0|0\nCalculator|/bin/calculator.elf|+-|0xFFFAB387|0|0\nText Editor|/bin/texteditor.elf|Txt|0xFF94E2D5|0|0\nCode Studio|/bin/ide.elf|IDE|0xFFCBA6F7|0|0\nFont Viewer|/bin/fontviewer.elf|Aa|0xFFFAB387|0|0\nTemplates|/home/azami/Templates|Tpl|0xFFA6E3A1|1|1\nPaint|/bin/paint.elf|Art|0xFFCBA6F7|0|0\nSys Monitor|/bin/sysmon.elf|CPU|0xFFF38BA8|0|0\nSettings|/bin/settings.elf|Set|0xFF74C7EC|0|0\n" > $(BUILD_DIR)/etc/desktop_icons.conf
	@# ── Desktop app config: taskbar dock, launcher registry, filemanager
	@# places, and MIME associations. Each app ships a compiled-in default
	@# matching this exact content (see tb_load_default_dock(),
	@# load_default_registry(), load_default_places(),
	@# load_default_file_assoc()) so a missing/corrupt file degrades
	@# gracefully — but these are the real, editable source of truth.
	@printf "# Quick Launch Dock entries: glyph|label|path|0xAARRGGBB\nT|Terminal|/bin/terminal.elf|0xFFA6E3A1\nF|Files|/bin/filemanager.elf|0xFFF9E2AF\nI|Images|/bin/imageviewer.elf|0xFF89DCEB\nN|Notes|/bin/notes.elf|0xFFF9E2AF\nD|Studio|/bin/ide.elf|0xFFCBA6F7\nV|Fonts|/bin/fontviewer.elf|0xFFFAB387\nE|Editor|/bin/texteditor.elf|0xFF89B4FA\nC|Calc|/bin/calculator.elf|0xFFFAB387\nP|Paint|/bin/paint.elf|0xFFCBA6F7\nA|Audio|/bin/audioplayer.elf|0xFFF38BA8\nS|Settings|/bin/settings.elf|0xFF74C7EC\nM|Sysmon|/bin/sysmon.elf|0xFF94E2D5\nX|XClock|/bin/xclock.elf|0xFF89DCEB\nY|XEyes|/bin/xeyes.elf|0xFFF5C2E7\nK|XCalc|/bin/xcalc.elf|0xFFF9E2AF\nG|XDemo|/bin/xgui_demo.elf|0xFFB4BEFE\n" > $(BUILD_DIR)/etc/taskbar_dock.conf
	@printf "# App Launcher registry: appname=category:subtitle\n#\n# This is an allowlist. The launcher shows the apps named here and\n# nothing else, so the ~200 command-line tools in /bin stay out of the\n# app grid instead of appearing as entries that open no window. A new\n# GUI app becomes visible by adding one line.\n# Categories: system, productivity, media, games.\nterminal=system:Command Terminal\nfilemanager=system:Files & Storage\nsettings=system:System Control\nsysmon=system:Activity Monitor\nabout=system:System Info\nfetch=system:System Telemetry\nscreenshot=system:Screen Capture\nfontviewer=system:Font & Typography Inspector\ntexteditor=productivity:Code & Text Editor\nide=productivity:Code Studio & IDE\nnotes=productivity:Sticky Notes\ncalculator=productivity:Math & Calculations\nclock=productivity:Clock & World Times\npaint=productivity:Paint & Sketch\nimageviewer=media:Image & Photo Viewer\naudioplayer=media:Music & Synthwave\nminesweeper=games:Minesweeper Puzzle\n2048=games:2048 Number Puzzle\nsnake=games:Arcade Snake Game\npasjans=games:Solitaire Card Game\nbreakout=games:Breakout Arcade\n3d_test=games:3D Renderer Demo\ndemo3d=games:3D Scene Demo\nxcalc=system:X11 Calculator\nxclock=system:X11 Clock\nxeyes=system:X11 Eyes\nxgui_demo=system:X11 Widget Demo\n" > $(BUILD_DIR)/etc/launcher.conf
	@printf "# File manager sidebar places: label|path|icon\nRoot|/|/\nBoot|/boot|O\nMusic|/music|M\nDesktop|/home/azami/Desktop|D\nHard Disk|/hdd|H\nBinaries|/bin|B\nSystem|/sbin|S\nConfig|/etc|E\nProc FS|/proc|P\nDevices|/dev|D\nTemporary|/tmp|T\n" > $(BUILD_DIR)/etc/filemanager_places.conf
	@printf "# AzamiOS Kernel Runtime Configuration\n# File: /etc/sysctl.conf\n\n# Security Hardening Mitigations\nkernel.dmesg_restrict = 1\nkernel.kptr_restrict = 1\nkernel.mmap_min_addr = 65536\nkernel.yama.ptrace_scope = 1\nfs.protected_hardlinks = 1\nfs.protected_symlinks = 1\n\n# System & Hostname Defaults\nkernel.hostname = azamios\nfs.file-max = 65536\n" > $(BUILD_DIR)/etc/sysctl.conf
	@printf "# File extension -> GUI app associations: ext=/path/to/app.elf\ntxt=/bin/texteditor.elf\nmd=/bin/texteditor.elf\nc=/bin/texteditor.elf\nh=/bin/texteditor.elf\nconf=/bin/texteditor.elf\ncfg=/bin/texteditor.elf\nini=/bin/texteditor.elf\nlog=/bin/texteditor.elf\nsh=/bin/texteditor.elf\njson=/bin/texteditor.elf\nppm=/bin/imageviewer.elf\nbmp=/bin/imageviewer.elf\ntga=/bin/imageviewer.elf\nqoi=/bin/imageviewer.elf\nicn=/bin/imageviewer.elf\npng=/bin/imageviewer.elf\nazf=/bin/fontviewer.elf\nwav=/bin/audioplayer.elf\nmp3=/bin/audioplayer.elf\n" > $(BUILD_DIR)/etc/mime.conf
	@printf "/bin/bash\n/bin/sh\n/bin/sh.elf\n/usr/bin/bash\n/usr/bin/sh\n/bin/azami-sh.elf\n" > $(BUILD_DIR)/etc/shells
	@printf "Welcome to AzamiOS 7.0 (x86_64) - \\\\l\n\n" > $(BUILD_DIR)/etc/issue
	@printf "ext2\nprocfs\ndevfs\n*\n" > $(BUILD_DIR)/etc/filesystems
	@printf "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin\nLANG=C.UTF-8\nSHELL=/bin/bash\nEDITOR=/bin/texteditor.elf\nPAGER=/bin/less.elf\n" > $(BUILD_DIR)/etc/environment
	@# ── Shell startup files ──────────────────────────────────────────────
	@# Two shells read these, so they are split by who can parse what.
	@#
	@# /etc/profile and ~/.profile are the plain ones — one `export K=V` per
	@# line, no quoting, no conditionals, no prompt. The native shell
	@# (/bin/azami-sh.elf) sources both at startup and its parser splits on
	@# spaces, so a line like `export PS1="root@azamios:\w\$$ "` came back
	@# at it as `PS1="root@azamios:\w\$$: command not found`.
	@#
	@# ~/.bashrc and ~/.bash_profile are bash's and hold everything bash
	@# needs but the native shell cannot read: the prompt, history settings,
	@# and the sourcing that makes a login shell pick up the interactive
	@# file. bash reads /etc/profile + ~/.bash_profile when it is a login
	@# shell and ~/.bashrc when it is interactive and not, which is why the
	@# prompt has to live in ~/.bashrc and be pulled in from the other side.
	@#
	@# COMPILER_PATH/LIBRARY_PATH are gone from all of these: they pointed
	@# into the cross-gcc the image no longer packs (see `toolchain`).
	@printf "export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin\nexport USER=root\nexport HOME=/root\nexport TERM=azami\nexport SHELL=/bin/bash\nexport TMPDIR=/tmp\nexport PAGER=/bin/less.elf\nexport EDITOR=/bin/texteditor.elf\n" > $(BUILD_DIR)/etc/profile
	@cp $(BUILD_DIR)/etc/profile $(BUILD_DIR)/root/.profile
	@printf "# root's interactive bash: prompt and history.\nPS1=\"root@azamios:\w\\$$ \"\nHISTFILE=/root/.bash_history\nHISTSIZE=500\nHISTFILESIZE=500\nexport PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin\n" > $(BUILD_DIR)/root/.bashrc
	@printf "[ -f /etc/profile ] && . /etc/profile\n[ -f /root/.bashrc ] && . /root/.bashrc\n" > $(BUILD_DIR)/root/.bash_profile
	@printf "export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin\nexport USER=azami\nexport HOME=/home/azami\nexport TERM=azami\nexport SHELL=/bin/bash\nexport TMPDIR=/tmp\nexport PAGER=/bin/less.elf\nexport EDITOR=/bin/texteditor.elf\n" > $(BUILD_DIR)/home/azami/.profile
	@printf "# azami's interactive bash: prompt and history.\nPS1=\"azami@azamios:\w\\$$ \"\nHISTFILE=/home/azami/.bash_history\nHISTSIZE=500\nHISTFILESIZE=500\nexport PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin\n" > $(BUILD_DIR)/home/azami/.bashrc
	@printf "[ -f /etc/profile ] && . /etc/profile\n[ -f /home/azami/.bashrc ] && . /home/azami/.bashrc\n" > $(BUILD_DIR)/home/azami/.bash_profile
	@cp $(BUILD_DIR)/etc/terminal.conf $(BUILD_DIR)/root/.config/terminal.conf
	@printf "[terminal]\ntheme=catppuccin\nfont_size=12\nopacity=95\nscrollback=500\ncursor_blink=1\nhistory_size=100\nprompt=azami@azamios:\w$$ \n" > $(BUILD_DIR)/home/azami/.config/terminal.conf
	@mkdir -p $(BUILD_DIR)/tmp $(BUILD_DIR)/var/tmp $(BUILD_DIR)/var/log $(BUILD_DIR)/var/run
	@touch $(BUILD_DIR)/var/run/utmp $(BUILD_DIR)/var/log/wtmp 2>/dev/null || true
	@rm -f $(BUILD_DIR)/etc/run-userspace-test $(BUILD_DIR)/etc/run-toolchain-selftest $(BUILD_DIR)/etc/run-dynlink-test
ifeq ($(SELFTESTS),1)
	@touch $(BUILD_DIR)/etc/run-userspace-test $(BUILD_DIR)/etc/run-toolchain-selftest $(BUILD_DIR)/etc/run-dynlink-test 2>/dev/null || true
	@echo "  +  Boot self-test suites enabled (SELFTESTS=1)"
endif
	@mkdir -p $(BUILD_DIR)/examples
	@mkdir -p $(BUILD_DIR)/usr/src/examples
	@mkdir -p $(BUILD_DIR)/usr/src/libc
	@mkdir -p $(BUILD_DIR)/usr/src/azami-libc
	@mkdir -p $(BUILD_DIR)/home/azami/libc
	@cp -r libc/* $(BUILD_DIR)/usr/src/libc/ 2>/dev/null || true
	@cp -r libc/* $(BUILD_DIR)/usr/src/azami-libc/ 2>/dev/null || true
	@cp -r libc/* $(BUILD_DIR)/home/azami/libc/ 2>/dev/null || true
	@printf '#!/bin/sh\n\
# AzamiOS libc native build script\n\
# Usage: cd /usr/src/libc && sh build.sh\n\
set -e\n\
LIBC_DIR=/usr/src/libc\n\
cd "$$LIBC_DIR"\n\
echo "=== Building AzamiOS libc with native GCC 14.2.0 ==="\n\
CFLAGS="-std=c11 -ffreestanding -fno-stack-protector -fno-pie -fno-pic -m64 -O2 -I. -Iinclude"\n\
SRCS="syscall.c stdio.c stdlib.c string.c ctype.c dirent.c time.c math.c az_ipc.c signal.c mman.c pthread.c termios.c resource.c getopt.c acl.c x11.c socket.c netdb.c pwd.c grp.c regex.c fnmatch.c glob.c spawn.c syslog.c search.c tls.c"\n\
echo "  Compiling C sources..."\n\
gcc $$CFLAGS -c $$SRCS\n\
echo "  Assembling crt0.asm..."\n\
nasm -f elf64 crt0.asm -o crt0.o\n\
echo "  Archiving libc.a..."\n\
ar rcs libc.a syscall.o stdio.o stdlib.o string.o ctype.o dirent.o time.o math.o az_ipc.o signal.o mman.o pthread.o termios.o resource.o getopt.o acl.o x11.o socket.o netdb.o pwd.o grp.o regex.o fnmatch.o glob.o spawn.o syslog.o search.o tls.o\n\
echo "  Installing to /usr/lib /lib /lib64 ..."\n\
cp libc.a  /usr/lib/libc.a  2>/dev/null || true\n\
cp libc.a  /usr/lib/libm.a  2>/dev/null || true\n\
cp libc.a  /lib/libc.a      2>/dev/null || true\n\
cp libc.a  /lib/libm.a      2>/dev/null || true\n\
cp libc.a  /lib64/libc.a    2>/dev/null || true\n\
cp crt0.o  /usr/lib/crt0.o  2>/dev/null || true\n\
cp crt0.o  /lib/crt0.o      2>/dev/null || true\n\
cp crt0.o  /lib64/crt0.o    2>/dev/null || true\n\
GCCLIB=/usr/lib/gcc/x86_64-elf/14.2.0\n\
cp libc.a  $$GCCLIB/libc.a   2>/dev/null || true\n\
cp crt0.o  $$GCCLIB/crt0.o   2>/dev/null || true\n\
echo "=== libc.a built and installed successfully! ==="\n\
echo "Test: gcc hello.c -o hello && ./hello"\n\
' > $(BUILD_DIR)/usr/src/libc/build.sh
	@chmod +x $(BUILD_DIR)/usr/src/libc/build.sh 2>/dev/null || true
	@cp $(BUILD_DIR)/usr/src/libc/build.sh $(BUILD_DIR)/home/azami/libc/build.sh 2>/dev/null || true
	@cp -r examples/*.c $(BUILD_DIR)/examples/ 2>/dev/null || true
	@cp -r examples/*.c $(BUILD_DIR)/usr/src/examples/ 2>/dev/null || true
	@cp -r examples/*.c $(BUILD_DIR)/home/azami/ 2>/dev/null || true
	@cp -r examples/*.c $(BUILD_DIR)/root/ 2>/dev/null || true
	@printf "=== Building C programs on AzamiOS ===\n\nThe compiler is TinyCC (/usr/bin/tcc, also reachable as cc). Link\nstatically: every native binary on this image is static, and tcc's\ndefault output is dynamic.\n\nExamples in /examples and /usr/src/examples:\n  • hello.c       - Basic Hello World & argument printer\n  • fib.c         - Fibonacci calculation\n  • mandelbrot.c  - ASCII fractal generator\n  • primes.c      - Sieve of Eratosthenes prime calculator\n  • syscall_test.c- POSIX file I/O, malloc, fork/waitpid\n  • memory_test.c - Dynamic linked list & realloc test\n\nFull libc source in /usr/src/libc (build script: build.sh).\n\nCompile and run:\n  cc -static hello.c -o hello && ./hello\n  cc -static fib.c -o fib && ./fib 15\n  cc -static mandelbrot.c -o mandelbrot && ./mandelbrot\n\nGNU Make is a package away if you want a Makefile: pkg install make\nSee /usr/share/doc/azami/TOOLCHAIN.md (pkg install azami-docs).\n" > $(BUILD_DIR)/home/azami/README_BUILDING.txt
	@cp $(BUILD_DIR)/home/azami/README_BUILDING.txt $(BUILD_DIR)/root/README_BUILDING.txt
	@cp $(BUILD_DIR)/home/azami/README_BUILDING.txt $(BUILD_DIR)/examples/README.txt
	@rm -f $(BUILD_DIR)/home/azami/README_GCC.txt $(BUILD_DIR)/root/README_GCC.txt
	@printf "Welcome to AzamiOS Dynamic Desktop!\n\n• Double-click any icon to launch.\n• Drag icons anywhere to rearrange them.\n• Right-click for context menu (New Notes, Folders, Themes).\n• Open Terminal and type 'config' to view/edit system settings.\n• Try GCC: 'gcc hello.c -o hello && ./hello'\n" > $(BUILD_DIR)/home/azami/Desktop/Welcome_Notes.txt
	@cp $(BUILD_DIR)/home/azami/Desktop/Welcome_Notes.txt $(BUILD_DIR)/desktop/Welcome_Notes.txt 2>/dev/null || true

azamios-src: skeleton
	@echo "  ↓  Copying AzamiOS source tree to /usr/src/azamios..."
	@mkdir -p $(BUILD_DIR)/usr/src/azamios
	@rsync -a --exclude=build --exclude=userland/build --exclude=tools/linux/dl --exclude=tools/linux/out --exclude=tools/linux/log --exclude=tools/limine --exclude=.git ../ $(BUILD_DIR)/usr/src/azamios/ 2>/dev/null || true
	@printf "#!/bin/sh\n\
# AzamiOS self-hosting build script\n\
# Usage: cd /usr/src/azamios && sh build.sh\n\
set -e\n\
cd /usr/src/azamios\n\
echo \"=== Building AzamiOS natively ===\"\n\
make CROSS_PREFIX=\"\" -j4\n\
echo \"=== Build complete! ===\"\n\
" > $(BUILD_DIR)/usr/src/azamios/build.sh
	@chmod +x $(BUILD_DIR)/usr/src/azamios/build.sh 2>/dev/null || true
	@cp $(BUILD_DIR)/usr/src/azamios/build.sh $(BUILD_DIR)/home/azami/build_azamios.sh 2>/dev/null || true

icons: skeleton
	@for f in apps/*/*.icn; do \
		if [ -f "$$f" ]; then \
			appname=$$(basename $$(dirname "$$f")); \
			cp "$$f" $(BUILD_DIR)/usr/share/icons/$$appname.icn; \
			cp "$$f" $(BUILD_DIR)/bin/$$appname.icn 2>/dev/null || true; \
		fi \
	done

fonts: skeleton
	@echo "  ↓  Compiling font subsystem binary fonts..."
	@python3 ../scripts/generate_fonts.py
	@printf "[font]\nsystem_font=/usr/share/fonts/vga_regular.azf\nmono_font=/usr/share/fonts/terminus_regular.azf\nsmall_font=/usr/share/fonts/vga_small.azf\nsize=16\n" > $(BUILD_DIR)/etc/font.conf

music: skeleton
	@echo "  ↓  Generating audio tracks for /music..."
	@mkdir -p $(BUILD_DIR)/music $(BUILD_DIR)/home/azami/Music $(BUILD_DIR)/usr/share/music $(BUILD_DIR)/home/azami/Desktop
	@python3 ../scripts/generate_music.py $(BUILD_DIR)/music
	@cp -f music/*.mp3 $(BUILD_DIR)/music/ 2>/dev/null || true
	@cp $(BUILD_DIR)/music/* $(BUILD_DIR)/home/azami/Music/ 2>/dev/null || true
	@cp $(BUILD_DIR)/music/* $(BUILD_DIR)/usr/share/music/ 2>/dev/null || true
	@ln -sf /music $(BUILD_DIR)/home/azami/Desktop/Music 2>/dev/null || true

images: skeleton
	@echo "  ↓  Generating sample images for /home/azami/Pictures..."
	@mkdir -p $(BUILD_DIR)/home/azami/Pictures $(BUILD_DIR)/usr/share/wallpapers $(BUILD_DIR)/examples
	@python3 ../scripts/generate_sample_images.py $(BUILD_DIR)/home/azami/Pictures
	@cp $(BUILD_DIR)/home/azami/Pictures/* $(BUILD_DIR)/examples/ 2>/dev/null || true
	@ln -sf /home/azami/Pictures $(BUILD_DIR)/home/azami/Desktop/Pictures 2>/dev/null || true

templates: skeleton
	@echo "  ↓  Installing application templates..."
	@mkdir -p $(BUILD_DIR)/usr/share/templates $(BUILD_DIR)/home/azami/Templates
	@cp templates/* $(BUILD_DIR)/usr/share/templates/ 2>/dev/null || true
	@cp templates/* $(BUILD_DIR)/home/azami/Templates/ 2>/dev/null || true

# Sample package repository (/repo on the image) + default /etc/pkg/repos.conf,
# so `pkg install` works out of the box with no network: native app bundles
# (cowsay, games, devtools, netutils, ...), data packages (docs, examples,
# extra themes) and, once `make linux` has been run, the stock Linux
# binaries too. The catalogue lives in scripts/generate_pkg_repo.py; the
# same directory can be served over HTTP with `make pkgserve`. See
# docs/PACKAGES.md.
pkgrepo: apps
	@echo "  ↓  Building sample package repository..."
	@python3 ../scripts/generate_pkg_repo.py $(BUILD_DIR) ../tools/linux/out/bin

# ── System binaries (/sbin) + user utilities & applications (/bin) ──────────
#
# Every target in SBIN_APPS and BIN_APPS used to carry its own explicit
# two-line rule — compile apps/NAME/main.c, link it against libc.a — copied
# ~150 times with nothing but the name changing (this file was ~700 lines of
# that alone). A static pattern rule per list does the same compiles and
# links, driven by the two lists already above instead of by a rule per app,
# so adding an app means adding one line to SBIN_APPS/BIN_APPS, not a new
# three-line rule to keep in sync with them. Object files still land at
# $(OBJ_DIR)/NAME.o exactly as before, so nothing downstream (this file's own
# `apps:` target, anything scripted against build/obj/) changes shape.
#
# azwm.elf is the one exception — four source files, not one — and keeps its
# own explicit rule below rather than forcing the pattern to cover that case.

SBIN_APPS_SIMPLE := $(filter-out $(SBIN_DIR)/azwm.elf,$(SBIN_APPS))

# Compiling and linking used to be one recipe keyed only off main.c, which
# hid the .o from make's dependency graph entirely: filemanager.elf's only
# real prerequisite was apps/filemanager/main.c, so build/obj/filemanager.o
# — the file -MMD's generated .d actually attaches header prerequisites to —
# was never itself something anything depended on. Touching a shared header
# like ui_kit.h left every consumer looking up to date; only a full `make
# clean` ever picked it up. Splitting compile and link into separate rules
# gives the .o a real place in the graph: the .elf depends on the .o, the .o
# rule's prerequisites are extended by its .d file (see the "Header
# dependencies" section below), and a stale header now reaches the link the
# normal way — through an .o that's actually stale.
# The native shell's source stays at apps/sh/main.c, but its binary is no
# longer /bin/sh.elf — GNU bash is (see the `apps` target). Only the link
# needs naming here; $(OBJ_DIR)/sh.o comes from the ordinary object rule
# below like every other app's.
$(BIN_DIR)/azami-sh.elf: $(OBJ_DIR)/sh.o libc/libc.a libc/crt0.o | skeleton
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/sh.o libc/libc.a -o $@

$(OBJ_DIR)/%.o: apps/%/main.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@

$(SBIN_APPS_SIMPLE): $(SBIN_DIR)/%.elf: $(OBJ_DIR)/%.o libc/libc.a libc/crt0.o | skeleton
	$(LD) $(LDFLAGS) libc/crt0.o $< libc/libc.a -o $@

$(BIN_APPS_SIMPLE): $(BIN_DIR)/%.elf: $(OBJ_DIR)/%.o libc/libc.a libc/crt0.o | skeleton
	$(LD) $(LDFLAGS) libc/crt0.o $< libc/libc.a -o $@

$(GAME_APPS): $(BIN_DIR)/%.elf: $(OBJ_DIR)/%.o libc/libc.a libc/crt0.o libgame/libgame.a | skeleton
	$(LD) $(LDFLAGS) libc/crt0.o $< libgame/libgame.a libc/libc.a -o $@


# ── settings.elf (Settings Panel) ──────────────────────────────────────────
$(OBJ_DIR)/settings_main.o: apps/settings/main.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/settings_tab_display.o: apps/settings/tab_display.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/settings_tab_audio.o: apps/settings/tab_audio.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/settings_tab_theme.o: apps/settings/tab_theme.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/settings_tab_time.o: apps/settings/tab_time.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/settings_tab_network.o: apps/settings/tab_network.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/settings_tab_power.o: apps/settings/tab_power.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/settings_tab_disks.o: apps/settings/tab_disks.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/settings_tab_security.o: apps/settings/tab_security.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/settings_tab_system.o: apps/settings/tab_system.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@

SETTINGS_OBJS = $(OBJ_DIR)/settings_main.o $(OBJ_DIR)/settings_tab_display.o $(OBJ_DIR)/settings_tab_audio.o \
                $(OBJ_DIR)/settings_tab_theme.o $(OBJ_DIR)/settings_tab_time.o $(OBJ_DIR)/settings_tab_network.o \
                $(OBJ_DIR)/settings_tab_power.o $(OBJ_DIR)/settings_tab_disks.o $(OBJ_DIR)/settings_tab_security.o \
                $(OBJ_DIR)/settings_tab_system.o

$(BIN_DIR)/settings.elf: $(SETTINGS_OBJS) libc/libc.a libc/crt0.o | skeleton
	$(LD) $(LDFLAGS) libc/crt0.o $(SETTINGS_OBJS) libc/libc.a -o $@

# ── azwm.elf (window manager + compositor + DE extension + desktop) ─────────
$(OBJ_DIR)/azwm_main.o: apps/azwm/main.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/azwm_comp.o: apps/azwm/compositor.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/azwm_de.o: apps/azwm/de_compositor.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/azwm_desk.o: apps/azwm/desktop.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@

$(SBIN_DIR)/azwm.elf: $(OBJ_DIR)/azwm_main.o $(OBJ_DIR)/azwm_comp.o $(OBJ_DIR)/azwm_de.o $(OBJ_DIR)/azwm_desk.o libc/libc.a libc/crt0.o | skeleton
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/azwm_main.o $(OBJ_DIR)/azwm_comp.o $(OBJ_DIR)/azwm_de.o $(OBJ_DIR)/azwm_desk.o libc/libc.a -o $@

# ── Header dependencies (must stay last; see CFLAGS += -MMD -MP above) ──────
#
# Every SBIN_APPS_SIMPLE/BIN_APPS object lands at $(OBJ_DIR)/<name>.o, so its
# .d sits right beside it at $(OBJ_DIR)/<name>.d; azwm's four objects are
# named explicitly in its own rule above and listed here the same way. An
# `include` contributes rules, and the first rule make sees sets the default
# goal — including these any earlier would make a stray .o the default
# target instead of `apps`/`all`.
SIMPLE_APP_NAMES := $(notdir $(basename $(SBIN_APPS_SIMPLE) $(BIN_APPS_SIMPLE)))
DEPFILES := $(addprefix $(OBJ_DIR)/,$(addsuffix .d,$(SIMPLE_APP_NAMES))) \
            $(OBJ_DIR)/settings_main.d $(OBJ_DIR)/settings_tab_display.d $(OBJ_DIR)/settings_tab_audio.d $(OBJ_DIR)/settings_tab_theme.d $(OBJ_DIR)/settings_tab_time.d $(OBJ_DIR)/settings_tab_network.d $(OBJ_DIR)/settings_tab_power.d $(OBJ_DIR)/settings_tab_disks.d $(OBJ_DIR)/settings_tab_security.d $(OBJ_DIR)/settings_tab_system.d \
            $(OBJ_DIR)/azwm_main.d $(OBJ_DIR)/azwm_comp.d $(OBJ_DIR)/azwm_de.d $(OBJ_DIR)/azwm_desk.d
-include $(DEPFILES)

# ── Clean ─────────────────────────────────────────────────────────────────────

clean:
	$(MAKE) -C libc clean
	$(MAKE) -C libgame clean
	rm -rf $(BUILD_DIR)
