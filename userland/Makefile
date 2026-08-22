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
    $(SBIN_DIR)/netstat.elf \
    $(SBIN_DIR)/dhcpcd.elf

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
    $(BIN_DIR)/split.elf

.PHONY: all clean libc apps skeleton etc icons toolchain

all: skeleton libc apps toolchain etc icons

skeleton:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR) $(SBIN_DIR) $(OBJ_DIR) $(BUILD_DIR)/etc $(BUILD_DIR)/dev $(BUILD_DIR)/proc $(BUILD_DIR)/sys $(BUILD_DIR)/root $(BUILD_DIR)/home/azami/Desktop $(BUILD_DIR)/desktop $(BUILD_DIR)/usr/bin $(BUILD_DIR)/usr/sbin $(BUILD_DIR)/usr/lib $(BUILD_DIR)/usr/share/icons $(BUILD_DIR)/usr/share/wallpapers $(BUILD_DIR)/lib $(BUILD_DIR)/lib64 $(BUILD_DIR)/var $(BUILD_DIR)/tmp

libc: skeleton libc/libc.a

libc/libc.a libc/crt0.o:
	$(MAKE) -C libc

toolchain: skeleton libc
	@mkdir -p $(BUILD_DIR)/lib64 $(BUILD_DIR)/lib/x86_64-linux-gnu $(BUILD_DIR)/lib $(BUILD_DIR)/usr/lib $(BUILD_DIR)/usr/bin $(BUILD_DIR)/bin
	@mkdir -p $(BUILD_DIR)/usr/libexec/gcc/x86_64-elf/14.2.0
	@mkdir -p $(BUILD_DIR)/usr/lib/ldscripts $(BUILD_DIR)/lib/ldscripts $(BUILD_DIR)/usr/lib/bfd-plugins
	@mkdir -p $(BUILD_DIR)/usr/include $(BUILD_DIR)/usr/local/include
	@for tool in gcc g++ c++ cpp as ld ar ranlib nm objdump objcopy strip strings readelf size addr2line c++filt gcov gcov-dump gcov-tool elfedit; do \
		src="/home/a0wsvg/opt/cross-x86_64/bin/x86_64-elf-$$tool"; \
		if [ -f "$$src" ]; then \
			strip -s "$$src" -o $(BUILD_DIR)/usr/bin/$$tool 2>/dev/null || cp "$$src" $(BUILD_DIR)/usr/bin/$$tool; \
			ln -sf /usr/bin/$$tool $(BUILD_DIR)/bin/$$tool 2>/dev/null || cp $(BUILD_DIR)/usr/bin/$$tool $(BUILD_DIR)/bin/$$tool; \
		fi; \
	done
	@ln -sf /usr/bin/gcc $(BUILD_DIR)/bin/cc 2>/dev/null || cp $(BUILD_DIR)/usr/bin/gcc $(BUILD_DIR)/bin/cc
	@ln -sf /usr/bin/gcc $(BUILD_DIR)/usr/bin/cc 2>/dev/null || true
	@ln -sf /usr/bin/ld $(BUILD_DIR)/bin/ld.bfd 2>/dev/null || true
	@ln -sf /usr/bin/ld $(BUILD_DIR)/usr/bin/ld.bfd 2>/dev/null || true
	@for pass in cc1 collect2 lto-wrapper liblto_plugin.so; do \
		src="/home/a0wsvg/opt/cross-x86_64/libexec/gcc/x86_64-elf/14.2.0/$$pass"; \
		if [ -f "$$src" ]; then \
			strip -s "$$src" -o $(BUILD_DIR)/usr/libexec/gcc/x86_64-elf/14.2.0/$$pass 2>/dev/null || cp "$$src" $(BUILD_DIR)/usr/libexec/gcc/x86_64-elf/14.2.0/$$pass; \
		fi; \
	done
	@ln -sf /usr/libexec/gcc/x86_64-elf/14.2.0/liblto_plugin.so $(BUILD_DIR)/usr/lib/bfd-plugins/liblto_plugin.so 2>/dev/null || true
	@ln -sf /usr/libexec/gcc/x86_64-elf/14.2.0/liblto_plugin.so $(BUILD_DIR)/usr/lib/liblto_plugin.so 2>/dev/null || true
	@ln -sf /usr/libexec/gcc/x86_64-elf/14.2.0/liblto_plugin.so $(BUILD_DIR)/lib/liblto_plugin.so 2>/dev/null || true
	@ln -sf /usr/libexec/gcc/x86_64-elf/14.2.0/lto-wrapper $(BUILD_DIR)/usr/bin/lto-wrapper 2>/dev/null || true
	@cp -r /home/a0wsvg/opt/cross-x86_64/x86_64-elf/lib/ldscripts/* $(BUILD_DIR)/usr/lib/ldscripts/ 2>/dev/null || true
	@ln -sf /usr/lib/ldscripts $(BUILD_DIR)/lib/ldscripts 2>/dev/null || true
	@cp -r /home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/include/* $(BUILD_DIR)/usr/include/ 2>/dev/null || true
	@cp -r /home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/include-fixed/* $(BUILD_DIR)/usr/include/ 2>/dev/null || true
	@cp -r /home/a0wsvg/opt/cross-x86_64/x86_64-elf/include/* $(BUILD_DIR)/usr/include/ 2>/dev/null || true
	@cp -r libc/include/* $(BUILD_DIR)/usr/include/ 2>/dev/null || true
	@cp -r libc/include/* $(BUILD_DIR)/usr/local/include/ 2>/dev/null || true
	@cp /home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/crtbegin*.o $(BUILD_DIR)/usr/lib/ 2>/dev/null || true
	@cp /home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/crtend*.o $(BUILD_DIR)/usr/lib/ 2>/dev/null || true
	@cp /home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/libgcc*.a $(BUILD_DIR)/usr/lib/ 2>/dev/null || true
	@cp libc/crt0.o $(BUILD_DIR)/usr/lib/crt0.o 2>/dev/null || true
	@cp libc/libc.a $(BUILD_DIR)/usr/lib/libc.a 2>/dev/null || true
	@cp libc/libc.a $(BUILD_DIR)/usr/lib/libm.a 2>/dev/null || true
	@cp libc/libc.a $(BUILD_DIR)/usr/lib/libg.a 2>/dev/null || true
	@cp libc/libc.a $(BUILD_DIR)/usr/lib/libnosys.a 2>/dev/null || true
	@mkdir -p $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64
	@ln -sf /usr/bin $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/bin 2>/dev/null || true
	@ln -sf /usr/libexec $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/libexec 2>/dev/null || true
	@ln -sf /usr/lib $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/lib 2>/dev/null || true
	@ln -sf /usr/include $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/include 2>/dev/null || true
	@mkdir -p $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/x86_64-elf
	@ln -sf /usr/bin $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/x86_64-elf/bin 2>/dev/null || true
	@ln -sf /usr/lib $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/x86_64-elf/lib 2>/dev/null || true
	@ln -sf /usr/include $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/x86_64-elf/include 2>/dev/null || true
	@ln -sf /usr/include $(BUILD_DIR)/home/a0wsvg/opt/cross-x86_64/x86_64-elf/sys-include 2>/dev/null || true
	@ln -sf /usr/lib $(BUILD_DIR)/lib 2>/dev/null || true
	@ln -sf /usr/lib $(BUILD_DIR)/lib64 2>/dev/null || true
	@cp /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 $(BUILD_DIR)/lib64/ld-linux-x86-64.so.2 2>/dev/null || true
	@cp /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 $(BUILD_DIR)/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 2>/dev/null || true
	@cp /lib/x86_64-linux-gnu/libc.so.6 $(BUILD_DIR)/lib/x86_64-linux-gnu/libc.so.6 2>/dev/null || true
	@cp /lib/x86_64-linux-gnu/libm.so.6 $(BUILD_DIR)/lib/x86_64-linux-gnu/libm.so.6 2>/dev/null || true
	@cp /lib/x86_64-linux-gnu/libc.so.6 $(BUILD_DIR)/lib64/libc.so.6 2>/dev/null || true
	@cp /lib/x86_64-linux-gnu/libm.so.6 $(BUILD_DIR)/lib64/libm.so.6 2>/dev/null || true
	@cp /lib/x86_64-linux-gnu/libc.so.6 $(BUILD_DIR)/lib/libc.so.6 2>/dev/null || true
	@cp /lib/x86_64-linux-gnu/libm.so.6 $(BUILD_DIR)/lib/libm.so.6 2>/dev/null || true

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
	@echo "Europe/Warsaw" > $(BUILD_DIR)/etc/timezone
	@printf "127.0.0.1   localhost azamios\n::1         localhost ip6-localhost ip6-loopback\n" > $(BUILD_DIR)/etc/hosts
	@printf "nameserver 10.0.2.3\nnameserver 1.1.1.1\nnameserver 8.8.8.8\n" > $(BUILD_DIR)/etc/resolv.conf
	@printf "[theme]\ntheme_id=0\nname=Catppuccin Mocha\nwallpaper=/usr/share/wallpapers/default.raw\n\n[display]\nvsync=1\ncompositing=1\ncursor_aa=1\nfps=60\n\n[panel]\nposition=bottom\nheight=32\nautohide=0\nshow_clock=1\n" > $(BUILD_DIR)/etc/desktop.conf
	@printf "[terminal]\ntheme=catppuccin\nfont_size=12\nopacity=95\nscrollback=500\ncursor_blink=1\nhistory_size=100\nprompt=root@azamios:\w$$ \n" > $(BUILD_DIR)/etc/terminal.conf
	@printf "[audio]\nvolume=80\nmaster_mute=0\nsample_rate=44100\nchannels=2\ndefault_sink=/dev/dsp\n" > $(BUILD_DIR)/etc/audio.conf
	@printf "[network]\ninterface=eth0\ndhcp=1\nip=10.0.2.15\nnetmask=255.255.255.0\ngateway=10.0.2.2\ndns=10.0.2.3\n" > $(BUILD_DIR)/etc/network.conf
	@printf "export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/home/a0wsvg/opt/cross-x86_64/bin:/home/a0wsvg/opt/cross-x86_64/x86_64-elf/bin:/\nexport USER=root\nexport HOME=/root\nexport TERM=azami\nexport SHELL=/bin/sh.elf\nexport TMPDIR=/tmp\nexport COMPILER_PATH=/home/a0wsvg/opt/cross-x86_64/libexec/gcc/x86_64-elf/14.2.0/:/home/a0wsvg/opt/cross-x86_64/x86_64-elf/bin/:/usr/bin:/bin\nexport LIBRARY_PATH=/home/a0wsvg/opt/cross-x86_64/x86_64-elf/lib/:/home/a0wsvg/opt/cross-x86_64/lib/gcc/x86_64-elf/14.2.0/:/usr/lib:/lib\nexport C_INCLUDE_PATH=/usr/include:/usr/local/include:/home/a0wsvg/opt/cross-x86_64/x86_64-elf/include:/home/a0wsvg/opt/cross-x86_64/x86_64-elf/sys-include\nexport CPATH=/usr/include:/usr/local/include:/home/a0wsvg/opt/cross-x86_64/x86_64-elf/include\nexport PS1=\"root@azamios:\w$$ \"\n" > $(BUILD_DIR)/etc/profile
	@cp $(BUILD_DIR)/etc/profile $(BUILD_DIR)/root/.profile
	@cp $(BUILD_DIR)/etc/profile $(BUILD_DIR)/home/azami/.profile
	@mkdir -p $(BUILD_DIR)/tmp $(BUILD_DIR)/var/tmp $(BUILD_DIR)/var/log $(BUILD_DIR)/var/run
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
cd "$LIBC_DIR"\n\
echo "=== Building AzamiOS libc with native GCC 14.2.0 ==="\n\
CFLAGS="-std=c11 -ffreestanding -fno-stack-protector -fno-pie -fno-pic -m64 -O2 -I. -Iinclude"\n\
SRCS="syscall.c stdio.c stdlib.c string.c ctype.c dirent.c time.c math.c az_ipc.c signal.c mman.c pthread.c termios.c resource.c getopt.c acl.c x11.c socket.c netdb.c pwd.c grp.c regex.c fnmatch.c glob.c spawn.c syslog.c search.c"\n\
echo "  Compiling C sources..."\n\
gcc $CFLAGS -c $SRCS\n\
echo "  Assembling crt0.asm..."\n\
nasm -f elf64 crt0.asm -o crt0.o\n\
echo "  Archiving libc.a..."\n\
ar rcs libc.a syscall.o stdio.o stdlib.o string.o ctype.o dirent.o time.o math.o az_ipc.o signal.o mman.o pthread.o termios.o resource.o getopt.o acl.o x11.o socket.o netdb.o pwd.o grp.o regex.o fnmatch.o glob.o spawn.o syslog.o search.o\n\
echo "  Installing to /usr/lib /lib /lib64 ..."\n\
cp libc.a  /usr/lib/libc.a  2>/dev/null || true\n\
cp libc.a  /usr/lib/libm.a  2>/dev/null || true\n\
cp libc.a  /lib/libc.a      2>/dev/null || true\n\
cp libc.a  /lib/libm.a      2>/dev/null || true\n\
cp libc.a  /lib64/libc.a    2>/dev/null || true\n\
cp crt0.o  /usr/lib/crt0.o  2>/dev/null || true\n\
cp crt0.o  /lib/crt0.o      2>/dev/null || true\n\
cp crt0.o  /lib64/crt0.o    2>/dev/null || true\n\
GCCLIB=/home/a0wsvg/opt/cross-x86_64/x86_64-elf/lib\n\
cp libc.a  $GCCLIB/libc.a   2>/dev/null || true\n\
cp crt0.o  $GCCLIB/crt0.o   2>/dev/null || true\n\
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

$(BIN_DIR)/find.elf: apps/find/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/find.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/find.o libc/libc.a -o $@

$(BIN_DIR)/whoami.elf: apps/whoami/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/whoami.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/whoami.o libc/libc.a -o $@

$(BIN_DIR)/du.elf: apps/du/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/du.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/du.o libc/libc.a -o $@

$(BIN_DIR)/time.elf: apps/time/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/time.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/time.o libc/libc.a -o $@

$(BIN_DIR)/tar.elf: apps/tar/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/tar.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/tar.o libc/libc.a -o $@

$(BIN_DIR)/strings.elf: apps/strings/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/strings.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/strings.o libc/libc.a -o $@

$(BIN_DIR)/xargs.elf: apps/xargs/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/xargs.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/xargs.o libc/libc.a -o $@

$(BIN_DIR)/seq.elf: apps/seq/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/seq.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/seq.o libc/libc.a -o $@

$(BIN_DIR)/stat.elf: apps/stat/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/stat.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/stat.o libc/libc.a -o $@

$(BIN_DIR)/basename.elf: apps/basename/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/basename.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/basename.o libc/libc.a -o $@

$(BIN_DIR)/dirname.elf: apps/dirname/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/dirname.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/dirname.o libc/libc.a -o $@

$(BIN_DIR)/yes.elf: apps/yes/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/yes.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/yes.o libc/libc.a -o $@

$(BIN_DIR)/true.elf: apps/true/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/true.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/true.o libc/libc.a -o $@

$(BIN_DIR)/false.elf: apps/false/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/false.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/false.o libc/libc.a -o $@

$(BIN_DIR)/audioplayer.elf: apps/audioplayer/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/audioplayer.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/audioplayer.o libc/libc.a -o $@

$(BIN_DIR)/minesweeper.elf: apps/minesweeper/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/minesweeper.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/minesweeper.o libc/libc.a -o $@

$(BIN_DIR)/2048.elf: apps/2048/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/2048.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/2048.o libc/libc.a -o $@

$(BIN_DIR)/snake.elf: apps/snake/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/snake.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/snake.o libc/libc.a -o $@

$(BIN_DIR)/getfacl.elf: apps/getfacl/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/getfacl.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/getfacl.o libc/libc.a -o $@

$(BIN_DIR)/setfacl.elf: apps/setfacl/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/setfacl.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/setfacl.o libc/libc.a -o $@

$(BIN_DIR)/xorg.elf: apps/xorg/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/xorg.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/xorg.o libc/libc.a -o $@

$(BIN_DIR)/xwm.elf: apps/xwm/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/xwm.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/xwm.o libc/libc.a -o $@

$(BIN_DIR)/xclock.elf: apps/xclock/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/xclock.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/xclock.o libc/libc.a -o $@

$(BIN_DIR)/xcalc.elf: apps/xcalc/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/xcalc.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/xcalc.o libc/libc.a -o $@

$(BIN_DIR)/xeyes.elf: apps/xeyes/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/xeyes.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/xeyes.o libc/libc.a -o $@

$(BIN_DIR)/xgui_demo.elf: apps/xgui_demo/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/xgui_demo.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/xgui_demo.o libc/libc.a -o $@

$(BIN_DIR)/nc.elf: apps/nc/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/nc.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/nc.o libc/libc.a -o $@

$(BIN_DIR)/curl.elf: apps/curl/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/curl.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/curl.o libc/libc.a -o $@

$(BIN_DIR)/httpd.elf: apps/httpd/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/httpd.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/httpd.o libc/libc.a -o $@

$(BIN_DIR)/config.elf: apps/config/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/config.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/config.o libc/libc.a -o $@

$(SBIN_DIR)/dhcpcd.elf: apps/dhcpcd/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/dhcpcd.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/dhcpcd.o libc/libc.a -o $@

$(BIN_DIR)/test.elf: apps/test/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/test.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/test.o libc/libc.a -o $@

$(BIN_DIR)/id.elf: apps/id/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/id.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/id.o libc/libc.a -o $@

$(BIN_DIR)/tr.elf: apps/tr/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/tr.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/tr.o libc/libc.a -o $@

$(BIN_DIR)/sed.elf: apps/sed/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/sed.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/sed.o libc/libc.a -o $@

$(BIN_DIR)/ln.elf: apps/ln/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/ln.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/ln.o libc/libc.a -o $@

$(BIN_DIR)/tee.elf: apps/tee/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/tee.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/tee.o libc/libc.a -o $@

$(BIN_DIR)/paste.elf: apps/paste/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/paste.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/paste.o libc/libc.a -o $@

$(BIN_DIR)/comm.elf: apps/comm/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/comm.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/comm.o libc/libc.a -o $@

$(BIN_DIR)/cmp.elf: apps/cmp/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/cmp.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/cmp.o libc/libc.a -o $@

$(BIN_DIR)/fold.elf: apps/fold/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/fold.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/fold.o libc/libc.a -o $@

$(BIN_DIR)/expand.elf: apps/expand/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/expand.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/expand.o libc/libc.a -o $@

$(BIN_DIR)/unexpand.elf: apps/unexpand/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/unexpand.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/unexpand.o libc/libc.a -o $@

$(BIN_DIR)/expr.elf: apps/expr/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/expr.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/expr.o libc/libc.a -o $@

$(BIN_DIR)/printenv.elf: apps/printenv/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/printenv.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/printenv.o libc/libc.a -o $@

$(BIN_DIR)/split.elf: apps/split/main.c libc/libc.a libc/crt0.o | skeleton
	$(CC) $(CFLAGS) -c $< -o $(OBJ_DIR)/split.o
	$(LD) $(LDFLAGS) libc/crt0.o $(OBJ_DIR)/split.o libc/libc.a -o $@

# ── Clean ─────────────────────────────────────────────────────────────────────

clean:
	$(MAKE) -C libc clean
	rm -rf $(BUILD_DIR)
