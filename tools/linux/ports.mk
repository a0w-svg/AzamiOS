# ==============================================================================
# AzamiOS — Ported third-party software
# File: tools/linux/ports.mk   (included by tools/linux/Makefile)
#
# Real, unmodified upstream releases built with the musl toolchain from the
# parent Makefile and linked static, exactly like BusyBox: nothing here is
# patched for AzamiOS, and each one runs because the kernel implements the
# Linux x86_64 syscall ABI (docs/LINUX-BINARIES.md). The results land in
# out/bin (and out/<port>/ for a port that ships more than a binary), from
# where scripts/generate_pkg_repo.py turns each into an installable package.
#
#   make -C tools/linux ports          — build every port
#   make -C tools/linux port-lua       — build one
#   make -C tools/linux ports-fetch    — download the tarballs only
#   make -C tools/linux ports-clean    — drop built ports, keep the tarballs
#
# Ports are deliberately NOT part of `make -C tools/linux all`, and are never
# staged into the image by `install`: they are what the package repository is
# *for*. The image stays as it was, and you install what you want at runtime.
#
# Each port follows the same shape — fetch a tarball to dl/, unpack into
# src/, build there with PORT_CC, copy the results into out/. A port whose
# build fails leaves its log in log/<name>.log and, because the repository
# generator only packages what exists, simply does not appear in the
# catalogue; it never breaks the rest of the build.
# ==============================================================================

# ── Versions ─────────────────────────────────────────────────────────────────
TOYBOX_VER    := 0.8.11
LUA_VER       := 5.4.7
SQLITE_VER    := 3460100
SQLITE_YEAR   := 2024
DASH_VER      := 0.5.12
BZIP2_VER     := 1.0.8
XZ_VER        := 5.4.7
ZSTD_VER      := 1.5.6
MAKE_VER      := 4.4.1
CURL_VER      := 8.9.1
JQ_VER        := 1.7.1
# TinyCC has not cut a release since 0.9.27 (2017), and 0.9.27 cannot link
# against a libc that uses thread-local storage: it aborts with "Unknown
# relocation type: 23" (R_X86_64_TPOFF32) the moment AzamiOS's libc.a is on
# the command line, because this libc's errno is __thread. Upstream has
# handled the TLS relocations for years. So the port pins a commit of the
# development branch rather than the stale release — the version string
# below is that branch's own VERSION file plus the commit date.
TCC_VER       := 0.9.28rc-20260904
TCC_COMMIT    := 0fb54300b56512754221d80adda85ddb9815bceb
NCURSES_VER   := 6.4
NANO_VER      := 7.2
LESS_VER      := 643
GAWK_VER      := 5.3.0
BASH_VER      := 5.2.21
FILE_VER      := 5.45
TREE_VER      := 2.1.1
MICROPY_VER   := 1.23.0
# ── New GNU ports ─────────────────────────────────────────────────────────────
COREUTILS_VER := 9.5
SED_VER       := 4.9
GREP_VER      := 3.11
DIFFUTILS_VER := 3.10
FINDUTILS_VER := 4.10.0
GTAR_VER      := 1.35
GZIP_VER      := 1.13
UTIL_LINUX_VER := 2.40.2
PROCPS_VER    := 4.0.4
GBC_VER       := 1.07.1
ED_VER        := 1.22.6
WGET_VER      := 1.24.5
SOCAT_VER     := 1.8.0.0
HTOP_VER      := 3.3.0
STRACE_VER    := 6.10
BINUTILS_VER  := 2.43.1
GCC_VER       := 14.1.0
GMP_VER       := 6.3.0
MPFR_VER      := 4.2.1
MPC_VER       := 1.3.1
NASM_VER      := 2.16.03
VIM_VER       := 9.1.0
SCREEN_VER    := 4.9.1
NCDU_VER      := 2.7
RIPGREP_VER   := 14.1.1
FD_VER        := 10.2.0
BAT_VER       := 0.24.0
EXA_VER       := 0.10.1
HEXYL_VER     := 0.14.0

# ── Where each tarball comes from ────────────────────────────────────────────
TOYBOX_URL    := http://landley.net/toybox/downloads/toybox-$(TOYBOX_VER).tar.gz
LUA_URL       := https://www.lua.org/ftp/lua-$(LUA_VER).tar.gz
SQLITE_URL    := https://www.sqlite.org/$(SQLITE_YEAR)/sqlite-autoconf-$(SQLITE_VER).tar.gz
DASH_URL      := http://gondor.apana.org.au/~herbert/dash/files/dash-$(DASH_VER).tar.gz
BZIP2_URL     := https://sourceware.org/pub/bzip2/bzip2-$(BZIP2_VER).tar.gz
XZ_URL        := https://github.com/tukaani-project/xz/releases/download/v$(XZ_VER)/xz-$(XZ_VER).tar.gz
ZSTD_URL      := https://github.com/facebook/zstd/releases/download/v$(ZSTD_VER)/zstd-$(ZSTD_VER).tar.gz
MAKE_URL      := https://ftp.gnu.org/gnu/make/make-$(MAKE_VER).tar.gz
CURL_URL      := https://curl.se/download/curl-$(CURL_VER).tar.gz
JQ_URL        := https://github.com/jqlang/jq/releases/download/jq-$(JQ_VER)/jq-$(JQ_VER).tar.gz
TCC_URL       := https://github.com/TinyCC/tinycc/archive/$(TCC_COMMIT).tar.gz
NCURSES_URL   := https://ftp.gnu.org/gnu/ncurses/ncurses-$(NCURSES_VER).tar.gz
NANO_URL      := https://www.nano-editor.org/dist/v7/nano-$(NANO_VER).tar.xz
LESS_URL      := https://www.greenwoodsoftware.com/less/less-$(LESS_VER).tar.gz
GAWK_URL      := https://ftp.gnu.org/gnu/gawk/gawk-$(GAWK_VER).tar.gz
BASH_URL      := https://ftp.gnu.org/gnu/bash/bash-$(BASH_VER).tar.gz
FILE_URL      := https://astron.com/pub/file/file-$(FILE_VER).tar.gz
TREE_URL      := https://gitlab.com/OldManProgrammer/unix-tree/-/archive/$(TREE_VER)/unix-tree-$(TREE_VER).tar.gz
MICROPY_URL   := https://micropython.org/resources/source/micropython-$(MICROPY_VER).tar.xz
# New GNU ports
COREUTILS_URL := https://ftp.gnu.org/gnu/coreutils/coreutils-$(COREUTILS_VER).tar.xz
SED_URL       := https://ftp.gnu.org/gnu/sed/sed-$(SED_VER).tar.xz
GREP_URL      := https://ftp.gnu.org/gnu/grep/grep-$(GREP_VER).tar.xz
DIFFUTILS_URL := https://ftp.gnu.org/gnu/diffutils/diffutils-$(DIFFUTILS_VER).tar.xz
FINDUTILS_URL := https://ftp.gnu.org/gnu/findutils/findutils-$(FINDUTILS_VER).tar.xz
GTAR_URL      := https://ftp.gnu.org/gnu/tar/tar-$(GTAR_VER).tar.xz
GZIP_URL      := https://ftp.gnu.org/gnu/gzip/gzip-$(GZIP_VER).tar.xz
UTIL_LINUX_URL := https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v$(basename $(UTIL_LINUX_VER))/util-linux-$(UTIL_LINUX_VER).tar.xz
PROCPS_URL    := https://downloads.sourceforge.net/project/procps-ng/Production/procps-ng-$(PROCPS_VER).tar.xz
GBC_URL       := https://ftp.gnu.org/gnu/bc/bc-$(GBC_VER).tar.gz
ED_URL        := https://ftp.gnu.org/gnu/ed/ed-$(ED_VER).tar.lz
WGET_URL      := https://ftp.gnu.org/gnu/wget/wget-$(WGET_VER).tar.gz
SOCAT_URL     := http://www.dest-unreach.org/socat/download/socat-$(SOCAT_VER).tar.gz
HTOP_URL      := https://github.com/htop-dev/htop/releases/download/$(HTOP_VER)/htop-$(HTOP_VER).tar.xz
STRACE_URL    := https://github.com/strace/strace/releases/download/v$(STRACE_VER)/strace-$(STRACE_VER).tar.xz
BINUTILS_URL  := https://ftp.gnu.org/gnu/binutils/binutils-$(BINUTILS_VER).tar.xz
GCC_URL       := https://ftp.gnu.org/gnu/gcc/gcc-$(GCC_VER)/gcc-$(GCC_VER).tar.xz
GMP_URL       := https://ftp.gnu.org/gnu/gmp/gmp-$(GMP_VER).tar.xz
MPFR_URL      := https://ftp.gnu.org/gnu/mpfr/mpfr-$(MPFR_VER).tar.xz
MPC_URL       := https://ftp.gnu.org/gnu/mpc/mpc-$(MPC_VER).tar.gz
NASM_URL      := https://www.nasm.us/pub/nasm/releasebuilds/$(NASM_VER)/nasm-$(NASM_VER).tar.xz
VIM_URL       := https://github.com/vim/vim/archive/refs/tags/v$(VIM_VER).tar.gz
SCREEN_URL    := https://ftp.gnu.org/gnu/screen/screen-$(SCREEN_VER).tar.gz
NCDU_URL      := https://code.blicky.net/yorhel/ncdu/archive/v$(NCDU_VER).tar.gz

# ── Build settings shared by every port ──────────────────────────────────────
#
# Static, like BusyBox: a dynamic binary would need ld-linux and a shared libc
# on the image, which is a different thing to test. -O2 and nothing clever —
# the point is that unmodified upstream code compiled the ordinary way runs.
PORT_CC      := $(CCACHE_PREFIX) $(MUSL_GCC)
# Same escape hatch BusyBox needs: a few toybox/util commands include Linux
# UAPI headers (linux/fs.h, linux/if_tun.h, ...) that musl deliberately does
# not ship. The host's copies go *after* musl's include path so musl's own
# headers still win every conflict.
PORT_CC_UAPI := $(CCACHE_PREFIX) $(MUSL_GCC) $(UAPI_INC) $(UAPI_ASM)
PORT_CFLAGS  := -O2
PORT_LDFLAGS := -static
PORT_DIR     := $(OUT_DIR)
PORT_BIN     := $(OUT_DIR)/bin

# Autotools packages all take the same handful of arguments. --host names a
# musl triplet so configure treats this as a cross build and skips the
# run-tests it cannot run; --disable-nls keeps gettext out of a static link.
# CC_FOR_BUILD matters: several of these packages compile a helper program
# and then *run* it during their own build (dash's mksyntax, bash's
# mkbuiltins). Autoconf otherwise sets CC_FOR_BUILD to whatever CC is, and a
# musl-gcc-linked helper cannot run on a host that has no musl loader
# installed — the build dies with "required file not found" from the shell,
# not from the compiler.
PORT_CONFIGURE_ENV := CC="$(PORT_CC)" CC_FOR_BUILD=gcc BUILD_CC=gcc \
                      CFLAGS="$(PORT_CFLAGS)" LDFLAGS="$(PORT_LDFLAGS)"
