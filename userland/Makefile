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
         -I. -Ilibc/include -Iapps/shared -Iapps/azwm

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

BIN_APPS = \
    $(BIN_DIR)/testarch.elf \
    $(BIN_DIR)/hwtest.elf \
    $(BIN_DIR)/sh.elf \
    $(BIN_DIR)/strace.elf \
    $(BIN_DIR)/ktrace.elf \
    $(BIN_DIR)/sem_test.elf \
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
    $(BIN_DIR)/settings.elf \
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


.PHONY: all clean libc apps skeleton etc icons fonts music images templates toolchain

all: skeleton libc apps toolchain etc icons fonts music images templates pkgrepo

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

toolchain: skeleton libc
	@rm -rf $(BUILD_DIR)/home/a0wsvg $(BUILD_DIR)/lib $(BUILD_DIR)/lib64 2>/dev/null || true
	@mkdir -p $(BUILD_DIR)/lib64 $(BUILD_DIR)/lib/x86_64-linux-gnu $(BUILD_DIR)/lib $(BUILD_DIR)/usr/lib $(BUILD_DIR)/usr/bin $(BUILD_DIR)/bin $(BUILD_DIR)/usr/local/lib $(BUILD_DIR)/usr/local/bin
	@mkdir -p $(BUILD_DIR)/usr/libexec/gcc/x86_64-elf/14.2.0
	@mkdir -p $(BUILD_DIR)/usr/lib/ldscripts $(BUILD_DIR)/lib/ldscripts $(BUILD_DIR)/usr/lib/bfd-plugins
	@mkdir -p $(BUILD_DIR)/usr/include $(BUILD_DIR)/usr/local/include
	@for tool in gcc g++ c++ cpp as ld ar ranlib nm objdump objcopy strip strings readelf size addr2line c++filt gcov gcov-dump gcov-tool elfedit; do \
		src="/home/a0wsvg/opt/cross-x86_64/bin/x86_64-elf-$$tool"; \
		if [ -f "$$src" ]; then \
			strip --strip-debug "$$src" -o $(BUILD_DIR)/usr/bin/$$tool 2>/dev/null || cp "$$src" $(BUILD_DIR)/usr/bin/$$tool; \
			ln -sf /usr/bin/$$tool $(BUILD_DIR)/bin/$$tool 2>/dev/null || cp $(BUILD_DIR)/usr/bin/$$tool $(BUILD_DIR)/bin/$$tool; \
		fi; \
	done
	@ln -sf /usr/bin/gcc $(BUILD_DIR)/bin/cc 2>/dev/null || cp $(BUILD_DIR)/usr/bin/gcc $(BUILD_DIR)/bin/cc
	@ln -sf /usr/bin/gcc $(BUILD_DIR)/usr/bin/cc 2>/dev/null || true
	@ln -sf /usr/bin/gcc $(BUILD_DIR)/usr/bin/x86_64-elf-gcc 2>/dev/null || true
	@ln -sf /usr/bin/gcc $(BUILD_DIR)/bin/x86_64-elf-gcc 2>/dev/null || true
	@ln -sf /usr/bin/as $(BUILD_DIR)/usr/bin/x86_64-elf-as 2>/dev/null || true
	@ln -sf /usr/bin/as $(BUILD_DIR)/bin/x86_64-elf-as 2>/dev/null || true
	@ln -sf /usr/bin/ld $(BUILD_DIR)/bin/ld.bfd 2>/dev/null || true
	@ln -sf /usr/bin/ld $(BUILD_DIR)/usr/bin/ld.bfd 2>/dev/null || true
	@ln -sf /usr/bin/ld $(BUILD_DIR)/usr/bin/x86_64-elf-ld 2>/dev/null || true
	@ln -sf /usr/bin/ld $(BUILD_DIR)/bin/x86_64-elf-ld 2>/dev/null || true
	@for pass in cc1 collect2 lto-wrapper liblto_plugin.so; do \
		src="/home/a0wsvg/opt/cross-x86_64/libexec/gcc/x86_64-elf/14.2.0/$$pass"; \
		if [ -f "$$src" ]; then \
			strip --strip-debug "$$src" -o $(BUILD_DIR)/usr/libexec/gcc/x86_64-elf/14.2.0/$$pass 2>/dev/null || cp "$$src" $(BUILD_DIR)/usr/libexec/gcc/x86_64-elf/14.2.0/$$pass; \
		fi; \
	done
	@ln -sf /usr/bin/as $(BUILD_DIR)/usr/libexec/gcc/x86_64-elf/14.2.0/as 2>/dev/null || true
	@ln -sf /usr/bin/ld $(BUILD_DIR)/usr/libexec/gcc/x86_64-elf/14.2.0/ld 2>/dev/null || true
	@ln -sf /usr/bin/ld $(BUILD_DIR)/usr/libexec/gcc/x86_64-elf/14.2.0/real-ld 2>/dev/null || true
	@ln -sf /usr/libexec/gcc/x86_64-elf/14.2.0/cc1 $(BUILD_DIR)/usr/libexec/cc1 2>/dev/null || true
	@ln -sf /usr/libexec/gcc/x86_64-elf/14.2.0/collect2 $(BUILD_DIR)/usr/libexec/collect2 2>/dev/null || true
	@ln -sf /usr/libexec/gcc/x86_64-elf/14.2.0/lto-wrapper $(BUILD_DIR)/usr/libexec/lto-wrapper 2>/dev/null || true
	@ln -sf /usr/bin/as $(BUILD_DIR)/usr/libexec/as 2>/dev/null || true
	@ln -sf /usr/bin/ld $(BUILD_DIR)/usr/libexec/ld 2>/dev/null || true
	@ln -sf /usr/bin/ld $(BUILD_DIR)/usr/libexec/real-ld 2>/dev/null || true


	@ln -sf /usr/libexec/gcc/x86_64-elf/14.2.0/liblto_plugin.so $(BUILD_DIR)/usr/lib/bfd-plugins/liblto_plugin.so 2>/dev/null || true
	@ln -sf /usr/libexec/gcc/x86_64-elf/14.2.0/liblto_plugin.so $(BUILD_DIR)/usr/lib/liblto_plugin.so 2>/dev/null || true
	@ln -sf /usr/libexec/gcc/x86_64-elf/14.2.0/liblto_plugin.so $(BUILD_DIR)/lib/liblto_plugin.so 2>/dev/null || true
	@ln -sf /usr/libexec/gcc/x86_64-elf/14.2.0/lto-wrapper $(BUILD_DIR)/usr/bin/lto-wrapper 2>/dev/null || true
	@cp -r /home/a0wsvg/opt/cross-x86_64/x86_64-elf/lib/ldscripts/* $(BUILD_DIR)/usr/lib/ldscripts/ 2>/dev/null || true
	@cp -r /home/a0wsvg/opt/cross-x86_64/x86_64-elf/lib/ldscripts/* $(BUILD_DIR)/lib/ldscripts/ 2>/dev/null || true
	@mkdir -p $(BUILD_DIR)/usr/lib/gcc/x86_64-elf/14.2.0/include $(BUILD_DIR)/usr/lib/gcc/x86_64-elf/14.2.0/include-fixed
	@cp -r /home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/include/* $(BUILD_DIR)/usr/lib/gcc/x86_64-elf/14.2.0/include/ 2>/dev/null || true
	@cp -r /home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/include-fixed/* $(BUILD_DIR)/usr/lib/gcc/x86_64-elf/14.2.0/include-fixed/ 2>/dev/null || true
	@cp -r /home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/include/* $(BUILD_DIR)/usr/include/ 2>/dev/null || true
	@cp -r libc/include/* $(BUILD_DIR)/usr/include/ 2>/dev/null || true
	@cp -r libc/include/* $(BUILD_DIR)/usr/local/include/ 2>/dev/null || true
	@mkdir -p $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/x86_64-elf
	@mkdir -p $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0
	@mkdir -p $(BUILD_DIR)/lib/gcc/x86_64-elf/14.2.0
	@mkdir -p $(BUILD_DIR)/usr/x86_64-elf $(BUILD_DIR)/x86_64-elf $(BUILD_DIR)/usr/lib/gcc/x86_64-elf/14.2.0
	@ln -sf /usr/lib/gcc/x86_64-elf/14.2.0/include $(BUILD_DIR)/lib/gcc/x86_64-elf/14.2.0/include 2>/dev/null || true
	@ln -sf /usr/lib/gcc/x86_64-elf/14.2.0/include-fixed $(BUILD_DIR)/lib/gcc/x86_64-elf/14.2.0/include-fixed 2>/dev/null || true
	@ln -sf /usr/lib/gcc/x86_64-elf/14.2.0/include $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/include 2>/dev/null || true
	@ln -sf /usr/lib/gcc/x86_64-elf/14.2.0/include-fixed $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/include-fixed 2>/dev/null || true
	@ln -sf /usr/bin $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/bin 2>/dev/null || true
	@ln -sf /usr/libexec $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/libexec 2>/dev/null || true
	@ln -sf /usr/lib $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/lib 2>/dev/null || true
	@ln -sf /usr/include $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/include 2>/dev/null || true
	@ln -sf /usr/bin $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/x86_64-elf/bin 2>/dev/null || true
	@ln -sf /usr/lib $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/x86_64-elf/lib 2>/dev/null || true
	@ln -sf /usr/include $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/x86_64-elf/include 2>/dev/null || true
	@ln -sf /usr/local/include $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/x86_64-elf/sys-include 2>/dev/null || true
	@ln -sf /usr/bin $(BUILD_DIR)/usr/x86_64-elf/bin 2>/dev/null || true
	@ln -sf /usr/lib $(BUILD_DIR)/usr/x86_64-elf/lib 2>/dev/null || true
	@ln -sf /usr/include $(BUILD_DIR)/usr/x86_64-elf/include 2>/dev/null || true
	@ln -sf /usr/local/include $(BUILD_DIR)/usr/x86_64-elf/sys-include 2>/dev/null || true
	@ln -sf /usr/bin $(BUILD_DIR)/x86_64-elf/bin 2>/dev/null || true
	@ln -sf /usr/lib $(BUILD_DIR)/x86_64-elf/lib 2>/dev/null || true
	@ln -sf /usr/include $(BUILD_DIR)/x86_64-elf/include 2>/dev/null || true
	@ln -sf /usr/local/include $(BUILD_DIR)/x86_64-elf/sys-include 2>/dev/null || true
	@ln -sf /usr/libexec $(BUILD_DIR)/x86_64-elf/libexec 2>/dev/null || true
	@for d in $(BUILD_DIR)/usr/lib $(BUILD_DIR)/lib $(BUILD_DIR)/lib64 $(BUILD_DIR)/usr/local/lib $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0 $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/x86_64-elf/lib $(BUILD_DIR)/usr/lib/gcc/x86_64-elf/14.2.0 $(BUILD_DIR)/x86_64-elf/lib; do \
		mkdir -p "$$d" 2>/dev/null || true; \
		cp /home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/crtbegin*.o "$$d"/ 2>/dev/null || true; \
		cp /home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/crtend*.o "$$d"/ 2>/dev/null || true; \
		cp /home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/libgcc*.a "$$d"/ 2>/dev/null || true; \
		cp libc/crt0.o "$$d"/crt0.o 2>/dev/null || true; \
		cp libc/libc.a "$$d"/libc.a 2>/dev/null || true; \
		cp libc/libc.a "$$d"/libm.a 2>/dev/null || true; \
		cp libc/libc.a "$$d"/libg.a 2>/dev/null || true; \
		cp libc/libc.a "$$d"/libnosys.a 2>/dev/null || true; \
	done
	@mkdir -p $(BUILD_DIR)/lib64 $(BUILD_DIR)/lib/x86_64-linux-gnu $(BUILD_DIR)/lib $(BUILD_DIR)/usr/lib
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
	@ln -sf sh.elf $(BIN_DIR)/bash 2>/dev/null || true
	@ln -sf sh.elf $(BIN_DIR)/sh 2>/dev/null || true
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
	@echo "root:x:0:0:root:/root:/bin/sh.elf" > $(BUILD_DIR)/etc/passwd
	@echo "azami:x:1000:1000:Azami User:/home/azami:/bin/sh.elf" >> $(BUILD_DIR)/etc/passwd
	@echo "root:4813494d137e1631bba301d5acab6e7bb7aa74ce1185d456565ef51d737677b2:19000:0:99999:7:::" > $(BUILD_DIR)/etc/shadow
	@echo "azami:f58bf0947db3ff9d404be3b7fca4251831d8f2c4dd0b618cfcce7b7a2fcd11ac:19000:0:99999:7:::" >> $(BUILD_DIR)/etc/shadow
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
	@printf "# App Launcher registry: appname=category:subtitle, or appname=hidden\nterminal=system:Command Terminal\nfilemanager=system:Files & Storage\nimageviewer=media:Image & Photo Viewer\nnotes=productivity:Sticky Notes\ntexteditor=productivity:Code & Text Editor\nide=productivity:Code Studio & IDE\nfontviewer=system:Font & Typography Inspector\nsysmon=system:Activity Monitor\ncalculator=productivity:Math & Calculations\nclock=productivity:Clock & World Times\nsettings=system:System Control\nabout=system:System Info\nfetch=system:System Telemetry\nscreenshot=system:Screen Capture\npaint=productivity:Paint & Sketch\naudioplayer=media:Music & Synthwave\nminesweeper=games:Minesweeper Puzzle\n2048=games:2048 Number Puzzle\nsnake=games:Arcade Snake Game\n# Internal services & CLI tools: excluded from the app grid\ninit=hidden\nsessiond=hidden\nazwm=hidden\nwallpaper=hidden\ntaskbar=hidden\nlauncher=hidden\ngui_test=hidden\nsh=hidden\nplay=hidden\nls=hidden\ncat=hidden\necho=hidden\npwd=hidden\nuname=hidden\nhead=hidden\ntail=hidden\nwc=hidden\ngrep=hidden\nmkdir=hidden\nrm=hidden\ntouch=hidden\ncp=hidden\nmv=hidden\ndate=hidden\nuptime=hidden\ndf=hidden\nfree=hidden\nifconfig=hidden\nping=hidden\nlspci=hidden\nenv=hidden\nwhich=hidden\nsleep=hidden\nkill=hidden\nchmod=hidden\nclear=hidden\nhexdump=hidden\nbase64=hidden\nmd5sum=hidden\ncut=hidden\nsort=hidden\nuniq=hidden\nps=hidden\ntop=hidden\ndmesg=hidden\ntree=hidden\ncal=hidden\nwatch=hidden\nnetstat=hidden\npoweroff=hidden\nreboot=hidden\ngetfacl=hidden\nsetfacl=hidden\nless=hidden\nbc=hidden\nhexedit=hidden\n" > $(BUILD_DIR)/etc/launcher.conf
	@printf "# File manager sidebar places: label|path|icon\nRoot|/|/\nBoot|/boot|O\nMusic|/music|M\nDesktop|/home/azami/Desktop|D\nHard Disk|/hdd|H\nBinaries|/bin|B\nSystem|/sbin|S\nConfig|/etc|E\nProc FS|/proc|P\nDevices|/dev|D\nTemporary|/tmp|T\n" > $(BUILD_DIR)/etc/filemanager_places.conf
	@printf "# AzamiOS Kernel Runtime Configuration\n# File: /etc/sysctl.conf\n\n# Security Hardening Mitigations\nkernel.dmesg_restrict = 1\nkernel.kptr_restrict = 1\nkernel.mmap_min_addr = 65536\nkernel.yama.ptrace_scope = 1\nfs.protected_hardlinks = 1\nfs.protected_symlinks = 1\n\n# System & Hostname Defaults\nkernel.hostname = azamios\nfs.file-max = 65536\n" > $(BUILD_DIR)/etc/sysctl.conf
	@printf "# File extension -> GUI app associations: ext=/path/to/app.elf\ntxt=/bin/texteditor.elf\nmd=/bin/texteditor.elf\nc=/bin/texteditor.elf\nh=/bin/texteditor.elf\nconf=/bin/texteditor.elf\ncfg=/bin/texteditor.elf\nini=/bin/texteditor.elf\nlog=/bin/texteditor.elf\nsh=/bin/texteditor.elf\njson=/bin/texteditor.elf\nppm=/bin/imageviewer.elf\nbmp=/bin/imageviewer.elf\ntga=/bin/imageviewer.elf\nqoi=/bin/imageviewer.elf\nicn=/bin/imageviewer.elf\npng=/bin/imageviewer.elf\nazf=/bin/fontviewer.elf\nwav=/bin/audioplayer.elf\nmp3=/bin/audioplayer.elf\n" > $(BUILD_DIR)/etc/mime.conf
	@printf "/bin/sh\n/bin/sh.elf\n/bin/bash\n/usr/bin/sh\n" > $(BUILD_DIR)/etc/shells
	@printf "Welcome to AzamiOS 7.0 (x86_64) - \\\\l\n\n" > $(BUILD_DIR)/etc/issue
	@printf "ext2\nprocfs\ndevfs\n*\n" > $(BUILD_DIR)/etc/filesystems
	@printf "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin\nLANG=C.UTF-8\nEDITOR=/bin/texteditor.elf\nPAGER=/bin/less.elf\n" > $(BUILD_DIR)/etc/environment
	@printf "export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin\nexport USER=root\nexport HOME=/root\nexport TERM=azami\nexport SHELL=/bin/sh.elf\nexport TMPDIR=/tmp\nexport COMPILER_PATH=/usr/libexec/gcc/x86_64-elf/14.2.0/:/usr/libexec/:/usr/bin:/bin\nexport LIBRARY_PATH=/usr/lib/gcc/x86_64-elf/14.2.0/:/usr/lib:/lib:/lib64:/usr/local/lib\nexport PS1=\"root@azamios:\w\\$$ \"\n" > $(BUILD_DIR)/etc/profile
	@cp $(BUILD_DIR)/etc/profile $(BUILD_DIR)/root/.profile
	@printf "export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin\nexport USER=azami\nexport HOME=/home/azami\nexport TERM=azami\nexport SHELL=/bin/sh.elf\nexport TMPDIR=/tmp\nexport COMPILER_PATH=/usr/libexec/gcc/x86_64-elf/14.2.0/:/usr/libexec/:/usr/bin:/bin\nexport LIBRARY_PATH=/usr/lib/gcc/x86_64-elf/14.2.0/:/usr/lib:/lib:/lib64:/usr/local/lib\nexport PS1=\"azami@azamios:\w\\$$ \"\n" > $(BUILD_DIR)/home/azami/.profile
	@cp $(BUILD_DIR)/etc/terminal.conf $(BUILD_DIR)/root/.config/terminal.conf
	@printf "[terminal]\ntheme=catppuccin\nfont_size=12\nopacity=95\nscrollback=500\ncursor_blink=1\nhistory_size=100\nprompt=azami@azamios:\w$$ \n" > $(BUILD_DIR)/home/azami/.config/terminal.conf
	@mkdir -p $(BUILD_DIR)/tmp $(BUILD_DIR)/var/tmp $(BUILD_DIR)/var/log $(BUILD_DIR)/var/run
	@touch $(BUILD_DIR)/var/run/utmp $(BUILD_DIR)/var/log/wtmp $(BUILD_DIR)/etc/run-userspace-test $(BUILD_DIR)/etc/run-toolchain-selftest 2>/dev/null || true
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
	@printf "=== Native GCC 14.2.0 on AzamiOS ===\n\nExamples included in /examples and /usr/src/examples:\n  • hello.c       - Basic Hello World & argument printer\n  • fib.c         - Fibonacci calculation\n  • mandelbrot.c  - ASCII fractal generator\n  • primes.c      - Sieve of Eratosthenes prime calculator\n  • syscall_test.c- POSIX file I/O, malloc, fork/waitpid\n  • memory_test.c - Dynamic linked list & realloc test\n\nFull Libc Source Code in /usr/src/libc:\n  • All C standard library sources (stdio.c, stdlib.c, string.c, syscall.c, etc.)\n  • Build script: /usr/src/libc/build.sh\n\nHow to compile and run:\n  gcc hello.c -o hello && ./hello\n  gcc fib.c -o fib && ./fib 15\n  gcc mandelbrot.c -o mandelbrot && ./mandelbrot\n  gcc primes.c -o primes && ./primes 100\n  gcc syscall_test.c -o syscall_test && ./syscall_test\n" > $(BUILD_DIR)/home/azami/README_GCC.txt
	@cp $(BUILD_DIR)/home/azami/README_GCC.txt $(BUILD_DIR)/root/README_GCC.txt
	@cp $(BUILD_DIR)/home/azami/README_GCC.txt $(BUILD_DIR)/examples/README.txt
	@printf "Welcome to AzamiOS Dynamic Desktop!\n\n• Double-click any icon to launch.\n• Drag icons anywhere to rearrange them.\n• Right-click for context menu (New Notes, Folders, Themes).\n• Open Terminal and type 'config' to view/edit system settings.\n• Try GCC: 'gcc hello.c -o hello && ./hello'\n" > $(BUILD_DIR)/home/azami/Desktop/Welcome_Notes.txt
	@cp $(BUILD_DIR)/home/azami/Desktop/Welcome_Notes.txt $(BUILD_DIR)/desktop/Welcome_Notes.txt 2>/dev/null || true

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
# so `pkg install cowsay` (a native app) and `pkg install busybox` (a stock
# Linux binary, when `make linux` has been run) work out of the box with no
# network. See docs/PACKAGES.md.
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
$(OBJ_DIR)/%.o: apps/%/main.c | skeleton
	$(CC) $(CFLAGS) -c $< -o $@

$(SBIN_APPS_SIMPLE): $(SBIN_DIR)/%.elf: $(OBJ_DIR)/%.o libc/libc.a libc/crt0.o | skeleton
	$(LD) $(LDFLAGS) libc/crt0.o $< libc/libc.a -o $@

$(BIN_APPS): $(BIN_DIR)/%.elf: $(OBJ_DIR)/%.o libc/libc.a libc/crt0.o | skeleton
	$(LD) $(LDFLAGS) libc/crt0.o $< libc/libc.a -o $@

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
SIMPLE_APP_NAMES := $(notdir $(basename $(SBIN_APPS_SIMPLE) $(BIN_APPS)))
DEPFILES := $(addprefix $(OBJ_DIR)/,$(addsuffix .d,$(SIMPLE_APP_NAMES))) \
            $(OBJ_DIR)/azwm_main.d $(OBJ_DIR)/azwm_comp.d $(OBJ_DIR)/azwm_de.d $(OBJ_DIR)/azwm_desk.d
-include $(DEPFILES)

# ── Clean ─────────────────────────────────────────────────────────────────────

clean:
	$(MAKE) -C libc clean
	rm -rf $(BUILD_DIR)
