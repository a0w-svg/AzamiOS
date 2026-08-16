CROSS_PREFIX ?= $(HOME)/opt/cross-x86_64/bin/x86_64-elf-
CC = $(CROSS_PREFIX)gcc
LD = $(CROSS_PREFIX)ld
AR = $(CROSS_PREFIX)ar

CFLAGS = -std=c11 -ffreestanding -fno-stack-protector -fno-pie -fno-pic -m64 \
         -Wall -Wextra -Wno-unused-parameter -O2 \
         -I. -Ilibc/include -Iapps/shared -Iapps/azwm

LDFLAGS = -T ../scripts/user.ld -nostdlib -z max-page-size=0x1000

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
    $(SBIN_DIR)/netstat.elf

BIN_APPS = \
    $(BIN_DIR)/sh.elf \
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
    $(BIN_DIR)/paint.elf

.PHONY: all clean libc apps skeleton etc icons

all: skeleton libc apps etc icons

skeleton:
	mkdir -p $(BIN_DIR) $(SBIN_DIR) $(OBJ_DIR)
	mkdir -p $(BUILD_DIR)/usr/bin $(BUILD_DIR)/usr/share/icons $(BUILD_DIR)/usr/lib $(BUILD_DIR)/usr/include
	mkdir -p $(BUILD_DIR)/etc $(BUILD_DIR)/dev $(BUILD_DIR)/proc $(BUILD_DIR)/sys
	mkdir -p $(BUILD_DIR)/var/log $(BUILD_DIR)/var/run $(BUILD_DIR)/var/tmp $(BUILD_DIR)/tmp
	mkdir -p $(BUILD_DIR)/root $(BUILD_DIR)/home/azami $(BUILD_DIR)/mnt $(BUILD_DIR)/boot

libc:
	$(MAKE) -C libc

apps: $(SBIN_APPS) $(BIN_APPS)

etc: skeleton
	@echo "root:x:0:0:root:/root:/bin/sh.elf" > $(BUILD_DIR)/etc/passwd
	@echo "azami:x:1000:1000:Azami User:/home/azami:/bin/sh.elf" >> $(BUILD_DIR)/etc/passwd
	@echo "root:x:0:" > $(BUILD_DIR)/etc/group
	@echo "wheel:x:10:root,azami" >> $(BUILD_DIR)/etc/group
	@echo "users:x:100:azami" >> $(BUILD_DIR)/etc/group
	@echo "azamios" > $(BUILD_DIR)/etc/hostname
	@echo 'NAME="AzamiOS"' > $(BUILD_DIR)/etc/os-release
	@echo 'VERSION="7.0"' >> $(BUILD_DIR)/etc/os-release
	@echo 'ID=azamios' >> $(BUILD_DIR)/etc/os-release
	@echo 'PRETTY_NAME="AzamiOS v7.0 (x86_64)"' >> $(BUILD_DIR)/etc/os-release
	@echo "===================================================================" > $(BUILD_DIR)/etc/motd
	@echo "  Welcome to AzamiOS v7.0 (x86_64 Modular Microkernel)" >> $(BUILD_DIR)/etc/motd
	@echo "===================================================================" >> $(BUILD_DIR)/etc/motd
	@echo "/dev/ram0   /       ext2    defaults    1 1" > $(BUILD_DIR)/etc/fstab
	@echo "proc        /proc   procfs  defaults    0 0" >> $(BUILD_DIR)/etc/fstab
	@echo "dev         /dev    devfs   defaults    0 0" >> $(BUILD_DIR)/etc/fstab