PORT_CONFIGURE_ARGS := --host=x86_64-linux-musl --disable-shared --enable-static --disable-nls

# Every port's binaries, as the repository generator expects to find them.
PORT_BINS := \
    $(PORT_BIN)/toybox \
    $(PORT_BIN)/lua $(PORT_BIN)/luac \
    $(PORT_BIN)/sqlite3 \
    $(PORT_BIN)/dash \
    $(PORT_BIN)/bzip2 \
    $(PORT_BIN)/xz \
    $(PORT_BIN)/zstd \
    $(PORT_BIN)/make \
    $(PORT_BIN)/curl \
    $(PORT_BIN)/jq \
    $(PORT_BIN)/tcc \
    $(PORT_BIN)/nano \
    $(PORT_BIN)/less \
    $(PORT_BIN)/gawk \
    $(PORT_BIN)/bash \
    $(PORT_BIN)/file \
    $(PORT_BIN)/tree \
    $(PORT_BIN)/micropython \
    $(PORT_BIN)/coreutils \
    $(PORT_BIN)/sed \
    $(PORT_BIN)/grep \
    $(PORT_BIN)/diff \
    $(PORT_BIN)/find \
    $(PORT_BIN)/gtar \
    $(PORT_BIN)/gzip \
    $(PORT_BIN)/column \
    $(PORT_BIN)/ps \
    $(PORT_BIN)/bc \
    $(PORT_BIN)/dc \
    $(PORT_BIN)/ed \
    $(PORT_BIN)/wget \
    $(PORT_BIN)/socat \
    $(PORT_BIN)/htop \
    $(PORT_BIN)/strace-gnu \
    $(PORT_BIN)/objdump \
    $(PORT_BIN)/screen \
    $(PORT_BIN)/gcc \
    $(PORT_BIN)/nasm \
    $(PORT_BIN)/vim \
    $(PORT_BIN)/ncdu \
    $(PORT_BIN)/rg \
    $(PORT_BIN)/fd \
    $(PORT_BIN)/bat \
    $(PORT_BIN)/exa \
    $(PORT_BIN)/hexyl

.PHONY: ports ports-fetch ports-clean
ports:
	@$(MAKE) --no-print-directory --keep-going $(PORT_BINS) || true
	@echo ""
	@echo "  ── ports built ────────────────────────────────────────────────"
	@for b in $(PORT_BINS); do \
	    if [ -x $$b ]; then printf "  ✓  %-14s %8s bytes\n" $$(basename $$b) $$(wc -c < $$b); \
	    else printf "  ✗  %-14s (see $(LOG_DIR)/%s.log)\n" $$(basename $$b) $$(basename $$b); fi; \
	 done

ports-clean:
	rm -rf $(PORT_BIN) $(OUT_DIR)/tcc $(OUT_DIR)/file $(OUT_DIR)/terminfo \
	    $(OUT_DIR)/coreutils $(OUT_DIR)/util-linux $(OUT_DIR)/binutils \
	    $(OUT_DIR)/vim $(OUT_DIR)/screen

# ── Fetch + unpack helpers ───────────────────────────────────────────────────
# One pair of rules per archive format, so a port's own rule only has to
# depend on the unpacked directory's marker file.
$(DL_DIR)/%.tar.gz:
	@mkdir -p $(DL_DIR)
	@echo "  ↓  Fetching $*"
	@curl -fsSL -o $@.part $(URL_$*) && mv $@.part $@

$(DL_DIR)/%.tar.bz2:
	@mkdir -p $(DL_DIR)
	@echo "  ↓  Fetching $*"
	@curl -fsSL -o $@.part $(URL_$*) && mv $@.part $@

$(DL_DIR)/%.tar.xz:
	@mkdir -p $(DL_DIR)
	@echo "  ↓  Fetching $*"
	@curl -fsSL -o $@.part $(URL_$*) && mv $@.part $@

$(SRC_DIR)/%/.unpacked:
	@mkdir -p $(SRC_DIR)/$*
	@a=$(DL_DIR)/$*.tar.gz;  [ -f $$a ] || a=$(DL_DIR)/$*.tar.bz2; \
	 [ -f $$a ] || a=$(DL_DIR)/$*.tar.xz; \
	 [ -f $$a ] || a=$(DL_DIR)/$*.tar.lz; \
	 [ -f $$a ] || a=$(DL_DIR)/$*.zip; \
	 echo "  ▸  Unpacking $$(basename $$a)"; \
	 case $$a in \
	   *.tar.lz) tar --use-compress-program=lzip -xf $$a -C $(SRC_DIR) ;; \
	   *.zip)    python3 -c "import zipfile; zipfile.ZipFile('$$a', 'r').extractall('$(SRC_DIR)/$*')" ;; \
	   *)         tar -xf $$a -C $(SRC_DIR) ;; \
	 esac
	@touch $@

# URL_<basename> lookups used by the download rules above.
URL_toybox-$(TOYBOX_VER)   := $(TOYBOX_URL)
URL_lua-$(LUA_VER)         := $(LUA_URL)
URL_sqlite-autoconf-$(SQLITE_VER) := $(SQLITE_URL)
URL_dash-$(DASH_VER)       := $(DASH_URL)
URL_bzip2-$(BZIP2_VER)     := $(BZIP2_URL)
URL_xz-$(XZ_VER)           := $(XZ_URL)
URL_zstd-$(ZSTD_VER)       := $(ZSTD_URL)
URL_make-$(MAKE_VER)       := $(MAKE_URL)
URL_curl-$(CURL_VER)       := $(CURL_URL)
URL_jq-$(JQ_VER)           := $(JQ_URL)
URL_tinycc-$(TCC_COMMIT)   := $(TCC_URL)
URL_ncurses-$(NCURSES_VER) := $(NCURSES_URL)
URL_nano-$(NANO_VER)       := $(NANO_URL)
URL_less-$(LESS_VER)       := $(LESS_URL)
URL_gawk-$(GAWK_VER)       := $(GAWK_URL)
URL_bash-$(BASH_VER)       := $(BASH_URL)
URL_file-$(FILE_VER)       := $(FILE_URL)
URL_unix-tree-$(TREE_VER)  := $(TREE_URL)
URL_micropython-$(MICROPY_VER) := $(MICROPY_URL)

$(DL_DIR)/%.zip:
	@mkdir -p $(DL_DIR)
	@echo "  ↓  Fetching $*"
	@curl -fsSL -o $@.part $(URL_$*) && mv $@.part $@

$(DL_DIR)/%.tar.lz:
	@mkdir -p $(DL_DIR)
	@echo "  ↓  Fetching $*"
	@curl -fsSL -o $@.part $(URL_$*) && mv $@.part $@

ports-fetch: $(DL_DIR)/toybox-$(TOYBOX_VER).tar.gz $(DL_DIR)/lua-$(LUA_VER).tar.gz \
             $(DL_DIR)/sqlite-autoconf-$(SQLITE_VER).tar.gz $(DL_DIR)/dash-$(DASH_VER).tar.gz \
             $(DL_DIR)/bzip2-$(BZIP2_VER).tar.gz $(DL_DIR)/xz-$(XZ_VER).tar.gz \
             $(DL_DIR)/zstd-$(ZSTD_VER).tar.gz $(DL_DIR)/make-$(MAKE_VER).tar.gz \
             $(DL_DIR)/curl-$(CURL_VER).tar.gz $(DL_DIR)/jq-$(JQ_VER).tar.gz \
             $(DL_DIR)/tinycc-$(TCC_COMMIT).tar.gz $(DL_DIR)/ncurses-$(NCURSES_VER).tar.gz \
             $(DL_DIR)/nano-$(NANO_VER).tar.xz $(DL_DIR)/less-$(LESS_VER).tar.gz \
             $(DL_DIR)/gawk-$(GAWK_VER).tar.gz $(DL_DIR)/bash-$(BASH_VER).tar.gz \
             $(DL_DIR)/file-$(FILE_VER).tar.gz $(DL_DIR)/unix-tree-$(TREE_VER).tar.gz \
             $(DL_DIR)/micropython-$(MICROPY_VER).tar.xz \
             $(DL_DIR)/coreutils-$(COREUTILS_VER).tar.xz \
             $(DL_DIR)/sed-$(SED_VER).tar.xz \
             $(DL_DIR)/grep-$(GREP_VER).tar.xz \
             $(DL_DIR)/diffutils-$(DIFFUTILS_VER).tar.xz \
             $(DL_DIR)/findutils-$(FINDUTILS_VER).tar.xz \
             $(DL_DIR)/tar-$(GTAR_VER).tar.xz \
             $(DL_DIR)/gzip-$(GZIP_VER).tar.xz \
             $(DL_DIR)/util-linux-$(UTIL_LINUX_VER).tar.xz \
             $(DL_DIR)/procps-ng-$(PROCPS_VER).tar.xz \
             $(DL_DIR)/bc-$(GBC_VER).tar.gz \
             $(DL_DIR)/ed-$(ED_VER).tar.lz \
             $(DL_DIR)/wget-$(WGET_VER).tar.gz \
             $(DL_DIR)/socat-$(SOCAT_VER).tar.gz \
             $(DL_DIR)/htop-$(HTOP_VER).tar.xz \
             $(DL_DIR)/strace-$(STRACE_VER).tar.xz \
             $(DL_DIR)/binutils-$(BINUTILS_VER).tar.xz \
             $(DL_DIR)/gcc-$(GCC_VER).tar.xz \
             $(DL_DIR)/gmp-$(GMP_VER).tar.xz \
             $(DL_DIR)/mpfr-$(MPFR_VER).tar.xz \
             $(DL_DIR)/mpc-$(MPC_VER).tar.gz \
             $(DL_DIR)/nasm-$(NASM_VER).tar.xz \
             $(DL_DIR)/vim-$(VIM_VER).tar.gz \
             $(DL_DIR)/screen-$(SCREEN_VER).tar.gz \
             $(DL_DIR)/ncdu-$(NCDU_VER)-linux-x86_64.tar.gz \
             $(DL_DIR)/ripgrep-$(RIPGREP_VER)-x86_64-unknown-linux-musl.tar.gz \
             $(DL_DIR)/fd-v$(FD_VER)-x86_64-unknown-linux-musl.tar.gz \
             $(DL_DIR)/bat-v$(BAT_VER)-x86_64-unknown-linux-musl.tar.gz \
             $(DL_DIR)/exa-linux-x86_64-musl-v$(EXA_VER).zip \
             $(DL_DIR)/hexyl-v$(HEXYL_VER)-x86_64-unknown-linux-musl.tar.gz

