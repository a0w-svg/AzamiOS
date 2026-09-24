#include "bpf_internal.h"
#include "../mm/kmalloc.h"
#include "../mm/kmodmem.h"
#include "../mm/pmm.h"

static const u8 reg2hex[] = {
    [0] = 0,  /* RAX */
    [1] = 7,  /* RDI */
    [2] = 6,  /* RSI */
    [3] = 2,  /* RDX */
    [4] = 1,  /* RCX */
    [5] = 8,  /* R8  */
    [6] = 3,  /* RBX */
    [7] = 13, /* R13 */
    [8] = 14, /* R14 */
    [9] = 15, /* R15 */
    [10] = 5, /* RBP */
};

#define EMIT1(b1) do { \
    if (image) image[proglen] = b1; \
    proglen++; \
} while(0)

#define EMIT2(b1, b2) do { \
    if (image) { image[proglen] = b1; image[proglen+1] = b2; } \
    proglen += 2; \
} while(0)

#define EMIT3(b1, b2, b3) do { \
    if (image) { image[proglen] = b1; image[proglen+1] = b2; image[proglen+2] = b3; } \
    proglen += 3; \
} while(0)

#define EMIT4(b1, b2, b3, b4) do { \
    if (image) { image[proglen] = b1; image[proglen+1] = b2; image[proglen+2] = b3; image[proglen+3] = b4; } \
    proglen += 4; \
} while(0)

static inline void emit_modrm(u8 *image, u32 *proglen_ptr, u8 mod, u8 reg, u8 rm) {
    u32 proglen = *proglen_ptr;
    EMIT1((mod << 6) | ((reg & 7) << 3) | (rm & 7));
    *proglen_ptr = proglen;
}

static inline void emit_sib(u8 *image, u32 *proglen_ptr, u8 scale, u8 index, u8 base) {
    u32 proglen = *proglen_ptr;
    EMIT1((scale << 6) | ((index & 7) << 3) | (base & 7));
    *proglen_ptr = proglen;
}

static inline void emit_imm32(u8 *image, u32 *proglen_ptr, s32 imm) {
    u32 proglen = *proglen_ptr;
    if (image) {
        *(s32 *)&image[proglen] = imm;
    }
    proglen += 4;
    *proglen_ptr = proglen;
}

static inline void emit_imm64(u8 *image, u32 *proglen_ptr, s64 imm) {
    u32 proglen = *proglen_ptr;
    if (image) {
        *(s64 *)&image[proglen] = imm;
    }
    proglen += 8;
    *proglen_ptr = proglen;
}

static inline void emit_rex(u8 *image, u32 *proglen_ptr, u8 w, u8 r, u8 x, u8 b) {
    u32 proglen = *proglen_ptr;
    u8 rex = 0x40 | (w ? 8 : 0) | ((r > 7) ? 4 : 0) | ((x > 7) ? 2 : 0) | ((b > 7) ? 1 : 0);
    EMIT1(rex);
    *proglen_ptr = proglen;
}

static void build_prologue(u8 *image, u32 *proglen_ptr) {
    u32 proglen = *proglen_ptr;
    /* push rbp */
    EMIT1(0x55);
    /* mov rbp, rsp */
    emit_rex(image, &proglen, 1, 0, 0, 0); EMIT2(0x89, 0xE5);
    /* push rbx */
    EMIT1(0x53);
    /* push r13 */
    emit_rex(image, &proglen, 0, 0, 0, 13); EMIT1(0x50 | (13 & 7));
    /* push r14 */
    emit_rex(image, &proglen, 0, 0, 0, 14); EMIT1(0x50 | (14 & 7));
    /* push r15 */
    emit_rex(image, &proglen, 0, 0, 0, 15); EMIT1(0x50 | (15 & 7));
    *proglen_ptr = proglen;
}

