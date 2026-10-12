#!/usr/bin/env python3
"""
AzamiOS — Golden Struct Layout Generator
File: scripts/generate_layouts.py

Queries host GCC against Linux 6.6 UAPI headers to extract exact sizes and
member offsets for user-visible structs on x86-64, generating tests/compat/layouts.h.
"""

import subprocess
import os
import sys

STRUCTS_CONFIG = [
    # (Type in C probe, Type name in macro, [members to check])
    ("struct stat", "STAT", [
        "st_dev", "st_ino", "st_nlink", "st_mode", "st_uid", "st_gid",
        "st_rdev", "st_size", "st_blksize", "st_blocks",
        "st_atim", "st_mtim", "st_ctim"
    ]),
    ("struct statx", "STATX", [
        "stx_mask", "stx_blksize", "stx_attributes", "stx_nlink",
        "stx_uid", "stx_gid", "stx_mode", "stx_ino", "stx_size",
        "stx_blocks", "stx_attributes_mask", "stx_atime", "stx_btime",
        "stx_ctime", "stx_mtime", "stx_rdev_major", "stx_rdev_minor"
    ]),
    ("struct statx_timestamp", "STATX_TIMESTAMP", [
        "tv_sec", "tv_nsec"
    ]),
    ("struct timespec", "TIMESPEC", [
        "tv_sec", "tv_nsec"
    ]),
    ("struct itimerspec", "ITIMERSPEC", [
        "it_interval", "it_value"
    ]),
    ("struct timeval", "TIMEVAL", [
        "tv_sec", "tv_usec"
    ]),
    ("struct itimerval", "ITIMERVAL", [
        "it_interval", "it_value"
    ]),
    ("struct rlimit", "RLIMIT", [
        "rlim_cur", "rlim_max"
    ]),
    ("struct rlimit64", "RLIMIT64", [
        "rlim_cur", "rlim_max"
    ]),
    ("struct rusage", "RUSAGE", [
        "ru_utime", "ru_stime", "ru_maxrss", "ru_ixrss", "ru_idrss",
        "ru_isrss", "ru_minflt", "ru_majflt", "ru_nswap", "ru_inblock",
        "ru_oublock", "ru_msgsnd", "ru_msgrcv", "ru_nsignals",
        "ru_nvcsw", "ru_nivcsw"
    ]),
    ("struct sysinfo", "SYSINFO", [
        "uptime", "loads", "totalram", "freeram", "sharedram", "bufferram",
        "totalswap", "freeswap", "procs", "totalhigh", "freehigh", "mem_unit"
    ]),
    ("struct utsname", "UTSNAME", [
        "sysname", "nodename", "release", "version", "machine", "domainname"
    ]),
    ("struct epoll_event", "EPOLL_EVENT", [
        "events", "data"
    ]),
    ("struct iovec", "IOVEC", [
        "iov_base", "iov_len"
    ]),
    ("struct msghdr", "MSGHDR", [
        "msg_name", "msg_namelen", "msg_iov", "msg_iovlen",
        "msg_control", "msg_controllen", "msg_flags"
    ]),
    ("struct cmsghdr", "CMSGHDR", [
        "cmsg_len", "cmsg_level", "cmsg_type"
    ]),
    ("struct sockaddr", "SOCKADDR", [
        "sa_family", "sa_data"
    ]),
    ("struct sockaddr_in", "SOCKADDR_IN", [
        "sin_family", "sin_port", "sin_addr"
    ]),
    ("struct sockaddr_in6", "SOCKADDR_IN6", [
        "sin6_family", "sin6_port", "sin6_flowinfo", "sin6_addr", "sin6_scope_id"
    ]),
    ("struct sockaddr_un", "SOCKADDR_UN", [
        "sun_family", "sun_path"
    ]),
    ("struct sockaddr_nl", "SOCKADDR_NL", [
        "nl_family", "nl_pad", "nl_pid", "nl_groups"
    ]),
    ("struct dirent", "DIRENT", [
        "d_ino", "d_off", "d_reclen", "d_type", "d_name"
    ]),
    ("struct flock", "FLOCK", [
        "l_type", "l_whence", "l_start", "l_len", "l_pid"
    ]),
    ("struct flock64", "FLOCK64", [
        "l_type", "l_whence", "l_start", "l_len", "l_pid"
    ]),
    ("struct statfs", "STATFS", [
        "f_type", "f_bsize", "f_blocks", "f_bfree", "f_bavail",
        "f_files", "f_ffree", "f_fsid", "f_namelen", "f_frsize",
        "f_flags", "f_spare"
    ]),
    ("struct kernel_termios", "TERMIOS", [
        "c_iflag", "c_oflag", "c_cflag", "c_lflag", "c_line", "c_cc"
    ]),
    ("struct kernel_termios2", "TERMIOS2", [
        "c_iflag", "c_oflag", "c_cflag", "c_lflag", "c_line", "c_cc",
        "c_ispeed", "c_ospeed"
    ]),
    ("struct kernel_winsize", "WINSIZE", [
        "ws_row", "ws_col", "ws_xpixel", "ws_ypixel"
    ]),
    ("struct ifreq", "IFREQ", [
        "ifr_name"
    ]),
    ("struct perf_event_attr", "PERF_EVENT_ATTR", [
        "type", "size", "config", "sample_period", "sample_type",
        "read_format"
    ]),
    ("struct clone_args", "CLONE_ARGS", [
        "flags", "pidfd", "child_tid", "parent_tid", "exit_signal",
        "stack", "stack_size", "tls", "set_tid", "set_tid_size",
        "cgroup"
    ]),
    ("struct open_how", "OPEN_HOW", [
        "flags", "mode", "resolve"
    ]),
    ("struct sched_attr", "SCHED_ATTR", [
        "size", "sched_policy", "sched_flags", "sched_nice", "sched_priority",
        "sched_runtime", "sched_deadline", "sched_period"
    ]),
    ("struct rseq", "RSEQ", [
        "cpu_id_start", "cpu_id", "rseq_cs", "flags"
    ]),
    ("struct seccomp_data", "SECCOMP_DATA", [
        "nr", "arch", "instruction_pointer", "args"
    ]),
    ("struct user_regs_struct", "USER_REGS_STRUCT", [
        "r15", "r14", "r13", "r12", "rbp", "rbx", "r11", "r10",
        "r9", "r8", "rax", "rcx", "rdx", "rsi", "rdi",
        "orig_rax", "rip", "cs", "eflags", "rsp", "ss"
    ]),
    ("struct user_fpregs_struct", "USER_FPREGS_STRUCT", [
        "cwd", "swd", "ftw", "fop", "rip", "rdp", "mxcsr", "mxcr_mask",
        "st_space", "xmm_space"
    ]),
    ("struct msqid64_ds", "MSQID64_DS", [
        "msg_perm", "msg_stime", "msg_rtime", "msg_ctime",
        "msg_cbytes", "msg_qnum", "msg_qbytes", "msg_lspid", "msg_lrpid"
    ]),
    ("struct semid64_ds", "SEMID64_DS", [
        "sem_perm", "sem_otime", "sem_ctime", "sem_nsems"
    ]),
    ("struct shmid64_ds", "SHMID64_DS", [
        "shm_perm", "shm_segsz", "shm_atime", "shm_dtime", "shm_ctime",
        "shm_cpid", "shm_lpid", "shm_nattch"
    ]),
    ("struct io_uring_sqe", "IO_URING_SQE", [
        "opcode", "flags", "ioprio", "fd", "off", "addr", "len",
        "user_data"
    ]),
    ("struct io_uring_cqe", "IO_URING_CQE", [
        "user_data", "res", "flags"
    ]),
    ("struct io_uring_params", "IO_URING_PARAMS", [
        "sq_entries", "cq_entries", "flags", "sq_thread_cpu",
        "sq_thread_idle", "features", "wq_fd", "sq_off", "cq_off"
    ])
]