icons: skeleton
	@for f in apps/*/*.icn; do \
		if [ -f "$$f" ]; then \
			appname=$$(basename $$(dirname "$$f")); \
			cp "$$f" $(BUILD_DIR)/usr/share/icons/$$appname.icn; \
			cp "$$f" $(BUILD_DIR)/bin/$$appname.icn 2>/dev/null || true; \
		fi \
	done

# ── System binaries (/sbin) ──────────────────────────────────────────────────

$(SBIN_DIR)/init.elf: apps/init/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/init.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/init.o libc/libc.a -o $@

$(SBIN_DIR)/sessiond.elf: apps/sessiond/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/sessiond.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/sessiond.o libc/libc.a -o $@

$(SBIN_DIR)/azwm.elf: apps/azwm/main.c apps/azwm/compositor.c apps/azwm/de_compositor.c apps/azwm/desktop.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c apps/azwm/main.c -o $(OBJ_DIR)/azwm_main.o
	$(CC) $(CFLAGS) -c apps/azwm/compositor.c -o $(OBJ_DIR)/azwm_comp.o
	$(CC) $(CFLAGS) -c apps/azwm/de_compositor.c -o $(OBJ_DIR)/azwm_de.o
	$(CC) $(CFLAGS) -c apps/azwm/desktop.c -o $(OBJ_DIR)/azwm_desk.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/azwm_main.o $(OBJ_DIR)/azwm_comp.o $(OBJ_DIR)/azwm_de.o $(OBJ_DIR)/azwm_desk.o libc/libc.a -o $@

$(SBIN_DIR)/wallpaper.elf: apps/wallpaper/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/wallpaper.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/wallpaper.o libc/libc.a -o $@

$(SBIN_DIR)/taskbar.elf: apps/taskbar/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/taskbar.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/taskbar.o libc/libc.a -o $@

$(SBIN_DIR)/poweroff.elf: apps/poweroff/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/poweroff.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/poweroff.o libc/libc.a -o $@

$(SBIN_DIR)/reboot.elf: apps/reboot/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/reboot.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/reboot.o libc/libc.a -o $@

$(SBIN_DIR)/lspci.elf: apps/lspci/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/lspci.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/lspci.o libc/libc.a -o $@

$(SBIN_DIR)/ifconfig.elf: apps/ifconfig/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/ifconfig.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/ifconfig.o libc/libc.a -o $@

$(SBIN_DIR)/ping.elf: apps/ping/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/ping.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/ping.o libc/libc.a -o $@

$(SBIN_DIR)/dmesg.elf: apps/dmesg/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/dmesg.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/dmesg.o libc/libc.a -o $@

$(SBIN_DIR)/netstat.elf: apps/netstat/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/netstat.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/netstat.o libc/libc.a -o $@

# ── User utilities & applications (/bin) ─────────────────────────────────────

$(BIN_DIR)/sh.elf: apps/sh/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/sh.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/sh.o libc/libc.a -o $@

$(BIN_DIR)/ls.elf: apps/ls/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/ls.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/ls.o libc/libc.a -o $@

$(BIN_DIR)/cat.elf: apps/cat/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/cat.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/cat.o libc/libc.a -o $@

$(BIN_DIR)/echo.elf: apps/echo/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/echo.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/echo.o libc/libc.a -o $@

$(BIN_DIR)/play.elf: apps/play/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/play.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/play.o libc/libc.a -o $@

$(BIN_DIR)/gui_test.elf: apps/gui_test/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/gui_test.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/gui_test.o libc/libc.a -o $@

$(BIN_DIR)/launcher.elf: apps/launcher/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/launcher.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/launcher.o libc/libc.a -o $@

$(BIN_DIR)/about.elf: apps/about/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/about.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/about.o libc/libc.a -o $@

$(BIN_DIR)/calculator.elf: apps/calculator/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/calculator.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/calculator.o libc/libc.a -o $@

$(BIN_DIR)/clock.elf: apps/clock/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/clock.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/clock.o libc/libc.a -o $@

$(BIN_DIR)/sysmon.elf: apps/sysmon/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/sysmon.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/sysmon.o libc/libc.a -o $@

$(BIN_DIR)/texteditor.elf: apps/texteditor/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/texteditor.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/texteditor.o libc/libc.a -o $@

$(BIN_DIR)/filemanager.elf: apps/filemanager/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/filemanager.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/filemanager.o libc/libc.a -o $@

$(BIN_DIR)/terminal.elf: apps/terminal/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/terminal.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/terminal.o libc/libc.a -o $@

$(BIN_DIR)/settings.elf: apps/settings/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/settings.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/settings.o libc/libc.a -o $@

$(BIN_DIR)/fetch.elf: apps/fetch/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/fetch.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/fetch.o libc/libc.a -o $@

$(BIN_DIR)/pwd.elf: apps/pwd/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/pwd.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/pwd.o libc/libc.a -o $@

$(BIN_DIR)/uname.elf: apps/uname/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/uname.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/uname.o libc/libc.a -o $@

$(BIN_DIR)/head.elf: apps/head/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/head.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/head.o libc/libc.a -o $@

$(BIN_DIR)/wc.elf: apps/wc/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/wc.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/wc.o libc/libc.a -o $@

$(BIN_DIR)/grep.elf: apps/grep/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/grep.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/grep.o libc/libc.a -o $@

$(BIN_DIR)/mkdir.elf: apps/mkdir/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/mkdir.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/mkdir.o libc/libc.a -o $@

$(BIN_DIR)/rm.elf: apps/rm/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/rm.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/rm.o libc/libc.a -o $@

$(BIN_DIR)/touch.elf: apps/touch/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/touch.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/touch.o libc/libc.a -o $@

$(BIN_DIR)/cp.elf: apps/cp/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/cp.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/cp.o libc/libc.a -o $@

$(BIN_DIR)/mv.elf: apps/mv/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/mv.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/mv.o libc/libc.a -o $@

$(BIN_DIR)/tail.elf: apps/tail/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/tail.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/tail.o libc/libc.a -o $@

$(BIN_DIR)/date.elf: apps/date/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/date.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/date.o libc/libc.a -o $@

$(BIN_DIR)/uptime.elf: apps/uptime/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/uptime.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/uptime.o libc/libc.a -o $@

$(BIN_DIR)/df.elf: apps/df/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/df.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/df.o libc/libc.a -o $@

$(BIN_DIR)/free.elf: apps/free/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/free.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/free.o libc/libc.a -o $@

$(BIN_DIR)/env.elf: apps/env/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/env.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/env.o libc/libc.a -o $@

$(BIN_DIR)/which.elf: apps/which/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/which.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/which.o libc/libc.a -o $@

$(BIN_DIR)/sleep.elf: apps/sleep/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/sleep.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/sleep.o libc/libc.a -o $@

$(BIN_DIR)/kill.elf: apps/kill/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/kill.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/kill.o libc/libc.a -o $@

$(BIN_DIR)/chmod.elf: apps/chmod/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/chmod.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/chmod.o libc/libc.a -o $@

$(BIN_DIR)/clear.elf: apps/clear/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/clear.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/clear.o libc/libc.a -o $@

$(BIN_DIR)/hexdump.elf: apps/hexdump/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/hexdump.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/hexdump.o libc/libc.a -o $@

$(BIN_DIR)/base64.elf: apps/base64/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/base64.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/base64.o libc/libc.a -o $@

$(BIN_DIR)/md5sum.elf: apps/md5sum/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/md5sum.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/md5sum.o libc/libc.a -o $@

$(BIN_DIR)/cut.elf: apps/cut/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/cut.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/cut.o libc/libc.a -o $@

$(BIN_DIR)/sort.elf: apps/sort/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/sort.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/sort.o libc/libc.a -o $@

$(BIN_DIR)/uniq.elf: apps/uniq/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/uniq.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/uniq.o libc/libc.a -o $@

$(BIN_DIR)/ps.elf: apps/ps/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/ps.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/ps.o libc/libc.a -o $@

$(BIN_DIR)/top.elf: apps/top/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/top.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/top.o libc/libc.a -o $@

$(BIN_DIR)/tree.elf: apps/tree/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/tree.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/tree.o libc/libc.a -o $@

$(BIN_DIR)/cal.elf: apps/cal/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/cal.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/cal.o libc/libc.a -o $@

$(BIN_DIR)/watch.elf: apps/watch/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/watch.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/watch.o libc/libc.a -o $@

$(BIN_DIR)/screenshot.elf: apps/screenshot/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/screenshot.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/screenshot.o libc/libc.a -o $@

$(BIN_DIR)/paint.elf: apps/paint/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/paint.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/paint.o libc/libc.a -o $@

# ── Clean ─────────────────────────────────────────────────────────────────────

clean:
	$(MAKE) -C libc clean
	rm -rf $(BUILD_DIR)
