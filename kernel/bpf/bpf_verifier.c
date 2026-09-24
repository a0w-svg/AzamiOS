#include "bpf_internal.h"
#include "../mm/kmalloc.h"

int bpf_check(bpf_prog_t *prog)
{
    if (prog->insn_cnt == 0 || prog->insn_cnt > 4096)
        return -(s64)E2BIG;
        
    /* Basic pass: ensure jump offsets are within bounds and instructions are known */
    for (u32 i = 0; i < prog->insn_cnt; i++) {
        struct bpf_insn *insn = &prog->insns[i];
        u8 class = BPF_CLASS(insn->code);
        
        if (class == BPF_ALU || class == BPF_ALU64) {
            /* Valid ALU operation? */
            u8 op = BPF_OP(insn->code);
            if (op > BPF_ARSH) return -(s64)EINVAL;
        } else if (class == BPF_JMP || class == BPF_JMP32) {
            u8 op = BPF_OP(insn->code);
            if (op == BPF_EXIT) {
                if (i != prog->insn_cnt - 1) {
                    /* Not strictly required to be at the end, but safe for our basic verifier */
                }
            } else if (op == BPF_CALL) {
                /* Valid helper ID? We only support basic ones for now */
            } else {
                /* Branch instruction: check bounds */
                s16 off = insn->off;
                if (off < 0 && (u32)(-off) > i) return -(s64)EINVAL; /* jump before start */
                if (off > 0 && i + off + 1 >= prog->insn_cnt) return -(s64)EINVAL; /* jump out of bounds */
            }
        } else if (class == BPF_LD || class == BPF_LDX || class == BPF_ST || class == BPF_STX) {
            /* Valid memory instructions */
        } else {
            return -(s64)EINVAL; /* Unknown class */
        }
    }
    
    return 0;
}