PROBE_C_TEMPLATE = """#define _GNU_SOURCE
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/vfs.h>
#include <sys/user.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <linux/netlink.h>
#include <linux/if.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <linux/types.h>
#include <linux/stat.h>
#include <linux/perf_event.h>
#include <linux/seccomp.h>
#include <linux/rseq.h>
#include <linux/openat2.h>
#include <linux/sched.h>
#include <linux/sched/types.h>
#include <linux/io_uring.h>
#include <asm/sembuf.h>
#include <asm/shmbuf.h>
#include <asm/msgbuf.h>

struct kernel_termios {
    unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line;
    unsigned char c_cc[19];
};
struct kernel_termios2 {
    unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line;
    unsigned char c_cc[19];
    unsigned int c_ispeed, c_ospeed;
};
struct kernel_winsize {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};

int main(void) {
__BODY__
    return 0;
}
"""

def main():
    body_lines = []
    for c_type, tag, members in STRUCTS_CONFIG:
        body_lines.append(f'    printf("SIZE|{tag}|%zu\\n", sizeof({c_type}));')
        for m in members:
            body_lines.append(f'    printf("OFF|{tag}|{m.upper()}|%zu\\n", offsetof({c_type}, {m}));')

    probe_code = PROBE_C_TEMPLATE.replace("__BODY__", "\n".join(body_lines))

    proc = subprocess.run(["gcc", "-O2", "-x", "c", "-", "-o", "/tmp/probe_layouts"],
                          input=probe_code, text=True, capture_output=True)
    if proc.returncode != 0:
        print("GCC compilation failed:", file=sys.stderr)
        print(proc.stderr, file=sys.stderr)
        sys.exit(1)

    probe_out = subprocess.run(["/tmp/probe_layouts"], capture_output=True, text=True).stdout

    data = {}
    for line in probe_out.strip().splitlines():
        parts = line.split("|")
        if parts[0] == "SIZE":
            data[f"LINUX_SIZEOF_{parts[1]}"] = int(parts[2])
        elif parts[0] == "OFF":
            data[f"LINUX_OFFSETOF_{parts[1]}_{parts[2]}"] = int(parts[3])

    out_header = []
    out_header.append("/* SPDX-License-Identifier: MIT */")
    out_header.append("/* ============================================================================")
    out_header.append(" * AzamiOS — Golden Linux 6.6 UAPI Struct Layouts and ABI Assertions")
    out_header.append(" * File: tests/compat/layouts.h")
    out_header.append(" * Generated from host Linux 6.6 x86-64 UAPI headers.")
    out_header.append(" * ============================================================================ */")
    out_header.append("#pragma once")
    out_header.append("")
    out_header.append("#include <stddef.h>")
    out_header.append("#include <stdint.h>")
    out_header.append("")
    out_header.append("/* ── Golden Sizes and Offsets (Linux 6.6 x86-64) ────────────────────────── */")

    for k, v in data.items():
        out_header.append(f"#define {k:<45} {v}")

    out_header.append("")
    out_header.append("/* ── Convenience Static Assertion Macros ────────────────────────────────── */")
    out_header.append("#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L")
    out_header.append("#  define AZAMI_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)")
    out_header.append("#elif defined(__cplusplus) && __cplusplus >= 201103L")
    out_header.append("#  define AZAMI_STATIC_ASSERT(cond, msg) static_assert(cond, msg)")
    out_header.append("#else")
    out_header.append("#  define AZAMI_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)")
    out_header.append("#endif")
    out_header.append("")
    out_header.append("#define ASSERT_LAYOUT_SIZE(type, expected_size) \\")
    out_header.append("    AZAMI_STATIC_ASSERT(sizeof(type) == (expected_size), #type \" size mismatch with Linux ABI\")")
    out_header.append("")
    out_header.append("#define ASSERT_LAYOUT_OFFSET(type, member, expected_offset) \\")
    out_header.append("    AZAMI_STATIC_ASSERT(offsetof(type, member) == (expected_offset), #type \".\" #member \" offset mismatch with Linux ABI\")")
    out_header.append("")

    target_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tests", "compat", "layouts.h")
    os.makedirs(os.path.dirname(target_path), exist_ok=True)
    with open(target_path, "w", encoding="utf-8") as f:
        f.write("\n".join(out_header) + "\n")

    print(f"Generated {target_path} successfully ({len(data)} constants).")

if __name__ == "__main__":
    main()