# Which tarball each unpack depends on (the recipe comes from the pattern
# rule above; these lines only say what has to be downloaded first).
$(SRC_DIR)/toybox-$(TOYBOX_VER)/.unpacked:              $(DL_DIR)/toybox-$(TOYBOX_VER).tar.gz
$(SRC_DIR)/lua-$(LUA_VER)/.unpacked:                    $(DL_DIR)/lua-$(LUA_VER).tar.gz
$(SRC_DIR)/sqlite-autoconf-$(SQLITE_VER)/.unpacked:     $(DL_DIR)/sqlite-autoconf-$(SQLITE_VER).tar.gz
$(SRC_DIR)/dash-$(DASH_VER)/.unpacked:                  $(DL_DIR)/dash-$(DASH_VER).tar.gz
$(SRC_DIR)/bzip2-$(BZIP2_VER)/.unpacked:                $(DL_DIR)/bzip2-$(BZIP2_VER).tar.gz
$(SRC_DIR)/xz-$(XZ_VER)/.unpacked:                      $(DL_DIR)/xz-$(XZ_VER).tar.gz
$(SRC_DIR)/zstd-$(ZSTD_VER)/.unpacked:                  $(DL_DIR)/zstd-$(ZSTD_VER).tar.gz
$(SRC_DIR)/make-$(MAKE_VER)/.unpacked:                  $(DL_DIR)/make-$(MAKE_VER).tar.gz
$(SRC_DIR)/curl-$(CURL_VER)/.unpacked:                  $(DL_DIR)/curl-$(CURL_VER).tar.gz
$(SRC_DIR)/jq-$(JQ_VER)/.unpacked:                      $(DL_DIR)/jq-$(JQ_VER).tar.gz
$(SRC_DIR)/tinycc-$(TCC_COMMIT)/.unpacked:              $(DL_DIR)/tinycc-$(TCC_COMMIT).tar.gz
$(SRC_DIR)/ncurses-$(NCURSES_VER)/.unpacked:            $(DL_DIR)/ncurses-$(NCURSES_VER).tar.gz
$(SRC_DIR)/nano-$(NANO_VER)/.unpacked:                  $(DL_DIR)/nano-$(NANO_VER).tar.xz
$(SRC_DIR)/less-$(LESS_VER)/.unpacked:                  $(DL_DIR)/less-$(LESS_VER).tar.gz
$(SRC_DIR)/gawk-$(GAWK_VER)/.unpacked:                  $(DL_DIR)/gawk-$(GAWK_VER).tar.gz
$(SRC_DIR)/bash-$(BASH_VER)/.unpacked:                  $(DL_DIR)/bash-$(BASH_VER).tar.gz
$(SRC_DIR)/file-$(FILE_VER)/.unpacked:                  $(DL_DIR)/file-$(FILE_VER).tar.gz
$(SRC_DIR)/unix-tree-$(TREE_VER)/.unpacked:             $(DL_DIR)/unix-tree-$(TREE_VER).tar.gz
$(SRC_DIR)/micropython-$(MICROPY_VER)/.unpacked:        $(DL_DIR)/micropython-$(MICROPY_VER).tar.xz
$(SRC_DIR)/coreutils-$(COREUTILS_VER)/.unpacked:        $(DL_DIR)/coreutils-$(COREUTILS_VER).tar.xz
$(SRC_DIR)/sed-$(SED_VER)/.unpacked:                    $(DL_DIR)/sed-$(SED_VER).tar.xz
$(SRC_DIR)/grep-$(GREP_VER)/.unpacked:                  $(DL_DIR)/grep-$(GREP_VER).tar.xz
$(SRC_DIR)/diffutils-$(DIFFUTILS_VER)/.unpacked:        $(DL_DIR)/diffutils-$(DIFFUTILS_VER).tar.xz
$(SRC_DIR)/findutils-$(FINDUTILS_VER)/.unpacked:        $(DL_DIR)/findutils-$(FINDUTILS_VER).tar.xz
$(SRC_DIR)/tar-$(GTAR_VER)/.unpacked:                   $(DL_DIR)/tar-$(GTAR_VER).tar.xz
$(SRC_DIR)/gzip-$(GZIP_VER)/.unpacked:                  $(DL_DIR)/gzip-$(GZIP_VER).tar.xz
$(SRC_DIR)/util-linux-$(UTIL_LINUX_VER)/.unpacked:      $(DL_DIR)/util-linux-$(UTIL_LINUX_VER).tar.xz
$(SRC_DIR)/procps-ng-$(PROCPS_VER)/.unpacked:           $(DL_DIR)/procps-ng-$(PROCPS_VER).tar.xz
$(SRC_DIR)/bc-$(GBC_VER)/.unpacked:                     $(DL_DIR)/bc-$(GBC_VER).tar.gz
$(SRC_DIR)/ed-$(ED_VER)/.unpacked:                      $(DL_DIR)/ed-$(ED_VER).tar.lz
$(SRC_DIR)/wget-$(WGET_VER)/.unpacked:                  $(DL_DIR)/wget-$(WGET_VER).tar.gz
$(SRC_DIR)/socat-$(SOCAT_VER)/.unpacked:                $(DL_DIR)/socat-$(SOCAT_VER).tar.gz
$(SRC_DIR)/htop-$(HTOP_VER)/.unpacked:                  $(DL_DIR)/htop-$(HTOP_VER).tar.xz
$(SRC_DIR)/strace-$(STRACE_VER)/.unpacked:              $(DL_DIR)/strace-$(STRACE_VER).tar.xz
$(SRC_DIR)/binutils-$(BINUTILS_VER)/.unpacked:          $(DL_DIR)/binutils-$(BINUTILS_VER).tar.xz

$(SRC_DIR)/gcc-$(GCC_VER)/.unpacked:                    $(DL_DIR)/gcc-$(GCC_VER).tar.xz $(DL_DIR)/gmp-$(GMP_VER).tar.xz $(DL_DIR)/mpfr-$(MPFR_VER).tar.xz $(DL_DIR)/mpc-$(MPC_VER).tar.gz
	@mkdir -p $(SRC_DIR)
	@echo "  ↓  Unpacking gcc $(GCC_VER) and dependencies"
	@tar -xf $(DL_DIR)/gcc-$(GCC_VER).tar.xz -C $(SRC_DIR)
	@tar -xf $(DL_DIR)/gmp-$(GMP_VER).tar.xz -C $(SRC_DIR)/gcc-$(GCC_VER)
	@mv $(SRC_DIR)/gcc-$(GCC_VER)/gmp-$(GMP_VER) $(SRC_DIR)/gcc-$(GCC_VER)/gmp
	@tar -xf $(DL_DIR)/mpfr-$(MPFR_VER).tar.xz -C $(SRC_DIR)/gcc-$(GCC_VER)
	@mv $(SRC_DIR)/gcc-$(GCC_VER)/mpfr-$(MPFR_VER) $(SRC_DIR)/gcc-$(GCC_VER)/mpfr
	@tar -xf $(DL_DIR)/mpc-$(MPC_VER).tar.gz -C $(SRC_DIR)/gcc-$(GCC_VER)
	@mv $(SRC_DIR)/gcc-$(GCC_VER)/mpc-$(MPC_VER) $(SRC_DIR)/gcc-$(GCC_VER)/mpc
	@touch $@

$(SRC_DIR)/nasm-$(NASM_VER)/.unpacked:                  $(DL_DIR)/nasm-$(NASM_VER).tar.xz

$(SRC_DIR)/vim-$(VIM_VER)/.unpacked:                    $(DL_DIR)/vim-$(VIM_VER).tar.gz
$(SRC_DIR)/screen-$(SCREEN_VER)/.unpacked:              $(DL_DIR)/screen-$(SCREEN_VER).tar.gz
$(SRC_DIR)/ncdu-$(NCDU_VER)-linux-x86_64/.unpacked: $(DL_DIR)/ncdu-$(NCDU_VER)-linux-x86_64.tar.gz
$(SRC_DIR)/ripgrep-$(RIPGREP_VER)-x86_64-unknown-linux-musl/.unpacked: $(DL_DIR)/ripgrep-$(RIPGREP_VER)-x86_64-unknown-linux-musl.tar.gz
$(SRC_DIR)/fd-v$(FD_VER)-x86_64-unknown-linux-musl/.unpacked: $(DL_DIR)/fd-v$(FD_VER)-x86_64-unknown-linux-musl.tar.gz
$(SRC_DIR)/bat-v$(BAT_VER)-x86_64-unknown-linux-musl/.unpacked: $(DL_DIR)/bat-v$(BAT_VER)-x86_64-unknown-linux-musl.tar.gz
$(SRC_DIR)/exa-linux-x86_64-musl-v$(EXA_VER)/.unpacked: $(DL_DIR)/exa-linux-x86_64-musl-v$(EXA_VER).zip
$(SRC_DIR)/hexyl-v$(HEXYL_VER)-x86_64-unknown-linux-musl/.unpacked: $(DL_DIR)/hexyl-v$(HEXYL_VER)-x86_64-unknown-linux-musl.tar.gz

# URL_<basename> lookups for the new tarballs
URL_coreutils-$(COREUTILS_VER)    := $(COREUTILS_URL)
URL_sed-$(SED_VER)                := $(SED_URL)
URL_grep-$(GREP_VER)              := $(GREP_URL)
URL_diffutils-$(DIFFUTILS_VER)    := $(DIFFUTILS_URL)
URL_findutils-$(FINDUTILS_VER)    := $(FINDUTILS_URL)
URL_tar-$(GTAR_VER)               := $(GTAR_URL)
URL_gzip-$(GZIP_VER)              := $(GZIP_URL)
URL_util-linux-$(UTIL_LINUX_VER)  := $(UTIL_LINUX_URL)
URL_procps-ng-$(PROCPS_VER)       := $(PROCPS_URL)
URL_bc-$(GBC_VER)                 := $(GBC_URL)
URL_ed-$(ED_VER)                  := $(ED_URL)
URL_screen-$(SCREEN_VER)          := $(SCREEN_URL)
URL_wget-$(WGET_VER)              := $(WGET_URL)
URL_socat-$(SOCAT_VER)            := $(SOCAT_URL)
URL_htop-$(HTOP_VER)              := $(HTOP_URL)
URL_strace-$(STRACE_VER)          := $(STRACE_URL)
URL_binutils-$(BINUTILS_VER)      := $(BINUTILS_URL)
URL_gcc-$(GCC_VER)                := $(GCC_URL)
URL_gmp-$(GMP_VER)                := $(GMP_URL)
URL_mpfr-$(MPFR_VER)              := $(MPFR_URL)
URL_mpc-$(MPC_VER)                := $(MPC_URL)
URL_nasm-$(NASM_VER)              := $(NASM_URL)
URL_vim-$(VIM_VER)                := $(VIM_URL)
URL_screen-$(SCREEN_VER)          := $(SCREEN_URL)
URL_ncdu-$(NCDU_VER)-linux-x86_64 := https://dev.yorhel.nl/download/ncdu-$(NCDU_VER)-linux-x86_64.tar.gz
URL_ripgrep-$(RIPGREP_VER)-x86_64-unknown-linux-musl := https://github.com/BurntSushi/ripgrep/releases/download/$(RIPGREP_VER)/ripgrep-$(RIPGREP_VER)-x86_64-unknown-linux-musl.tar.gz
URL_fd-v$(FD_VER)-x86_64-unknown-linux-musl := https://github.com/sharkdp/fd/releases/download/v$(FD_VER)/fd-v$(FD_VER)-x86_64-unknown-linux-musl.tar.gz
URL_bat-v$(BAT_VER)-x86_64-unknown-linux-musl := https://github.com/sharkdp/bat/releases/download/v$(BAT_VER)/bat-v$(BAT_VER)-x86_64-unknown-linux-musl.tar.gz
URL_exa-linux-x86_64-musl-v$(EXA_VER) := https://github.com/ogham/exa/releases/download/v$(EXA_VER)/exa-linux-x86_64-musl-v$(EXA_VER).zip
URL_hexyl-v$(HEXYL_VER)-x86_64-unknown-linux-musl := https://github.com/sharkdp/hexyl/releases/download/v$(HEXYL_VER)/hexyl-v$(HEXYL_VER)-x86_64-unknown-linux-musl.tar.gz