static void build_epilogue(u8 *image, u32 *proglen_ptr) {
    u32 proglen = *proglen_ptr;
    /* pop r15 */
    emit_rex(image, &proglen, 0, 0, 0, 15); EMIT1(0x58 | (15 & 7));
    /* pop r14 */
    emit_rex(image, &proglen, 0, 0, 0, 14); EMIT1(0x58 | (14 & 7));
    /* pop r13 */
    emit_rex(image, &proglen, 0, 0, 0, 13); EMIT1(0x58 | (13 & 7));
    /* pop rbx */
    EMIT1(0x5B);
    /* pop rbp */
    EMIT1(0x5D);
    /* ret */
    EMIT1(0xC3);
    *proglen_ptr = proglen;
}

int bpf_int_jit_compile(bpf_prog_t *prog)
{
    if (!prog || !prog->insn_cnt) return -(s64)EINVAL;
    
    u8 *image = NULL;
    u32 proglen = 0;
    
    /* First pass: calculate length */
    build_prologue(image, &proglen);
    
    for (u32 i = 0; i < prog->insn_cnt; i++) {
        struct bpf_insn *insn = &prog->insns[i];
        u8 class = BPF_CLASS(insn->code);
        u8 dst = reg2hex[insn->dst_reg];
        u8 src = reg2hex[insn->src_reg];
        
        if (class == BPF_ALU64) {
            u8 op = BPF_OP(insn->code);
            if (op == BPF_MOV) {
                if (BPF_SRC(insn->code) == BPF_X) {
                    /* mov dst, src */
                    emit_rex(image, &proglen, 1, src, 0, dst);
                    EMIT1(0x89);
                    emit_modrm(image, &proglen, 3, src, dst);
                } else {
                    /* mov dst, imm */
                    emit_rex(image, &proglen, 1, 0, 0, dst);
                    EMIT1(0xC7);
                    emit_modrm(image, &proglen, 3, 0, dst);
                    emit_imm32(image, &proglen, insn->imm);
                }
            } else if (op == BPF_ADD) {
                if (BPF_SRC(insn->code) == BPF_X) {
                    emit_rex(image, &proglen, 1, src, 0, dst);
                    EMIT1(0x01);
                    emit_modrm(image, &proglen, 3, src, dst);
                } else {
                    emit_rex(image, &proglen, 1, 0, 0, dst);
                    EMIT1(0x81);
                    emit_modrm(image, &proglen, 3, 0, dst);
                    emit_imm32(image, &proglen, insn->imm);
                }
            }
        } else if (class == BPF_JMP) {
            u8 op = BPF_OP(insn->code);
            if (op == BPF_EXIT) {
                build_epilogue(image, &proglen);
            } else if (op == BPF_JA) {
                EMIT1(0xE9);
                emit_imm32(image, &proglen, 0); /* We need to patch this with actual offset in 2nd pass, but for now we skip jumping logic in simple JIT */
            }
        } else if (class == BPF_LD) {
            /* LD_IMM64 */
            if (BPF_SIZE(insn->code) == BPF_DW && BPF_MODE(insn->code) == BPF_IMM) {
                u64 imm64 = (u32)insn->imm | ((u64)(prog->insns[i+1].imm) << 32);
                emit_rex(image, &proglen, 1, 0, 0, dst);
                EMIT1(0xB8 | (dst & 7));
                emit_imm64(image, &proglen, imm64);
                i++; /* consume next instruction */
            }
        } else if (class == BPF_STX) {
            if (BPF_SIZE(insn->code) == BPF_DW) {
                /* mov [dst + off], src */
                emit_rex(image, &proglen, 1, src, 0, dst);
                EMIT1(0x89);
                emit_modrm(image, &proglen, 1, src, dst);
                EMIT1(insn->off);
            }
        } else if (class == BPF_LDX) {
            if (BPF_SIZE(insn->code) == BPF_DW) {
                /* mov dst, [src + off] */
                emit_rex(image, &proglen, 1, dst, 0, src);
                EMIT1(0x8B);
                emit_modrm(image, &proglen, 1, dst, src);
                EMIT1(insn->off);
            }
        }
        /* Ignore complex instructions for this simple PoC */
    }
    
    build_epilogue(image, &proglen);
    
    /*
     * Allocate the image from the W^X code allocator, never from the heap.
     *
     * kzalloc() would work in the sense that the bytes land somewhere and the
     * CPU would run them — which is precisely the problem it used to be: heap
     * memory lives in the HHDM, so a JIT image there was a page that stayed
     * writable for as long as the program was loaded while also being
     * executable, and every heap-overflow primitive in the kernel turned into
     * arbitrary ring-0 code execution by aiming at it. kmod_alloc_exec()
     * hands back memory that is writable now and executable only after
     * kmod_seal_exec() below has taken the write permission away.
     */
    image = kmod_alloc_exec(proglen);
    if (!image) return -(s64)ENOMEM;
    
    /* Second pass: emit instructions */
    u32 pass2_len = 0;
    build_prologue(image, &pass2_len);
    
    for (u32 i = 0; i < prog->insn_cnt; i++) {
        struct bpf_insn *insn = &prog->insns[i];
        u8 class = BPF_CLASS(insn->code);
        u8 dst = reg2hex[insn->dst_reg];
        u8 src = reg2hex[insn->src_reg];
        
        if (class == BPF_ALU64) {
            u8 op = BPF_OP(insn->code);
            if (op == BPF_MOV) {
                if (BPF_SRC(insn->code) == BPF_X) {
                    emit_rex(image, &pass2_len, 1, src, 0, dst);
                    EMIT1(0x89);
                    emit_modrm(image, &pass2_len, 3, src, dst);
                } else {
                    emit_rex(image, &pass2_len, 1, 0, 0, dst);
                    EMIT1(0xC7);
                    emit_modrm(image, &pass2_len, 3, 0, dst);
                    emit_imm32(image, &pass2_len, insn->imm);
                }
            } else if (op == BPF_ADD) {
                if (BPF_SRC(insn->code) == BPF_X) {
                    emit_rex(image, &pass2_len, 1, src, 0, dst);
                    EMIT1(0x01);
                    emit_modrm(image, &pass2_len, 3, src, dst);
                } else {
                    emit_rex(image, &pass2_len, 1, 0, 0, dst);
                    EMIT1(0x81);
                    emit_modrm(image, &pass2_len, 3, 0, dst);
                    emit_imm32(image, &pass2_len, insn->imm);
                }
            }
        } else if (class == BPF_JMP) {
            u8 op = BPF_OP(insn->code);
            if (op == BPF_EXIT) {
                build_epilogue(image, &pass2_len);
            } else if (op == BPF_JA) {
                EMIT1(0xE9);
                emit_imm32(image, &pass2_len, 0); 
            }
        } else if (class == BPF_LD) {
            if (BPF_SIZE(insn->code) == BPF_DW && BPF_MODE(insn->code) == BPF_IMM) {
                u64 imm64 = (u32)insn->imm | ((u64)(prog->insns[i+1].imm) << 32);
                emit_rex(image, &pass2_len, 1, 0, 0, dst);
                EMIT1(0xB8 | (dst & 7));
                emit_imm64(image, &pass2_len, imm64);
                i++;
            }
        } else if (class == BPF_STX) {
            if (BPF_SIZE(insn->code) == BPF_DW) {
                emit_rex(image, &pass2_len, 1, src, 0, dst);
                EMIT1(0x89);
                emit_modrm(image, &pass2_len, 1, src, dst);
                EMIT1(insn->off);
            }
        } else if (class == BPF_LDX) {
            if (BPF_SIZE(insn->code) == BPF_DW) {
                emit_rex(image, &pass2_len, 1, dst, 0, src);
                EMIT1(0x8B);
                emit_modrm(image, &pass2_len, 1, dst, src);
                EMIT1(insn->off);
            }
        }
    }
    
    build_epilogue(image, &pass2_len);

    /* The second pass must have emitted exactly what the first pass measured;
     * anything else means a write ran past the allocation, and the image is
     * not safe to seal and jump to. */
    if (pass2_len > proglen) {
        kmod_free_exec(image);
        return -(s64)EFAULT;
    }

    /* Writable ends here. From this point the image is executable and the
     * kernel cannot modify it — including through the HHDM alias, which
     * kprotect_seal() made non-executable at boot. */
    if (kmod_seal_exec(image) != 0) {
        kmod_free_exec(image);
        return -(s64)ENOMEM;
    }

    prog->bpf_func = image;
    return 0;
}
