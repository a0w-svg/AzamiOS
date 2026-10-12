MUSL_CC ?= ../../tools/linux/musl/bin/musl-gcc
BENCH_CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror
BENCH_LDFLAGS ?= -static
TASK_BUILD := ../../build/bench/bin
PROGRAMS := bench_syscall bench_pipe bench_mmap bench_fork bench_fd bench_pmm bench_usercopy bench_malloc supervisor

.PHONY: all check FORCE
all: $(addprefix $(TASK_BUILD)/,$(PROGRAMS))

$(TASK_BUILD):
	mkdir -p $@

# Make rebuilds on flag/compiler changes as well as source changes. Preserve
# the stamp's mtime when the configuration is unchanged.
$(TASK_BUILD)/config.json: FORCE | $(TASK_BUILD)
	python3 build_info.py config $@ "$(MUSL_CC)" "$(BENCH_CFLAGS)" "$(BENCH_LDFLAGS)"

$(TASK_BUILD)/bench_malloc: bench_malloc.c common.h $(TASK_BUILD)/config.json build_info.py
	$(MUSL_CC) $(BENCH_CFLAGS) $(BENCH_LDFLAGS) -pthread $< -o $@
	python3 build_info.py payload $@ $< common.h $(TASK_BUILD)/config.json

$(TASK_BUILD)/%: %.c common.h $(TASK_BUILD)/config.json build_info.py
	$(MUSL_CC) $(BENCH_CFLAGS) $(BENCH_LDFLAGS) $< -o $@
	python3 build_info.py payload $@ $< common.h $(TASK_BUILD)/config.json

$(TASK_BUILD)/bench_fd: bench_fd.c $(TASK_BUILD)/config.json build_info.py
	$(MUSL_CC) $(BENCH_CFLAGS) $(BENCH_LDFLAGS) -pthread $< -o $@
	python3 build_info.py payload $@ $< bench_fd.c $(TASK_BUILD)/config.json

check: all
	python3 -B test_runner.py
