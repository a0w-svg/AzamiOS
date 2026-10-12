include ../../mk/toolchain.mk

TASK_BUILD := ../../build/smoke
MUSL_CC := ../../tools/linux/musl/bin/musl-gcc
NATIVE_CC := $(CROSS_PREFIX)gcc
NATIVE_LD := $(CROSS_PREFIX)ld
HOST_CC ?= cc
NATIVE_FLAGS := -std=c11 -ffreestanding -fno-stack-protector -fpic -fno-plt \
    -ftls-model=initial-exec -O2 -Wall -Wextra -Werror -MMD -MP \
    -I../../userland/libc/include $(TOOLCHAIN_CFLAGS)
NATIVE_LINK := -T ../../scripts/user-pic.ld -nostdlib -z max-page-size=0x1000

.PHONY: all check
check: $(TASK_BUILD)/hash-count $(TASK_BUILD)/check-layouts $(TASK_BUILD)/extensible-structs $(TASK_BUILD)/sigframe-test
	$(TASK_BUILD)/hash-count
	$(TASK_BUILD)/check-layouts
	$(TASK_BUILD)/extensible-structs
	$(TASK_BUILD)/sigframe-test

$(TASK_BUILD)/hash-count: hash_count.c ../../userland/libc/include/internal/elf_hash.h | $(TASK_BUILD)
	$(HOST_CC) -O2 -Wall -Wextra -Werror $< -o $@

$(TASK_BUILD)/check-layouts: ../compat/check_layouts.c ../compat/layouts.h | $(TASK_BUILD)
	$(HOST_CC) -O2 -Wall -Wextra -Werror -I../compat $< -o $@

$(TASK_BUILD)/extensible-structs: ../compat/extensible_structs.c | $(TASK_BUILD)
	$(HOST_CC) -O2 -Wall -Wextra -Werror $< -o $@

$(TASK_BUILD)/sigframe-test: ../compat/sigframe_test.c | $(TASK_BUILD)
	$(HOST_CC) -O2 -Wall -Wextra -Werror $< -o $@

all: $(TASK_BUILD)/smoke-init $(TASK_BUILD)/ports-init $(TASK_BUILD)/diff-init $(TASK_BUILD)/check-layouts $(TASK_BUILD)/native-dynamic-probe \
     $(TASK_BUILD)/musl-dynamic-probe $(TASK_BUILD)/native/libsmoke_plugin.so \
     $(TASK_BUILD)/musl/libsmoke_plugin.so $(TASK_BUILD)/ld-azami.so \
     $(TASK_BUILD)/native/libsmoke_versioned.so $(TASK_BUILD)/native/libsmoke_consumer_v1.so \
     $(TASK_BUILD)/native/libsmoke_consumer_v2.so $(TASK_BUILD)/musl/libsmoke_versioned.so \
     $(TASK_BUILD)/musl/libsmoke_consumer_v1.so $(TASK_BUILD)/musl/libsmoke_consumer_v2.so

$(TASK_BUILD) $(TASK_BUILD)/native $(TASK_BUILD)/musl:
	mkdir -p $@

$(TASK_BUILD)/smoke-init: init.c | $(TASK_BUILD)
	$(MUSL_CC) -static -O2 -Wall -Wextra -Werror $< -o $@

$(TASK_BUILD)/ports-init: ports_init.c | $(TASK_BUILD)
	$(MUSL_CC) -static -O2 -Wall -Wextra -Werror $< -o $@

$(TASK_BUILD)/diff-init: diff_init.c | $(TASK_BUILD)
	$(MUSL_CC) -static -O2 -Wall -Wextra -Werror $< -o $@

$(TASK_BUILD)/native/%.o: %.c | $(TASK_BUILD)/native
	$(NATIVE_CC) $(NATIVE_FLAGS) -c $< -o $@

$(TASK_BUILD)/native/libsmoke_start.so: $(TASK_BUILD)/native/startup.o Makefile
	$(NATIVE_LD) -shared $(NATIVE_LINK) --hash-style=gnu --soname=libsmoke_start.so $< -o $@

$(TASK_BUILD)/native/libsmoke_plugin.so: $(TASK_BUILD)/native/plugin.o $(TASK_BUILD)/native/libsmoke_start.so
	$(NATIVE_LD) -shared $(NATIVE_LINK) --hash-style=gnu --soname=libsmoke_plugin.so $^ -o $@

