# ==============================================================================
# AzamiOS — toolchain selection, shared by every Makefile in the tree
#
# Included by the top-level Makefile, userland/Makefile, userland/libc/Makefile
# and userland/libgame/Makefile, so all four always agree on one compiler.
#
# CROSS_PREFIX is chosen in this order, first match wins:
#
#   1. whatever the user set (command line or environment), even if empty;
#   2. the dedicated cross-compiler scripts/build_toolchain.sh installs,
#      $(HOME)/opt/cross-x86_64/bin/x86_64-elf-gcc;
#   3. an x86_64-elf-gcc anywhere on PATH (distribution cross packages);
#   4. the host gcc, when the host itself is x86_64;
#   5. x86_64-linux-gnu-gcc, the cross compiler Debian/Ubuntu/Fedora ship for
#      building x86_64 code on other architectures.
#
# Why a hosted gcc is acceptable at all: every AzamiOS object is compiled
# -ffreestanding -fno-pie and linked with a raw `ld -nostdlib` against our own
# linker scripts, so nothing of the host C library or its startup files ever
# reaches a binary. What does leak are three defaults distributions bake into
# their gcc, each switched off below:
#
#   -fcf-protection      Fedora and Ubuntu default to =full, which puts an
#                        ENDBR64 at every function entry. Harmless today (the
#                        kernel never enables CET-IBT, see arch/x86_64/cpu/msr.h)
#                        but it is code the x86_64-elf build does not have.
#   -fstack-clash-protection
#                        Ubuntu's default. Emits probes of every stack page on
#                        large frames — pointless in a kernel whose stacks are
#                        fixed-size and guard-paged, and a behavioural
#                        difference between two builds of the same source.
#   the system include path
#                        A hosted gcc searches /usr/include after our -I dirs,
#                        so a header our libc lacks would silently resolve to
#                        glibc's and compile against the wrong ABI. -nostdinc
#                        plus gcc's own freestanding include directory
#                        (stdarg.h, stddef.h, stdint-gcc.h, cpuid.h, …) is
#                        exactly the search path a bare x86_64-elf-gcc has.
#
# `make doctor` prints what was selected and why.
# ==============================================================================

ifndef AZAMI_TOOLCHAIN_MK
AZAMI_TOOLCHAIN_MK := 1

AZAMI_CROSS_HOME := $(HOME)/opt/cross-x86_64/bin/x86_64-elf-
HOST_ARCH        := $(shell uname -m)

have-cmd = $(shell command -v $(1) >/dev/null 2>&1 && echo y)

ifneq ($(origin CROSS_PREFIX),undefined)
  TOOLCHAIN_SOURCE := set by $(origin CROSS_PREFIX)
else ifneq ($(wildcard $(AZAMI_CROSS_HOME)gcc),)
  CROSS_PREFIX     := $(AZAMI_CROSS_HOME)
  TOOLCHAIN_SOURCE := dedicated cross-compiler in ~/opt/cross-x86_64
else ifeq ($(call have-cmd,x86_64-elf-gcc),y)
  CROSS_PREFIX     := x86_64-elf-
  TOOLCHAIN_SOURCE := x86_64-elf-gcc on PATH
else ifeq ($(HOST_ARCH)$(call have-cmd,gcc),x86_64y)
  CROSS_PREFIX     :=
  TOOLCHAIN_SOURCE := host gcc (x86_64 host)
else ifeq ($(call have-cmd,x86_64-linux-gnu-gcc),y)
  CROSS_PREFIX     := x86_64-linux-gnu-
  TOOLCHAIN_SOURCE := x86_64-linux-gnu-gcc on PATH
else
  CROSS_PREFIX     := $(AZAMI_CROSS_HOME)
  TOOLCHAIN_SOURCE := none found
  TOOLCHAIN_MISSING := 1
endif
export CROSS_PREFIX

# Goals that must work on a machine with no compiler at all.
TOOLCHAIN_FREE_GOALS := clean linux-clean doctor help distclean
ifeq ($(TOOLCHAIN_MISSING),1)
  ifneq ($(filter-out $(TOOLCHAIN_FREE_GOALS),$(or $(MAKECMDGOALS),all)),)
    $(error No x86_64 C compiler found. Either run the build in the bundled \
container (scripts/devenv.sh make), install gcc for an x86_64 host, or build \
the dedicated cross-compiler with scripts/build_toolchain.sh. `make doctor` \
lists everything the build needs)
  endif
endif

# The flags below are computed once here and exported, so recursive makes
# neither re-probe the compiler nor risk reaching a different answer.
ifndef TOOLCHAIN_CFLAGS
  TC_GCC := $(CROSS_PREFIX)gcc
  cc-option = $(shell $(TC_GCC) $(1) -Werror -E -x c /dev/null -o /dev/null \
                  >/dev/null 2>&1 && echo $(1))
  TC_INCDIR := $(shell $(TC_GCC) -print-file-name=include 2>/dev/null)
  TOOLCHAIN_CFLAGS := $(call cc-option,-fcf-protection=none) \
                      $(call cc-option,-fno-stack-clash-protection) \
                      -U_FORTIFY_SOURCE
  ifneq ($(wildcard $(TC_INCDIR)/stdarg.h),)
    TOOLCHAIN_CFLAGS += -nostdinc -isystem $(TC_INCDIR)
  endif
  TOOLCHAIN_CFLAGS := $(strip $(TOOLCHAIN_CFLAGS))
  export TOOLCHAIN_CFLAGS
endif

endif # AZAMI_TOOLCHAIN_MK
