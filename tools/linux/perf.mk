# Optional, unmodified upstream Linux perf counting tool. No kernel build needed.
PERF_VER := 6.12
PERF_SHA256 := b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb
PERF_ARCHIVE := $(DL_DIR)/linux-$(PERF_VER).tar.xz
PERF_SRC := $(SRC_DIR)/linux-$(PERF_VER)
PERF_OUTPUT := $(OUT_DIR)/perf
PERF_BIN := $(OUT_DIR)/bin/perf
# Counting needs none of the optional recording, UI or event-database libraries.
# WERROR=0 permits compiler-version warnings in the pinned, unchanged sources.
PERF_OPTIONS := NO_LIBELF=1 NO_LIBBPF=1 NO_LIBTRACEEVENT=1 NO_LIBPFM4=1 \
 NO_JVMTI=1 NO_LIBUNWIND=1 NO_LIBBABELTRACE=1 NO_SLANG=1 NO_GTK2=1 \
 NO_LIBPYTHON=1 NO_LIBPERL=1 NO_LIBAUDIT=1 NO_LIBCRYPTO=1 NO_LIBZSTD=1 \
 NO_LIBNUMA=1 NO_LIBCAP=1 NO_ZLIB=1 NO_LZMA=1 NO_CAPSTONE=1 NO_AUXTRACE=1 \
 NO_BPF_SKEL=1 NO_LIBBFD=1 NO_DEMANGLE=1 NO_LIBDW_DWARF_UNWIND=1 \
 NO_JEVENTS=1 NO_SDT=1 NO_SYSCALL_TABLE=1 NO_PERF_READ_VDSO32=1 \
 NO_PERF_READ_VDSOX32=1 NO_AIO=1

.PHONY: perf-static perf-stat-probe
perf-static: $(PERF_BIN)
perf-stat-probe: $(OUT_DIR)/bin/azami-perf-stat-probe

$(PERF_ARCHIVE):
	@mkdir -p $(DL_DIR)
	curl -fL https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$(PERF_VER).tar.xz -o $@.tmp
	@echo '$(PERF_SHA256)  $@.tmp' | sha256sum -c -
	mv $@.tmp $@

$(PERF_SRC)/.perf-unpacked: $(PERF_ARCHIVE)
	@echo '$(PERF_SHA256)  $(PERF_ARCHIVE)' | sha256sum -c -
	@mkdir -p $(SRC_DIR)
	tar -xJf $< -C $(SRC_DIR) linux-$(PERF_VER)/tools linux-$(PERF_VER)/include \
	 linux-$(PERF_VER)/scripts linux-$(PERF_VER)/arch linux-$(PERF_VER)/Makefile \
	 linux-$(PERF_VER)/Kbuild linux-$(PERF_VER)/Kconfig
	@touch $@

$(PERF_BIN): $(PERF_SRC)/.perf-unpacked $(MUSL_GCC) perf.mk
	@mkdir -p $(PERF_OUTPUT) $(dir $@) $(LOG_DIR)
	$(MAKE) -C $(PERF_SRC)/tools/perf JOBS=$(NPROC) OUTPUT=$(PERF_OUTPUT)/ \
	 CC=$(MUSL_GCC) HOSTCC=gcc EXTRA_CFLAGS='$(UAPI_INC) $(UAPI_ASM)' \
	 LDFLAGS=-static WERROR=0 $(PERF_OPTIONS) >$(LOG_DIR)/perf.log 2>&1
	@readelf -l -d $(PERF_OUTPUT)/perf > $(PERF_OUTPUT)/elf-check.txt
	@awk '/INTERP|\(NEEDED\)/ { bad=1 } END { exit bad }' $(PERF_OUTPUT)/elf-check.txt
	cp $(PERF_OUTPUT)/perf $@

$(OUT_DIR)/bin/azami-perf-stat-probe: tests/perf-stat.c tests/perf-stat-csv.h $(MUSL_GCC)
	@mkdir -p $(dir $@)
	$(BB_CC) -static -std=c11 -O2 -g -Wall -Wextra -Werror -o $@ $<

# Optional staging, kept out of default images like the other Linux ports.
.PHONY: perf-install
perf-install: $(PERF_BIN)
	@mkdir -p $(DESTDIR)/usr/bin
	cp $(PERF_BIN) $(DESTDIR)/usr/bin/perf