$(TASK_BUILD)/native/libsmoke_versioned.so: $(TASK_BUILD)/native/versioned_lib.o versioned_lib.map
	$(NATIVE_LD) -shared $(NATIVE_LINK) --hash-style=gnu --version-script=versioned_lib.map --soname=libsmoke_versioned.so $< -o $@

$(TASK_BUILD)/native/libsmoke_consumer_v1.so: $(TASK_BUILD)/native/consumer_v1.o $(TASK_BUILD)/native/libsmoke_versioned.so
	$(NATIVE_LD) -shared $(NATIVE_LINK) --hash-style=gnu --soname=libsmoke_consumer_v1.so $^ -o $@

$(TASK_BUILD)/native/libsmoke_consumer_v2.so: $(TASK_BUILD)/native/consumer_v2.o $(TASK_BUILD)/native/libsmoke_versioned.so
	$(NATIVE_LD) -shared $(NATIVE_LINK) --hash-style=gnu --soname=libsmoke_consumer_v2.so $^ -o $@

$(TASK_BUILD)/native-dynamic-probe: $(TASK_BUILD)/native/dynamic_probe.o $(TASK_BUILD)/native/libsmoke_start.so ../../userland/libc/libc.so ../../userland/libc/crt0_dyn.o
	$(NATIVE_LD) -pie $(NATIVE_LINK) --dynamic-linker=/lib/ld-azami.so \
	    ../../userland/libc/crt0_dyn.o $(filter-out ../../userland/libc/crt0_dyn.o,$^) -o $@

$(TASK_BUILD)/musl/libsmoke_start.so: startup.c | $(TASK_BUILD)/musl
	$(MUSL_CC) -shared -fPIC -ftls-model=initial-exec -O2 -Wall -Wextra -Werror \
	    -Wl,-soname,libsmoke_start.so $< -o $@

$(TASK_BUILD)/musl/libsmoke_plugin.so: plugin.c $(TASK_BUILD)/musl/libsmoke_start.so
	$(MUSL_CC) -shared -fPIC -O2 -Wall -Wextra -Werror \
	    -Wl,-soname,libsmoke_plugin.so $< -L$(TASK_BUILD)/musl -lsmoke_start -o $@

$(TASK_BUILD)/musl/libsmoke_versioned.so: versioned_lib.c versioned_lib.map | $(TASK_BUILD)/musl
	$(MUSL_CC) -shared -fPIC -O2 -Wall -Wextra -Werror \
	    -Wl,--version-script=versioned_lib.map -Wl,-soname,libsmoke_versioned.so $< -o $@

$(TASK_BUILD)/musl/libsmoke_consumer_v1.so: consumer_v1.c $(TASK_BUILD)/musl/libsmoke_versioned.so
	$(MUSL_CC) -shared -fPIC -O2 -Wall -Wextra -Werror \
	    -Wl,-soname,libsmoke_consumer_v1.so $< -L$(TASK_BUILD)/musl -lsmoke_versioned -o $@

$(TASK_BUILD)/musl/libsmoke_consumer_v2.so: consumer_v2.c $(TASK_BUILD)/musl/libsmoke_versioned.so
	$(MUSL_CC) -shared -fPIC -O2 -Wall -Wextra -Werror \
	    -Wl,-soname,libsmoke_consumer_v2.so $< -L$(TASK_BUILD)/musl -lsmoke_versioned -o $@

$(TASK_BUILD)/musl-dynamic-probe: dynamic_probe.c $(TASK_BUILD)/musl/libsmoke_start.so | $(TASK_BUILD)
	$(MUSL_CC) -fPIE -pie -O2 -Wall -Wextra -Werror $< \
	    -L$(TASK_BUILD)/musl -lsmoke_start -ldl -o $@

$(TASK_BUILD)/native/ldso.o: ../../userland/ldso/ldso.c ../../userland/libc/include/sys/syscall.h ../../userland/libc/include/internal/elf_hash.h | $(TASK_BUILD)/native
	$(NATIVE_CC) $(NATIVE_FLAGS) -c $< -o $@

$(TASK_BUILD)/native/crt0.o: ../../userland/ldso/crt0.asm | $(TASK_BUILD)/native
	nasm -f elf64 $< -o $@

$(TASK_BUILD)/ld-azami.so: $(TASK_BUILD)/native/crt0.o $(TASK_BUILD)/native/ldso.o
	$(NATIVE_LD) -shared $(NATIVE_LINK) $^ -o $@

-include $(wildcard $(TASK_BUILD)/native/*.d)
