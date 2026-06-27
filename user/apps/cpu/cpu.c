#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/cpu.h>

void _start(void) {
    cpu_info_t cpu;
    get_cpu_info(&cpu);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "x86 Processor Topology & Capability Report:\n"
             "  Vendor ID : %s\n"
             "  Family    : %u | Model: %u | Stepping: %u\n"
             "  Features  : %s%s%s%s%s%s%s\n",
             cpu.vendor, cpu.family, cpu.model, cpu.stepping,
             cpu.has_fpu ? "[FPU] " : "",
             cpu.has_tsc ? "[TSC] " : "",
             cpu.has_msr ? "[MSR] " : "",
             cpu.has_pae ? "[PAE] " : "",
             cpu.has_apic ? "[APIC] " : "",
             cpu.has_sse ? "[SSE] " : "",
             cpu.has_sse2 ? "[SSE2] " : "");
    int fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (fd >= 0) {
        write(fd, buf, strlen(buf));
        close(fd);
    }
    exit(0);
}
