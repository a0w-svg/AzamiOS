#ifndef _UAPI_BPF_H
#define _UAPI_BPF_H

#include "../types.h"

/* BPF commands */
enum bpf_cmd {
    BPF_MAP_CREATE,
    BPF_MAP_LOOKUP_ELEM,
    BPF_MAP_UPDATE_ELEM,
    BPF_MAP_DELETE_ELEM,
    BPF_MAP_GET_NEXT_KEY,
    BPF_PROG_LOAD,
    BPF_OBJ_PIN,
    BPF_OBJ_GET,
    BPF_PROG_ATTACH,
    BPF_PROG_DETACH,
    BPF_PROG_TEST_RUN,
};

/* BPF map types */
enum bpf_map_type {
    BPF_MAP_TYPE_UNSPEC,
    BPF_MAP_TYPE_HASH,
    BPF_MAP_TYPE_ARRAY,
    BPF_MAP_TYPE_PROG_ARRAY,
    BPF_MAP_TYPE_PERF_EVENT_ARRAY,
    BPF_MAP_TYPE_PERCPU_HASH,
    BPF_MAP_TYPE_PERCPU_ARRAY,
};

/* BPF prog types */
enum bpf_prog_type {
    BPF_PROG_TYPE_UNSPEC,
    BPF_PROG_TYPE_SOCKET_FILTER,
    BPF_PROG_TYPE_KPROBE,
    BPF_PROG_TYPE_SCHED_CLS,
    BPF_PROG_TYPE_SCHED_ACT,
    BPF_PROG_TYPE_TRACEPOINT,
    BPF_PROG_TYPE_XDP,
};

/* BPF instruction definition */
struct bpf_insn {
    u8    code;       /* opcode */
    u8    dst_reg:4;  /* dest register */
    u8    src_reg:4;  /* source register */
    s16   off;        /* signed offset */
    s32   imm;        /* signed immediate constant */
};

/* Instruction classes */
#define BPF_CLASS(code) ((code) & 0x07)
#define BPF_LD    0x00
#define BPF_LDX   0x01
#define BPF_ST    0x02
#define BPF_STX   0x03
#define BPF_ALU   0x04
#define BPF_JMP   0x05
#define BPF_JMP32 0x06
#define BPF_ALU64 0x07

/* ALU operations */
#define BPF_OP(code)    ((code) & 0xf0)
#define BPF_ADD   0x00
#define BPF_SUB   0x10
#define BPF_MUL   0x20
#define BPF_DIV   0x30
#define BPF_OR    0x40
#define BPF_AND   0x50
#define BPF_LSH   0x60
#define BPF_RSH   0x70
#define BPF_NEG   0x80
#define BPF_MOD   0x90
#define BPF_XOR   0xa0
#define BPF_MOV   0xb0
#define BPF_ARSH  0xc0

/* JMP operations */
#define BPF_JA    0x00
#define BPF_JEQ   0x10
#define BPF_JGT   0x20
#define BPF_JGE   0x30
#define BPF_JSET  0x40
#define BPF_JNE   0x50
#define BPF_JSGT  0x60
#define BPF_JSGE  0x70
#define BPF_CALL  0x80
#define BPF_EXIT  0x90

/* Source operands */
#define BPF_SRC(code)   ((code) & 0x08)
#define BPF_K     0x00
#define BPF_X     0x08

/* Size modifiers */
#define BPF_SIZE(code)  ((code) & 0x18)
#define BPF_W     0x00 /* 32-bit */
#define BPF_H     0x08 /* 16-bit */
#define BPF_B     0x10 /* 8-bit */
#define BPF_DW    0x18 /* 64-bit */

/* Mode modifiers */
#define BPF_MODE(code)  ((code) & 0xe0)
#define BPF_IMM   0x00
#define BPF_ABS   0x20
#define BPF_IND   0x40
#define BPF_MEM   0x60
#define BPF_LEN   0x80
#define BPF_MSH   0xa0

/* BPF attr union for syscall */
union bpf_attr {
    struct { /* BPF_MAP_CREATE */
        u32 map_type;
        u32 key_size;
        u32 value_size;
        u32 max_entries;
        u32 map_flags;
        u32 inner_map_fd;
    };
    struct { /* BPF_MAP_UPDATE_ELEM, BPF_MAP_LOOKUP_ELEM, BPF_MAP_DELETE_ELEM */
        u32 map_fd;
        u64 key;
        union {
            u64 value;
            u64 next_key;
        };
        u64 flags;
    };
    struct { /* BPF_PROG_LOAD */
        u32 prog_type;
        u32 insn_cnt;
        u64 insns;     /* Pointer to bpf_insn array */
        u64 license;
        u32 log_level;
        u32 log_size;
        u64 log_buf;
        u32 kern_version;
        u32 prog_flags;
    };
    struct { /* BPF_PROG_TEST_RUN */
        u32 prog_fd;
        u32 retval;
        u32 data_size_in;
        u32 data_size_out;
        u64 data_in;
        u64 data_out;
        u32 repeat;
        u32 duration;
        u32 ctx_size_in;
        u32 ctx_size_out;
        u64 ctx_in;
        u64 ctx_out;
    } test;
};

#endif /* _UAPI_BPF_H */