# ── toybox — a second full toolbox, next to BusyBox ──────────────────────────
# Its defconfig probes the host for features, so HOSTCC stays the real gcc;
# only the target compiler is musl's.
$(PORT_BIN)/toybox: $(MUSL_GCC) $(SRC_DIR)/toybox-$(TOYBOX_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  toybox $(TOYBOX_VER)"
	@# toybox's scripts/genconfig.sh resolves $CC with `command -v`, so CC
	@# has to be one program name and nothing else — no ccache prefix, no
	@# -idirafter. The UAPI header path rides in CFLAGS instead.
	@cd $(SRC_DIR)/toybox-$(TOYBOX_VER) && \
	    $(MAKE) distclean > $(LOG_DIR)/toybox.log 2>&1; \
	    $(MAKE) defconfig CC="$(MUSL_GCC)" HOSTCC=gcc \
	        CFLAGS="$(PORT_CFLAGS) $(UAPI_INC) $(UAPI_ASM)" \
	        >> $(LOG_DIR)/toybox.log 2>&1 && \
	    $(MAKE) -j$(NPROC) CC="$(MUSL_GCC)" HOSTCC=gcc LDFLAGS=--static \
	        CFLAGS="$(PORT_CFLAGS) $(UAPI_INC) $(UAPI_ASM)" \
	        >> $(LOG_DIR)/toybox.log 2>&1
	@cp $(SRC_DIR)/toybox-$(TOYBOX_VER)/toybox $@

# ── Lua — interpreter and bytecode compiler ──────────────────────────────────
# The "posix" target is the one without readline; nothing on the image
# provides libreadline, and a static link would need it at build time.
$(PORT_BIN)/lua $(PORT_BIN)/luac &: $(MUSL_GCC) $(SRC_DIR)/lua-$(LUA_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  lua $(LUA_VER)"
	@$(MAKE) -C $(SRC_DIR)/lua-$(LUA_VER) posix -j$(NPROC) \
	    CC="$(PORT_CC) -std=gnu99" MYLDFLAGS="$(PORT_LDFLAGS)" \
	    > $(LOG_DIR)/lua.log 2>&1
	@cp $(SRC_DIR)/lua-$(LUA_VER)/src/lua  $(PORT_BIN)/lua
	@cp $(SRC_DIR)/lua-$(LUA_VER)/src/luac $(PORT_BIN)/luac

# ── SQLite — the command-line shell, built from the amalgamation ─────────────
# Compiling the two amalgamation sources directly is both faster than the
# bundled configure and independent of readline/ncurses.
$(PORT_BIN)/sqlite3: $(MUSL_GCC) $(SRC_DIR)/sqlite-autoconf-$(SQLITE_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  sqlite $(SQLITE_VER)"
	@cd $(SRC_DIR)/sqlite-autoconf-$(SQLITE_VER) && \
	    $(PORT_CC) $(PORT_CFLAGS) $(PORT_LDFLAGS) -o $@ shell.c sqlite3.c \
	        -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
	        -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 -lm \
	        > $(LOG_DIR)/sqlite3.log 2>&1

# ── dash — a small POSIX shell ───────────────────────────────────────────────
$(PORT_BIN)/dash: $(MUSL_GCC) $(SRC_DIR)/dash-$(DASH_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  dash $(DASH_VER)"
	@# dash's configure only honours $CC_FOR_BUILD when it decides it is
	@# cross-compiling, and it does not decide that here; a make-command-line
	@# assignment overrides the Makefile's own regardless, so the mksyntax /
	@# mknodes generators it builds and runs are host binaries.
	@cd $(SRC_DIR)/dash-$(DASH_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        > $(LOG_DIR)/dash.log 2>&1 && \
	    $(MAKE) -j$(NPROC) CC_FOR_BUILD=gcc >> $(LOG_DIR)/dash.log 2>&1
	@cp $(SRC_DIR)/dash-$(DASH_VER)/src/dash $@

# ── bzip2 ────────────────────────────────────────────────────────────────────
# No configure; its Makefile takes CC and links with $(CC), so -static rides
# along on CC itself.
$(PORT_BIN)/bzip2: $(MUSL_GCC) $(SRC_DIR)/bzip2-$(BZIP2_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  bzip2 $(BZIP2_VER)"
	@$(MAKE) -C $(SRC_DIR)/bzip2-$(BZIP2_VER) bzip2 -j$(NPROC) \
	    CC="$(PORT_CC) $(PORT_LDFLAGS)" CFLAGS="$(PORT_CFLAGS) -D_FILE_OFFSET_BITS=64" \
	    > $(LOG_DIR)/bzip2.log 2>&1
	@cp $(SRC_DIR)/bzip2-$(BZIP2_VER)/bzip2 $@

# ── xz ───────────────────────────────────────────────────────────────────────
$(PORT_BIN)/xz: $(MUSL_GCC) $(SRC_DIR)/xz-$(XZ_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  xz $(XZ_VER)"
	@cd $(SRC_DIR)/xz-$(XZ_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        --disable-doc --disable-scripts \
	        > $(LOG_DIR)/xz.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/xz.log 2>&1
	@cp $(SRC_DIR)/xz-$(XZ_VER)/src/xz/xz $@

# ── zstd ─────────────────────────────────────────────────────────────────────
# The optional zlib/lzma/lz4 pass-through support is switched off: none of
# those libraries exist in this musl sysroot.
$(PORT_BIN)/zstd: $(MUSL_GCC) $(SRC_DIR)/zstd-$(ZSTD_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  zstd $(ZSTD_VER)"
	@$(MAKE) -C $(SRC_DIR)/zstd-$(ZSTD_VER)/programs zstd -j$(NPROC) \
	    CC="$(PORT_CC)" CFLAGS="$(PORT_CFLAGS)" LDFLAGS="$(PORT_LDFLAGS)" \
	    HAVE_ZLIB=0 HAVE_LZMA=0 HAVE_LZ4=0 \
	    > $(LOG_DIR)/zstd.log 2>&1
	@cp $(SRC_DIR)/zstd-$(ZSTD_VER)/programs/zstd $@

# ── GNU make — so the OS can drive its own builds, with tcc below ────────────
$(PORT_BIN)/make: $(MUSL_GCC) $(SRC_DIR)/make-$(MAKE_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  make $(MAKE_VER)"
	@cd $(SRC_DIR)/make-$(MAKE_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) --without-guile \
	        > $(LOG_DIR)/make.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/make.log 2>&1
	@cp $(SRC_DIR)/make-$(MAKE_VER)/make $@

# ── curl — a real HTTP client on top of this kernel's TCP stack ──────────────
# Plain HTTP only: TLS would mean building OpenSSL too, and the point here is
# the syscall/socket path, not the crypto.
$(PORT_BIN)/curl: $(MUSL_GCC) $(SRC_DIR)/curl-$(CURL_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  curl $(CURL_VER) (no TLS)"
	@cd $(SRC_DIR)/curl-$(CURL_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        --without-ssl --without-zlib --without-brotli --without-zstd \
	        --without-libpsl --without-nghttp2 --without-libidn2 \
	        --disable-ldap --disable-ldaps --disable-docs --disable-manual \
	        > $(LOG_DIR)/curl.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/curl.log 2>&1
	@cp $(SRC_DIR)/curl-$(CURL_VER)/src/curl $@

# ── jq ───────────────────────────────────────────────────────────────────────
$(PORT_BIN)/jq: $(MUSL_GCC) $(SRC_DIR)/jq-$(JQ_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  jq $(JQ_VER)"
	@cd $(SRC_DIR)/jq-$(JQ_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        --with-oniguruma=no --disable-docs --disable-maintainer-mode \
	        > $(LOG_DIR)/jq.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/jq.log 2>&1
	@cp $(SRC_DIR)/jq-$(JQ_VER)/jq $@

# ── TinyCC — a C compiler that runs on the OS ────────────────────────────────
# tcc needs more than its binary at runtime: libtcc1.a and its own headers.
# `make install` into out/tcc lays those out the way the compiler looks for
# them (prefix /usr), and the package ships that whole tree.
$(PORT_BIN)/tcc: $(MUSL_GCC) $(SRC_DIR)/tinycc-$(TCC_COMMIT)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR) $(OUT_DIR)/tcc
	@echo "  ⚙  tcc $(TCC_VER)"
	@# x86_64-libtcc1-usegcc=yes is tcc's own switch for building its
	@# runtime library with the system compiler instead of with the tcc it
	@# just linked. It is needed for the same reason as the paths below: the
	@# fresh tcc is configured to look for headers where AzamiOS keeps them,
	@# so it cannot compile anything on this build host. libtcc1.a is target
	@# code either way, and musl-gcc targets the same ABI.
	@#
	@# tccdefs_.h is generated by a helper tcc compiles and then runs during
	@# its own build; built with musl-gcc it is a dynamic musl binary this
	@# host cannot execute, so that one step is made with the host compiler
	@# first. It emits a header, not target code — nothing about it depends
	@# on which libc the finished compiler is linked against.
	@#
	@# Configured for the filesystem it will run on, not for this build
	@# host: AzamiOS keeps its libc.a and startup objects in /usr/lib and
	@# its headers in /usr/include, and tcc bakes those search paths in at
	@# configure time. Without this, `tcc hello.c -o hello` on the OS would
	@# look for a Debian layout that is not there. {B} is tcc's own
	@# directory (/usr/lib/tcc), where libtcc1.a and its headers live. It
	@# leads the library paths as well as the include paths: tcc looks for
	@# its own runtime library in the first library path, so without {B}
	@# there it reports "file 'libtcc1.a' not found" at link time even
	@# though the file is installed —
	@# first in the search order, the way a compiler's own headers always
	@# are, because <stdarg.h>, <stddef.h> and <float.h> belong to the
	@# compiler and this libc deliberately does not ship them.
	@cd $(SRC_DIR)/tinycc-$(TCC_COMMIT) && \
	    ./configure --cc="$(MUSL_GCC)" --prefix=/usr --config-musl \
	        --crtprefix=/usr/lib:/lib \
	        --libpaths={B}:/usr/lib:/lib:/usr/local/lib \
	        --sysincludepaths={B}/include:/usr/include:/usr/local/include \
	        --elfinterp=/lib/ld-azami.so \
	        --extra-cflags="$(PORT_CFLAGS)" --extra-ldflags="$(PORT_LDFLAGS)" \
	        > $(LOG_DIR)/tcc.log 2>&1 && \
	    rm -f c2str.exe tccdefs_.h && \
	    $(MAKE) tccdefs_.h CC=gcc >> $(LOG_DIR)/tcc.log 2>&1 && \
	    $(MAKE) -j$(NPROC) x86_64-libtcc1-usegcc=yes GITHASH="$(TCC_COMMIT)" \
	        >> $(LOG_DIR)/tcc.log 2>&1 && \
	    $(MAKE) install DESTDIR=$(OUT_DIR)/tcc x86_64-libtcc1-usegcc=yes GITHASH="$(TCC_COMMIT)" \
	        >> $(LOG_DIR)/tcc.log 2>&1
	@cp $(OUT_DIR)/tcc/usr/bin/tcc $@

# ── ncurses — not a port of its own, the dependency nano and less need ───────
# Built with compiled-in fallback terminal descriptions for the terminals
# AzamiOS's console actually claims to be, so nothing needs a terminfo
# database on the image at runtime.
NCURSES_LIB := $(OUT_DIR)/ncurses/lib/libncursesw.a
$(NCURSES_LIB): $(MUSL_GCC) $(SRC_DIR)/ncurses-$(NCURSES_VER)/.unpacked
	@mkdir -p $(LOG_DIR) $(OUT_DIR)/ncurses
	@echo "  ⚙  ncurses $(NCURSES_VER) (dependency of nano/less)"
	@cd $(SRC_DIR)/ncurses-$(NCURSES_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        --prefix=$(OUT_DIR)/ncurses --without-ada --without-tests \
	        --with-build-cc=gcc \
	        --without-manpages --without-progs --without-cxx-binding \
	        --enable-widec --with-fallbacks=linux,vt100,xterm,xterm-256color,ansi,dumb \
	        --disable-database --disable-db-install \
	        > $(LOG_DIR)/ncurses.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/ncurses.log 2>&1 && \
	    $(MAKE) install >> $(LOG_DIR)/ncurses.log 2>&1
	@# A widec build installs libncursesw.a and headers under include/ncursesw
	@# only. nano's and less's configure scripts test for -lncurses/-lcurses
	@# and <curses.h> at the top of the include path, so give them those
	@# names; there is one library here either way.
	@cd $(OUT_DIR)/ncurses/lib && \
	    for n in libncurses.a libcurses.a libtinfo.a; do ln -sf libncursesw.a $$n; done
	@cd $(OUT_DIR)/ncurses/include && \
	    for h in ncursesw/*.h; do ln -sf $$h $$(basename $$h); done

# ── nano — a full-screen editor ──────────────────────────────────────────────
$(PORT_BIN)/nano: $(NCURSES_LIB) $(SRC_DIR)/nano-$(NANO_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  nano $(NANO_VER)"
	@# nano reaches for <linux/vt.h> through musl's bits/vt.h, so it needs
	@# the host UAPI headers after musl's own, same as toybox and BusyBox.
	@cd $(SRC_DIR)/nano-$(NANO_VER) && \
	    CC="$(PORT_CC_UAPI)" CC_FOR_BUILD=gcc \
	    CFLAGS="$(PORT_CFLAGS) -I$(OUT_DIR)/ncurses/include -I$(OUT_DIR)/ncurses/include/ncursesw" \
	    CPPFLAGS="-I$(OUT_DIR)/ncurses/include -I$(OUT_DIR)/ncurses/include/ncursesw" \
	    LDFLAGS="$(PORT_LDFLAGS) -L$(OUT_DIR)/ncurses/lib" \
	    CURSES_LIB="-lncursesw" NCURSESW_CFLAGS="-I$(OUT_DIR)/ncurses/include/ncursesw" \
	    NCURSESW_LIBS="-L$(OUT_DIR)/ncurses/lib -lncursesw" \
	    ./configure $(PORT_CONFIGURE_ARGS) --disable-utf8 --disable-libmagic \
	        --enable-tiny --disable-browser --disable-help \
	        > $(LOG_DIR)/nano.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/nano.log 2>&1
	@cp $(SRC_DIR)/nano-$(NANO_VER)/src/nano $@

# ── less ─────────────────────────────────────────────────────────────────────
$(PORT_BIN)/less: $(NCURSES_LIB) $(SRC_DIR)/less-$(LESS_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  less $(LESS_VER)"
	@cd $(SRC_DIR)/less-$(LESS_VER) && \
	    CC="$(PORT_CC)" CC_FOR_BUILD=gcc CFLAGS="$(PORT_CFLAGS) -I$(OUT_DIR)/ncurses/include" \
	    CPPFLAGS="-I$(OUT_DIR)/ncurses/include" \
	    LDFLAGS="$(PORT_LDFLAGS) -L$(OUT_DIR)/ncurses/lib" LIBS="-lncursesw" \
	    ./configure $(PORT_CONFIGURE_ARGS) \
	        > $(LOG_DIR)/less.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/less.log 2>&1
	@cp $(SRC_DIR)/less-$(LESS_VER)/less $@

# ── gawk ─────────────────────────────────────────────────────────────────────
$(PORT_BIN)/gawk: $(MUSL_GCC) $(SRC_DIR)/gawk-$(GAWK_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  gawk $(GAWK_VER)"
	@cd $(SRC_DIR)/gawk-$(GAWK_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        --disable-extensions --without-readline --without-mpfr \
	        > $(LOG_DIR)/gawk.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/gawk.log 2>&1
	@cp $(SRC_DIR)/gawk-$(GAWK_VER)/gawk $@

# ── bash ─────────────────────────────────────────────────────────────────────
# Without readline: its line editing wants a terminal driver richer than this
# console, and the bundled readline would have to be statically linked too.
$(PORT_BIN)/bash: $(MUSL_GCC) $(SRC_DIR)/bash-$(BASH_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  bash $(BASH_VER)"
	@# bash ships its own strtoimax and, against musl, links it *as well as*
	@# the libc one — the static link then fails with "multiple definition
	@# of strtoimax". Read bash's configure carefully and the cache variable
	@# runs backwards from what its name suggests: bash_cv_func_strtoimax=yes
	@# is what *adds* lib/sh/strtoimax.o to LIBOBJS, so "no" is the setting
	@# that leaves musl's alone.
	@cd $(SRC_DIR)/bash-$(BASH_VER) && \
	    $(PORT_CONFIGURE_ENV) bash_cv_func_strtoimax=no ./configure $(PORT_CONFIGURE_ARGS) \
	        --without-bash-malloc --disable-readline --disable-history \
	        > $(LOG_DIR)/bash.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/bash.log 2>&1
	@cp $(SRC_DIR)/bash-$(BASH_VER)/bash $@

# ── file(1) — needs its magic database alongside the binary ──────────────────
$(PORT_BIN)/file: $(MUSL_GCC) $(SRC_DIR)/file-$(FILE_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR) $(OUT_DIR)/file
	@echo "  ⚙  file $(FILE_VER)"
	@# file(1) compiles its own magic database by *running* the file binary
	@# it has just linked. libtool links that binary dynamically unless told
	@# -all-static, and a dynamic musl binary will not run on a build host
	@# with no musl loader installed — so the build dies on its own output.
	@# -all-static goes on the make line, not configure's: it is a libtool
	@# option, and the compiler rejects it outright in a conftest link.
	@# --prefix=/usr is not cosmetic here: file(1) bakes the path of its
	@# magic database into the binary at configure time, and with the
	@# default prefix it looks in /usr/local/share/misc for a database the
	@# package installs under /usr/share/misc — "could not find any valid
	@# magic files!" on every run.
	@cd $(SRC_DIR)/file-$(FILE_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) --prefix=/usr \
	        --disable-libseccomp --disable-bzlib --disable-xzlib --disable-zlib \
	        --disable-zstdlib --disable-lzlib \
	        > $(LOG_DIR)/file.log 2>&1 && \
	    $(MAKE) -j$(NPROC) LDFLAGS="$(PORT_LDFLAGS) -all-static" \
	        >> $(LOG_DIR)/file.log 2>&1 && \
	    $(MAKE) install DESTDIR=$(OUT_DIR)/file prefix=/usr >> $(LOG_DIR)/file.log 2>&1
	@cp $(SRC_DIR)/file-$(FILE_VER)/src/file $@

# ── tree ─────────────────────────────────────────────────────────────────────
$(PORT_BIN)/tree: $(MUSL_GCC) $(SRC_DIR)/unix-tree-$(TREE_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  tree $(TREE_VER)"
	@$(MAKE) -C $(SRC_DIR)/unix-tree-$(TREE_VER) -j$(NPROC) \
	    CC="$(PORT_CC)" CFLAGS="$(PORT_CFLAGS)" LDFLAGS="$(PORT_LDFLAGS)" \
	    > $(LOG_DIR)/tree.log 2>&1
	@cp $(SRC_DIR)/unix-tree-$(TREE_VER)/tree $@

# ── MicroPython — a Python 3 interpreter small enough to be sane here ────────
# The "minimal" variant of the unix port drops the modules that would pull in
# libffi, which this sysroot does not have.
$(PORT_BIN)/micropython: $(MUSL_GCC) $(SRC_DIR)/micropython-$(MICROPY_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  micropython $(MICROPY_VER)"
	@$(MAKE) -C $(SRC_DIR)/micropython-$(MICROPY_VER)/ports/unix \
	    VARIANT=minimal -j$(NPROC) \
	    CC="$(PORT_CC)" LDFLAGS_EXTRA="$(PORT_LDFLAGS)" STRIP=strip \
	    > $(LOG_DIR)/micropython.log 2>&1
	@cp $(SRC_DIR)/micropython-$(MICROPY_VER)/ports/unix/build-minimal/micropython $@

# ==============================================================================
# New GNU / upstream ports
# ==============================================================================

# ── GNU coreutils — the authoritative POSIX userland ─────────────────────────
# Installs ~100 tools; we copy the full install tree under out/coreutils and
# expose the most useful ones individually in out/bin so the package generator
# can wrap each as a separate package entry.
# --enable-single-binary=symlinks keeps one ELF for each tool rather than a
# busybox-style multicall, which makes the per-binary packages simpler.
# gl_cv_func_getlocalename_l_ok avoids a configure test that tries to run a
# musl binary on the build host.
$(PORT_BIN)/coreutils: $(MUSL_GCC) $(SRC_DIR)/coreutils-$(COREUTILS_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR) $(OUT_DIR)/coreutils
	@echo "  ⚙  GNU coreutils $(COREUTILS_VER)"
	@cd $(SRC_DIR)/coreutils-$(COREUTILS_VER) && \
	    $(PORT_CONFIGURE_ENV) \
	    gl_cv_func_getlocalename_l_ok=yes \
	    ./configure $(PORT_CONFIGURE_ARGS) \
	        --prefix=$(OUT_DIR)/coreutils \
	        --without-gmp --disable-acl --disable-xattr \
	        > $(LOG_DIR)/coreutils.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/coreutils.log 2>&1 && \
	    $(MAKE) install    >> $(LOG_DIR)/coreutils.log 2>&1
	@# Expose the top-level multicall wrapper and the most important tools
	@cp $(OUT_DIR)/coreutils/bin/ls        $(PORT_BIN)/ls-gnu     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/cat       $(PORT_BIN)/cat-gnu    2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/cp        $(PORT_BIN)/cp-gnu     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/mv        $(PORT_BIN)/mv-gnu     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/rm        $(PORT_BIN)/rm-gnu     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/sort      $(PORT_BIN)/sort-gnu   2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/uniq      $(PORT_BIN)/uniq-gnu   2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/wc        $(PORT_BIN)/wc-gnu     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/head      $(PORT_BIN)/head-gnu   2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/tail      $(PORT_BIN)/tail-gnu   2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/cut       $(PORT_BIN)/cut-gnu    2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/tr        $(PORT_BIN)/tr-gnu     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/tac       $(PORT_BIN)/tac        2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/rev       $(PORT_BIN)/rev        2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/shuf      $(PORT_BIN)/shuf       2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/factor    $(PORT_BIN)/factor     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/numfmt    $(PORT_BIN)/numfmt     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/seq       $(PORT_BIN)/seq-gnu    2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/yes       $(PORT_BIN)/yes-gnu    2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/printf    $(PORT_BIN)/printf-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/od        $(PORT_BIN)/od-gnu     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/paste     $(PORT_BIN)/paste-gnu  2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/join      $(PORT_BIN)/join       2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/expand    $(PORT_BIN)/expand-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/unexpand  $(PORT_BIN)/unexpand-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/fold      $(PORT_BIN)/fold-gnu   2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/fmt       $(PORT_BIN)/fmt        2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/nl        $(PORT_BIN)/nl-gnu     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/pr        $(PORT_BIN)/pr-gnu     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/tsort     $(PORT_BIN)/tsort-gnu  2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/csplit    $(PORT_BIN)/csplit     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/split     $(PORT_BIN)/split-gnu  2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/comm      $(PORT_BIN)/comm-gnu   2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/mkfifo    $(PORT_BIN)/mkfifo     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/mknod     $(PORT_BIN)/mknod      2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/ln        $(PORT_BIN)/ln-gnu     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/stat      $(PORT_BIN)/stat-gnu   2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/chmod     $(PORT_BIN)/chmod-gnu  2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/chown     $(PORT_BIN)/chown      2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/touch     $(PORT_BIN)/touch-gnu  2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/md5sum    $(PORT_BIN)/md5sum-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/sha256sum $(PORT_BIN)/sha256sum-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/sha512sum $(PORT_BIN)/sha512sum-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/b2sum     $(PORT_BIN)/b2sum      2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/cksum     $(PORT_BIN)/cksum-gnu  2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/base64    $(PORT_BIN)/base64-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/base32    $(PORT_BIN)/base32     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/sleep     $(PORT_BIN)/sleep-gnu  2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/date      $(PORT_BIN)/date-gnu   2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/df        $(PORT_BIN)/df-gnu     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/du        $(PORT_BIN)/du-gnu     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/stty      $(PORT_BIN)/stty       2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/tty       $(PORT_BIN)/tty-gnu    2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/who       $(PORT_BIN)/who-gnu    2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/whoami    $(PORT_BIN)/whoami-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/id        $(PORT_BIN)/id-gnu     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/groups    $(PORT_BIN)/groups-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/logname   $(PORT_BIN)/logname-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/users     $(PORT_BIN)/users-gnu  2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/env       $(PORT_BIN)/env-gnu    2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/nohup     $(PORT_BIN)/nohup-gnu  2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/timeout   $(PORT_BIN)/timeout-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/nice      $(PORT_BIN)/nice-gnu   2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/nproc     $(PORT_BIN)/nproc      2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/sync      $(PORT_BIN)/sync-gnu   2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/truncate  $(PORT_BIN)/truncate-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/mktemp    $(PORT_BIN)/mktemp-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/readlink  $(PORT_BIN)/readlink-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/realpath  $(PORT_BIN)/realpath-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/dirname   $(PORT_BIN)/dirname-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/basename  $(PORT_BIN)/basename-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/pathchk   $(PORT_BIN)/pathchk-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/printenv  $(PORT_BIN)/printenv-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/uptime    $(PORT_BIN)/uptime-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/uname     $(PORT_BIN)/uname-gnu  2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/arch      $(PORT_BIN)/arch       2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/hostid    $(PORT_BIN)/hostid     2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/chroot    $(PORT_BIN)/chroot-gnu 2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/install   $(PORT_BIN)/install    2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/dd        $(PORT_BIN)/dd         2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/mkdir     $(PORT_BIN)/mkdir-gnu  2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/rmdir     $(PORT_BIN)/rmdir      2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/pwd       $(PORT_BIN)/pwd-gnu    2>/dev/null || true
	@cp $(OUT_DIR)/coreutils/bin/ls        $(PORT_BIN)/coreutils  2>/dev/null || true
	@echo "  ✓  coreutils $(COREUTILS_VER): $$(ls $(OUT_DIR)/coreutils/bin | wc -l) tools installed"

# ── GNU sed — the stream editor ───────────────────────────────────────────────
$(PORT_BIN)/sed: $(MUSL_GCC) $(SRC_DIR)/sed-$(SED_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  GNU sed $(SED_VER)"
	@cd $(SRC_DIR)/sed-$(SED_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        > $(LOG_DIR)/sed.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/sed.log 2>&1
	@cp $(SRC_DIR)/sed-$(SED_VER)/sed/sed $@

# ── GNU grep — regular expression file search ─────────────────────────────────
# --disable-perl-regexp avoids a libpcre dependency that would require
# building PCRE2 as well. The POSIX ERE engine is more than sufficient.
$(PORT_BIN)/grep: $(MUSL_GCC) $(SRC_DIR)/grep-$(GREP_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  GNU grep $(GREP_VER)"
	@cd $(SRC_DIR)/grep-$(GREP_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        --disable-perl-regexp \
	        > $(LOG_DIR)/grep.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/grep.log 2>&1
	@cp $(SRC_DIR)/grep-$(GREP_VER)/src/grep $@
	@cp $(SRC_DIR)/grep-$(GREP_VER)/src/grep $(PORT_BIN)/egrep-gnu 2>/dev/null || true
	@cp $(SRC_DIR)/grep-$(GREP_VER)/src/grep $(PORT_BIN)/fgrep-gnu 2>/dev/null || true

# ── GNU diffutils — diff, cmp, comm, sdiff ────────────────────────────────────
$(PORT_BIN)/diff: $(MUSL_GCC) $(SRC_DIR)/diffutils-$(DIFFUTILS_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  GNU diffutils $(DIFFUTILS_VER)"
	@cd $(SRC_DIR)/diffutils-$(DIFFUTILS_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        > $(LOG_DIR)/diffutils.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/diffutils.log 2>&1
	@cp $(SRC_DIR)/diffutils-$(DIFFUTILS_VER)/src/diff   $@
	@cp $(SRC_DIR)/diffutils-$(DIFFUTILS_VER)/src/cmp    $(PORT_BIN)/cmp-gnu   2>/dev/null || true
	@cp $(SRC_DIR)/diffutils-$(DIFFUTILS_VER)/src/sdiff  $(PORT_BIN)/sdiff     2>/dev/null || true
	@cp $(SRC_DIR)/diffutils-$(DIFFUTILS_VER)/src/diff3  $(PORT_BIN)/diff3     2>/dev/null || true

# ── GNU findutils — find, xargs, locate ──────────────────────────────────────
# --disable-rpath avoids a run-time RPATH being embedded in a static binary.
$(PORT_BIN)/find: $(MUSL_GCC) $(SRC_DIR)/findutils-$(FINDUTILS_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  GNU findutils $(FINDUTILS_VER)"
	@cd $(SRC_DIR)/findutils-$(FINDUTILS_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        --disable-rpath \
	        > $(LOG_DIR)/findutils.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/findutils.log 2>&1
	@cp $(SRC_DIR)/findutils-$(FINDUTILS_VER)/find/find  $@
	@cp $(SRC_DIR)/findutils-$(FINDUTILS_VER)/xargs/xargs $(PORT_BIN)/xargs-gnu 2>/dev/null || true
	@cp $(SRC_DIR)/findutils-$(FINDUTILS_VER)/locate/locate $(PORT_BIN)/locate  2>/dev/null || true
	@cp $(SRC_DIR)/findutils-$(FINDUTILS_VER)/locate/updatedb $(PORT_BIN)/updatedb 2>/dev/null || true

# ── GNU tar — full-featured archiver ─────────────────────────────────────────
# No external compression libraries: gzip/bzip2/xz are called as child
# processes (--use-compress-program) and those binaries exist on the OS.
$(PORT_BIN)/gtar: $(MUSL_GCC) $(SRC_DIR)/tar-$(GTAR_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  GNU tar $(GTAR_VER)"
	@cd $(SRC_DIR)/tar-$(GTAR_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        --without-lzma --without-lzo --without-lz4 --without-zstd \
	        --without-bz2lib --without-zlib \
	        > $(LOG_DIR)/gtar.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/gtar.log 2>&1
	@cp $(SRC_DIR)/tar-$(GTAR_VER)/src/tar $@

# ── GNU gzip — compress and expand files ─────────────────────────────────────
$(PORT_BIN)/gzip: $(MUSL_GCC) $(SRC_DIR)/gzip-$(GZIP_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  GNU gzip $(GZIP_VER)"
	@cd $(SRC_DIR)/gzip-$(GZIP_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        > $(LOG_DIR)/gzip.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/gzip.log 2>&1
	@cp $(SRC_DIR)/gzip-$(GZIP_VER)/gzip   $@
	@cp $(SRC_DIR)/gzip-$(GZIP_VER)/gzip   $(PORT_BIN)/gunzip-gnu  2>/dev/null || true
	@cp $(SRC_DIR)/gzip-$(GZIP_VER)/gzip   $(PORT_BIN)/zcat-gnu    2>/dev/null || true

# ── util-linux — column, rev, tac, shuf, logger, setsid, mkfifo, and more ────
# util-linux is a big package; we build only the userland tools (--disable-all-programs
# then re-enable the ones we want), skipping anything that needs libblkid or
# libmount linking complications.
$(PORT_BIN)/column: $(MUSL_GCC) $(SRC_DIR)/util-linux-$(UTIL_LINUX_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR) $(OUT_DIR)/util-linux
	@echo "  ⚙  util-linux $(UTIL_LINUX_VER)"
	@cd $(SRC_DIR)/util-linux-$(UTIL_LINUX_VER) && \
	    $(PORT_CONFIGURE_ENV) CFLAGS="$(PORT_CFLAGS) $(UAPI_INC) $(UAPI_ASM)" \
	    ADJTIME_PATH=/var/lib/hwclock/adjtime \
	    ./configure $(PORT_CONFIGURE_ARGS) \
	        --prefix=$(OUT_DIR)/util-linux \
	        --disable-all-programs \
	        --enable-column \
	        --enable-rev \
	        --enable-tac \
	        --enable-shuf \
	        --enable-logger \
	        --enable-setsid \
	        --enable-mkfifo \
	        --enable-mknod \
	        --enable-more \
	        --enable-mesg \
	        --enable-wall \
	        --enable-script \
	        --enable-scriptreplay \
	        --enable-look \
	        --enable-hexdump \
	        --enable-rename \
	        --enable-cal \
	        --enable-col \
	        --enable-colcrt \
	        --enable-colrm \
	        --enable-whereis \
	        --enable-getopt \
	        --without-python --without-udev --without-readline \
	        --without-libz --without-cap-ng --without-libmagic \
	        --without-audit --without-utempter \
	        --disable-use-tty-group --disable-makeinstall-chown \
	        > $(LOG_DIR)/util-linux.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/util-linux.log 2>&1 && \
	    $(MAKE) install    >> $(LOG_DIR)/util-linux.log 2>&1
	@for tool in column rev tac shuf logger setsid more mesg wall \
	             script scriptreplay look hexdump rename cal \
	             col colcrt colrm whereis getopt; do \
	    src=$(OUT_DIR)/util-linux/bin/$$tool; \
	    [ -f $$src ] || src=$(OUT_DIR)/util-linux/sbin/$$tool; \
	    [ -f $$src ] || src=$(OUT_DIR)/util-linux/usr/bin/$$tool; \
	    [ -f $$src ] && cp $$src $(PORT_BIN)/$$tool || true; \
	done
	@[ -f $(PORT_BIN)/column ] || cp $(OUT_DIR)/util-linux/bin/column $(PORT_BIN)/column 2>/dev/null || true
	@echo "  ✓  util-linux $(UTIL_LINUX_VER) tools staged"

# ── procps-ng — ps, top, vmstat, pgrep, pkill, sysctl, watch, free ───────────
# No ncurses dependency: plain top requires it, but --disable-modern-top
# builds the legacy curses-free version.
$(PORT_BIN)/ps: $(MUSL_GCC) $(SRC_DIR)/procps-ng-$(PROCPS_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  procps-ng $(PROCPS_VER)"
	@cd $(SRC_DIR)/procps-ng-$(PROCPS_VER) && \
	    $(PORT_CONFIGURE_ENV) PKG_CONFIG=true ./configure $(PORT_CONFIGURE_ARGS) \
	        --disable-modern-top \
	        --without-ncurses \
	        --without-systemd \
	        > $(LOG_DIR)/procps.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/procps.log 2>&1
	@cp $(SRC_DIR)/procps-ng-$(PROCPS_VER)/src/free          $(PORT_BIN)/free-gnu   2>/dev/null || true
	@cp $(SRC_DIR)/procps-ng-$(PROCPS_VER)/src/vmstat        $(PORT_BIN)/vmstat-gnu 2>/dev/null || true
	@cp $(SRC_DIR)/procps-ng-$(PROCPS_VER)/src/top/top       $(PORT_BIN)/top-gnu    2>/dev/null || true
	@cp $(SRC_DIR)/procps-ng-$(PROCPS_VER)/src/pgrep         $(PORT_BIN)/pgrep-gnu  2>/dev/null || true
	@cp $(SRC_DIR)/procps-ng-$(PROCPS_VER)/src/pkill         $(PORT_BIN)/pkill-gnu  2>/dev/null || true
	@cp $(SRC_DIR)/procps-ng-$(PROCPS_VER)/src/sysctl        $(PORT_BIN)/sysctl-gnu 2>/dev/null || true
	@cp $(SRC_DIR)/procps-ng-$(PROCPS_VER)/src/watch         $(PORT_BIN)/watch-gnu  2>/dev/null || true
	@cp $(SRC_DIR)/procps-ng-$(PROCPS_VER)/src/ps/pscommand  $@

# ── GNU bc/dc — arbitrary-precision calculator ────────────────────────────────
$(PORT_BIN)/bc $(PORT_BIN)/dc &: $(MUSL_GCC) $(SRC_DIR)/bc-$(GBC_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  GNU bc $(GBC_VER)"
	@cd $(SRC_DIR)/bc-$(GBC_VER) && \
	    ./configure && $(MAKE) -j$(NPROC) && cp bc/libmath.h ../libmath.h.save && \
	    $(MAKE) distclean && \
	    sed -i 's/SUBDIRS = lib bc dc doc/SUBDIRS = lib bc dc/' Makefile.in && \
	    $(PORT_CONFIGURE_ENV) MAKEINFO=true ./configure $(PORT_CONFIGURE_ARGS) \
	        > $(LOG_DIR)/bc.log 2>&1 && \
	    cp ../libmath.h.save bc/libmath.h && \
	    sed -i '/^libmath\.h:/,+5d' bc/Makefile && echo 'libmath.h:' >> bc/Makefile && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/bc.log 2>&1
	@cp $(SRC_DIR)/bc-$(GBC_VER)/bc/bc $(PORT_BIN)/bc 2>/dev/null || \
	    cp $(SRC_DIR)/bc-$(GBC_VER)/bc  $(PORT_BIN)/bc 2>/dev/null || true
	@cp $(SRC_DIR)/bc-$(GBC_VER)/dc/dc $(PORT_BIN)/dc 2>/dev/null || \
	    cp $(SRC_DIR)/bc-$(GBC_VER)/dc  $(PORT_BIN)/dc 2>/dev/null || true

# ── GNU ed — the classic line editor ─────────────────────────────────────────
$(PORT_BIN)/ed: $(MUSL_GCC) $(SRC_DIR)/ed-$(ED_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  GNU ed $(ED_VER)"
	@cd $(SRC_DIR)/ed-$(ED_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        > $(LOG_DIR)/ed.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/ed.log 2>&1
	@cp $(SRC_DIR)/ed-$(ED_VER)/ed $@

# ── GNU wget — non-interactive downloader ─────────────────────────────────────
# No TLS: same decision as the existing curl port — exercises the network stack,
# not the crypto layer. --without-ssl / --without-gnutls enforce plain HTTP.
$(PORT_BIN)/wget: $(MUSL_GCC) $(SRC_DIR)/wget-$(WGET_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  GNU wget $(WGET_VER) (no TLS)"
	@cd $(SRC_DIR)/wget-$(WGET_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        --without-ssl --without-gnutls --without-libuuid \
	        --without-libpsl --without-libidn2 \
	        --disable-ntlm --disable-digest --disable-iri \
	        > $(LOG_DIR)/wget.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/wget.log 2>&1
	@cp $(SRC_DIR)/wget-$(WGET_VER)/src/wget $@

# ── socat — multipurpose relay (SOcket CAT) ──────────────────────────────────
# No OpenSSL: avoids pulling in a large dependency for a port whose primary
# value is the raw socket/pipe relay functionality.
$(PORT_BIN)/socat: $(MUSL_GCC) $(SRC_DIR)/socat-$(SOCAT_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  socat $(SOCAT_VER)"
	@cd $(SRC_DIR)/socat-$(SOCAT_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        --disable-openssl --disable-readline --disable-fips \
	        > $(LOG_DIR)/socat.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/socat.log 2>&1
	@cp $(SRC_DIR)/socat-$(SOCAT_VER)/socat $@

# ── htop — interactive process viewer ─────────────────────────────────────────
# Depends on ncurses (built as part of nano/less above).
$(PORT_BIN)/htop: $(NCURSES_LIB) $(SRC_DIR)/htop-$(HTOP_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  htop $(HTOP_VER)"
	@cd $(SRC_DIR)/htop-$(HTOP_VER) && \
	    ./autogen.sh >> $(LOG_DIR)/htop.log 2>&1; true && \
	    CC="$(PORT_CC)" CC_FOR_BUILD=gcc \
	    CFLAGS="$(PORT_CFLAGS) -I$(OUT_DIR)/ncurses/include -I$(OUT_DIR)/ncurses/include/ncursesw" \
	    CPPFLAGS="-I$(OUT_DIR)/ncurses/include -I$(OUT_DIR)/ncurses/include/ncursesw" \
	    LDFLAGS="$(PORT_LDFLAGS) -L$(OUT_DIR)/ncurses/lib" \
	    NCURSES_CFLAGS="-I$(OUT_DIR)/ncurses/include/ncursesw" \
	    NCURSES_LIBS="-L$(OUT_DIR)/ncurses/lib -lncursesw" \
	    ./configure $(PORT_CONFIGURE_ARGS) \
	        --enable-proc \
	        --disable-unicode \
	        > $(LOG_DIR)/htop.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/htop.log 2>&1
	@cp $(SRC_DIR)/htop-$(HTOP_VER)/htop $@

# ── strace — system-call tracer (upstream / GNU) ──────────────────────────────
# Installed as strace-gnu to coexist with AzamiOS's own strace.elf native tool.
# strace needs Linux UAPI headers for the struct definitions it decodes; the
# same -idirafter the BusyBox/nano builds use.
$(PORT_BIN)/strace-gnu: $(MUSL_GCC) $(SRC_DIR)/strace-$(STRACE_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  strace $(STRACE_VER)"
	@cd $(SRC_DIR)/strace-$(STRACE_VER) && \
	    CC="$(PORT_CC_UAPI)" CC_FOR_BUILD=gcc \
	    CFLAGS="$(PORT_CFLAGS) $(UAPI_INC) $(UAPI_ASM)" \
	    LDFLAGS="$(PORT_LDFLAGS)" \
	    ./configure $(PORT_CONFIGURE_ARGS) \
	        --with-libiberty=no \
	        --enable-mpers=no \
	        --enable-bundled=yes \
	        > $(LOG_DIR)/strace.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/strace.log 2>&1
	@cp $(SRC_DIR)/strace-$(STRACE_VER)/src/strace $@

# ── NASM — assembler ─────────────────────────────────────────────────────────
$(PORT_BIN)/nasm: $(MUSL_GCC) $(SRC_DIR)/nasm-$(NASM_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR) $(OUT_DIR)/nasm
	@echo "  ⚙  NASM $(NASM_VER)"
	@cd $(SRC_DIR)/nasm-$(NASM_VER) && \
	    $(PORT_CONFIGURE_ENV) ./configure $(PORT_CONFIGURE_ARGS) \
	        --prefix=$(OUT_DIR)/nasm \
	        > $(LOG_DIR)/nasm.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/nasm.log 2>&1 && \
	    $(MAKE) install >> $(LOG_DIR)/nasm.log 2>&1
	@cp $(OUT_DIR)/nasm/bin/nasm $@

# ── GNU binutils — objdump, nm, readelf, strings, strip, ar, ld, as ──────────
# We build binutils targeting x86_64-linux-musl. gas and ld are enabled.
$(PORT_BIN)/objdump: $(MUSL_GCC) $(SRC_DIR)/binutils-$(BINUTILS_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR) $(OUT_DIR)/binutils
	@echo "  ⚙  GNU binutils $(BINUTILS_VER)"
	@cd $(SRC_DIR)/binutils-$(BINUTILS_VER) && \
	    $(PORT_CONFIGURE_ENV) AR=ar RANLIB=ranlib ./configure $(PORT_CONFIGURE_ARGS) \
	        --target=x86_64-pc-linux-musl \
	        --prefix=$(OUT_DIR)/binutils \
	        --disable-gdb --disable-gdbserver \
	        --disable-sim --disable-libctf \
	        --disable-gprofng \
	        --with-system-zlib=no \
	        > $(LOG_DIR)/binutils.log 2>&1 && \
	    ln -sf $$(which ar) $(PORT_BIN)/x86_64-linux-musl-ar && \
	    ln -sf $$(which ranlib) $(PORT_BIN)/x86_64-linux-musl-ranlib && \
	    $(MAKE) -j$(NPROC) AR=ar RANLIB=ranlib PATH="$(PORT_BIN):$$PATH" >> $(LOG_DIR)/binutils.log 2>&1 && \
	    $(MAKE) install AR=ar RANLIB=ranlib PATH="$(PORT_BIN):$$PATH" >> $(LOG_DIR)/binutils.log 2>&1
	@for tool in objdump nm readelf strings strip ar addr2line size c++filt as ld ld.bfd; do \
	    src=$(OUT_DIR)/binutils/x86_64-pc-linux-musl/bin/$$tool; \
	    [ -f $$src ] || src=$(OUT_DIR)/binutils/bin/x86_64-pc-linux-musl-$$tool; \
	    [ -f $$src ] || src=$(OUT_DIR)/binutils/bin/$$tool; \
	    [ -f $$src ] && cp $$src $(PORT_BIN)/$$tool || true; \
	done
	@[ -f $(PORT_BIN)/objdump ] || echo "  !  binutils objdump not found — check log"

# ── GNU compiler collection — gcc, g++ ───────────────────────────────────────
$(PORT_BIN)/gcc: $(MUSL_GCC) $(SRC_DIR)/gcc-$(GCC_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR) $(OUT_DIR)/gcc $(OUT_DIR)/gcc-install
	@echo "  ⚙  GNU gcc $(GCC_VER)"
	@cd $(OUT_DIR)/gcc && \
	    CC="gcc" CXX="g++" CFLAGS="-O2" CXXFLAGS="-O2" LDFLAGS="-static" ../../src/gcc-$(GCC_VER)/configure \
	        --target=x86_64-pc-linux-musl \
	        --host=x86_64-pc-linux-gnu \
	        --prefix=/usr \
	        --disable-shared \
	        --disable-multilib \
	        --disable-bootstrap \
	        --disable-nls \
	        --enable-languages=c,c++ \
	        > $(LOG_DIR)/gcc.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/gcc.log 2>&1 && \
	    $(MAKE) install DESTDIR=$(OUT_DIR)/gcc-install >> $(LOG_DIR)/gcc.log 2>&1
	@for tool in gcc g++ cc1 cc1plus; do \
	    find $(OUT_DIR)/gcc-install -name "$$tool" -type f -exec cp {} $(PORT_BIN)/ \; ; \
	done
	@[ -f $(PORT_BIN)/gcc ] || echo "  !  gcc not found — check log"


# ── GNU screen — terminal multiplexer ────────────────────────────────────────
# Depends on ncurses. --enable-pam and --enable-utmp are disabled because
# pam and utmpx support is minimal in this musl sysroot.
$(PORT_BIN)/screen: $(NCURSES_LIB) $(SRC_DIR)/screen-$(SCREEN_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR) $(OUT_DIR)/screen
	@echo "  ⚙  GNU screen $(SCREEN_VER)"
	@cd $(SRC_DIR)/screen-$(SCREEN_VER) && \
	    CC="$(PORT_CC)" CC_FOR_BUILD=gcc \
	    CFLAGS="$(PORT_CFLAGS) -I$(OUT_DIR)/ncurses/include -I$(OUT_DIR)/ncurses/include/ncursesw -DTERMINFO_FALLBACK" \
	    CPPFLAGS="-I$(OUT_DIR)/ncurses/include" \
	    LDFLAGS="$(PORT_LDFLAGS) -L$(OUT_DIR)/ncurses/lib" \
	    LIBS="-lncursesw" \
	    ./configure $(PORT_CONFIGURE_ARGS) \
	        --prefix=$(OUT_DIR)/screen \
	        --disable-pam --disable-utmp \
	        > $(LOG_DIR)/screen.log 2>&1 && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/screen.log 2>&1
	@cp $(SRC_DIR)/screen-$(SCREEN_VER)/screen $@

# ── Vim — the ubiquitous text editor ─────────────────────────────────────────
# Tiny build: no GUI, no X11, no Perl/Python/Ruby interpreters, no sound,
# just the core editor. --with-tlib=ncurses picks up our static ncurses.
# Vim's configure checks for working terminals by running a test binary; with
# --host set to a musl cross triple configure knows it cannot run target
# binaries and skips all the AC_RUN_IFELSE tests cleanly.
$(PORT_BIN)/vim: $(NCURSES_LIB) $(SRC_DIR)/vim-$(VIM_VER)/.unpacked
	@mkdir -p $(PORT_BIN) $(LOG_DIR)
	@echo "  ⚙  vim $(VIM_VER)"
	@cd $(SRC_DIR)/vim-$(VIM_VER) && \
	    CC="$(PORT_CC)" CC_FOR_BUILD=gcc \
	    CFLAGS="$(PORT_CFLAGS) -I$(OUT_DIR)/ncurses/include -I$(OUT_DIR)/ncurses/include/ncursesw \
	            -DHAVE_SETENV -DHAVE_UNSETENV" \
	    LDFLAGS="$(PORT_LDFLAGS) -L$(OUT_DIR)/ncurses/lib" \
	    LIBS="-lncursesw" \
	    ./configure \
	        --host=x86_64-linux-musl \
	        --prefix=/usr \
	        --with-features=small \
	        --disable-gui --disable-gtk2-check --disable-gnome-check \
	        --disable-motif-check --disable-athena-check \
	        --disable-fontset --disable-sound --disable-acl \
	        --disable-gpm --disable-sysmouse \
	        --disable-nls \
	        --without-x \
	        --with-tlib=ncursesw \
	        --enable-multibyte \
	        --disable-python3interp --disable-pythoninterp \
	        --disable-rubyinterp --disable-perlinterp \
	        --disable-luainterp --disable-tclinterp \
	        > $(LOG_DIR)/vim.log 2>&1 && \
	    sed -i -e '/tgoto/d' -e '/tget/d' -e '/tputs/d' src/auto/osdef.h 2>/dev/null || true && \
	    $(MAKE) -j$(NPROC) >> $(LOG_DIR)/vim.log 2>&1
	@cp $(SRC_DIR)/vim-$(VIM_VER)/src/vim $@
	@cp $(SRC_DIR)/vim-$(VIM_VER)/src/vim $(PORT_BIN)/vi 2>/dev/null || true

# ── Precompiled Rust/Zig tools ───────────────────────────────────────────────
$(PORT_BIN)/ncdu: $(SRC_DIR)/ncdu-$(NCDU_VER)-linux-x86_64/.unpacked
	@mkdir -p $(PORT_BIN)
	@echo "  ⚙  ncdu $(NCDU_VER)"
	@cp $(SRC_DIR)/ncdu $@
	@chmod +x $@

$(PORT_BIN)/rg: $(SRC_DIR)/ripgrep-$(RIPGREP_VER)-x86_64-unknown-linux-musl/.unpacked
	@mkdir -p $(PORT_BIN)
	@echo "  ⚙  ripgrep $(RIPGREP_VER)"
	@cp $(SRC_DIR)/ripgrep-$(RIPGREP_VER)-x86_64-unknown-linux-musl/rg $@
	@chmod +x $@

$(PORT_BIN)/fd: $(SRC_DIR)/fd-v$(FD_VER)-x86_64-unknown-linux-musl/.unpacked
	@mkdir -p $(PORT_BIN)
	@echo "  ⚙  fd $(FD_VER)"
	@cp $(SRC_DIR)/fd-v$(FD_VER)-x86_64-unknown-linux-musl/fd $@
	@chmod +x $@

$(PORT_BIN)/bat: $(SRC_DIR)/bat-v$(BAT_VER)-x86_64-unknown-linux-musl/.unpacked
	@mkdir -p $(PORT_BIN)
	@echo "  ⚙  bat $(BAT_VER)"
	@cp $(SRC_DIR)/bat-v$(BAT_VER)-x86_64-unknown-linux-musl/bat $@
	@chmod +x $@

$(PORT_BIN)/exa: $(SRC_DIR)/exa-linux-x86_64-musl-v$(EXA_VER)/.unpacked
	@mkdir -p $(PORT_BIN)
	@echo "  ⚙  exa $(EXA_VER)"
	@cp $(SRC_DIR)/exa-linux-x86_64-musl-v$(EXA_VER)/bin/exa $@
	@chmod +x $@

$(PORT_BIN)/hexyl: $(SRC_DIR)/hexyl-v$(HEXYL_VER)-x86_64-unknown-linux-musl/.unpacked
	@mkdir -p $(PORT_BIN)
	@echo "  ⚙  hexyl $(HEXYL_VER)"
	@cp $(SRC_DIR)/hexyl-v$(HEXYL_VER)-x86_64-unknown-linux-musl/hexyl $@
	@chmod +x $@
