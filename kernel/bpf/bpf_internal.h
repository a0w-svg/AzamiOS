#ifndef _BPF_INTERNAL_H
#define _BPF_INTERNAL_H

#include "../../fs/vfs.h"
#include "../../include/azami/uapi/bpf.h"
#include "../../include/azami/uapi/errno.h"

typedef struct bpf_prog {
    u32 prog_type;
    u32 insn_cnt;
    struct bpf_insn *insns;
    void *bpf_func; /* Pointer to JIT'ed code */
} bpf_prog_t;

/* Verifier */
int bpf_check(bpf_prog_t *prog);

/* JIT Compiler */
int bpf_int_jit_compile(bpf_prog_t *prog);

/* Maps */
int bpf_map_create(union bpf_attr *attr, file_t **out_file);
int bpf_map_lookup_elem(file_t *filp, void *key, void *value);
int bpf_map_update_elem(file_t *filp, void *key, void *value, u64 flags);

#endif /* _BPF_INTERNAL_H */
