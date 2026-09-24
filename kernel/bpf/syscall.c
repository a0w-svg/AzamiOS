#include "bpf_internal.h"
#include "../mm/kmalloc.h"
#include "../mm/kmodmem.h"
#include "../sched/sched.h"

extern int copy_from_user(void *dst, const void *src, size_t size);
extern int copy_to_user(void *dst, const void *src, size_t size);
extern s64 syscall_fd_install(process_t *proc, file_t *file, u8 fd_flags);

static s64 bpf_prog_release(inode_t *inode, file_t *filp)
{
    (void)inode;
    bpf_prog_t *prog = (bpf_prog_t *)filp->private_data;
    if (prog) {
        if (prog->insns) kfree(prog->insns);
        if (prog->bpf_func) kmod_free_exec(prog->bpf_func);
        kfree(prog);
    }
    return 0;
}

static const file_operations_t bpf_prog_fops = {
    .release = bpf_prog_release,
};

static int bpf_prog_load(union bpf_attr *attr, file_t **out_file)
{
    if (attr->insn_cnt == 0 || attr->insn_cnt > 4096) return -(s64)E2BIG;
    
    bpf_prog_t *prog = kzalloc(sizeof(bpf_prog_t));
    if (!prog) return -(s64)ENOMEM;
    
    prog->prog_type = attr->prog_type;
    prog->insn_cnt = attr->insn_cnt;
    
    u32 insn_size = prog->insn_cnt * sizeof(struct bpf_insn);
    prog->insns = kzalloc(insn_size);
    if (!prog->insns) {
        kfree(prog);
        return -(s64)ENOMEM;
    }
    
    if (copy_from_user(prog->insns, (void *)attr->insns, insn_size) != 0) {
        kfree(prog->insns);
        kfree(prog);
        return -(s64)EFAULT;
    }
    
    int err = bpf_check(prog);
    if (err < 0) {
        kfree(prog->insns);
        kfree(prog);
        return err;
    }
    
    err = bpf_int_jit_compile(prog);
    if (err < 0) {
        kfree(prog->insns);
        kfree(prog);
        return err;
    }
    
    file_t *filp = kzalloc(sizeof(file_t));
    if (!filp) {
        if (prog->bpf_func) kmod_free_exec(prog->bpf_func);
        kfree(prog->insns);
        kfree(prog);
        return -(s64)ENOMEM;
    }
    
    filp->f_op = (file_operations_t *)&bpf_prog_fops;
    filp->private_data = prog;
    filp->f_count = 1;
    
    *out_file = filp;
    return 0;
}

static int bpf_prog_test_run(union bpf_attr *attr)
{
    process_t *proc = sched_current_process();
    int fd = attr->test.prog_fd;
    
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;
    file_t *filp = proc->handle_table[fd];
    if (filp->f_op != &bpf_prog_fops) return -(s64)EINVAL;
    
    bpf_prog_t *prog = (bpf_prog_t *)filp->private_data;
    if (!prog || !prog->bpf_func) return -(s64)EINVAL;
    
    /* Call the JITed function. The JIT code takes (void *ctx) in rdi, returns u32 in rax. */
    u32 (*bpf_func)(void *) = (u32 (*)(void *))prog->bpf_func;
    
    /* Dummy context for test */
    u64 dummy_ctx[1] = {0};
    
    u32 ret = bpf_func(dummy_ctx);
    
    /* We don't copy back to user space for this minimal implementation, 
     * just return the result from the syscall for easy verification. */
    return (int)ret;
}

s64 bpf_syscall_entry(int cmd, union bpf_attr *uattr, unsigned int size)
{
    if (size < sizeof(union bpf_attr)) return -(s64)EINVAL;
    
    union bpf_attr attr;
    if (copy_from_user(&attr, uattr, sizeof(attr)) != 0) return -(s64)EFAULT;
    
    file_t *new_file = NULL;
    int err = 0;
    int is_fd = 0;
    
    switch (cmd) {
    case BPF_MAP_CREATE:
        err = bpf_map_create(&attr, &new_file);
        is_fd = 1;
        break;
    case BPF_PROG_LOAD:
        err = bpf_prog_load(&attr, &new_file);
        is_fd = 1;
        break;
    case BPF_MAP_LOOKUP_ELEM:
    case BPF_MAP_UPDATE_ELEM: {
        process_t *proc = sched_current_process();
        int fd = attr.map_fd;
        if (fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;
        file_t *filp = proc->handle_table[fd];
        
        /* Max key/value size supported in our stack buffers for syscall */
        u8 key[64] = {0};
        u8 val[64] = {0};
        
        if (copy_from_user(key, (void *)attr.key, 64) != 0) return -(s64)EFAULT;
        
        if (cmd == BPF_MAP_LOOKUP_ELEM) {
            err = bpf_map_lookup_elem(filp, key, val);
            if (err == 0) {
                if (copy_to_user((void *)attr.value, val, 64) != 0) err = -(s64)EFAULT;
            }
        } else {
            if (copy_from_user(val, (void *)attr.value, 64) != 0) return -(s64)EFAULT;
            err = bpf_map_update_elem(filp, key, val, attr.flags);
        }
        break;
    }
    case BPF_PROG_TEST_RUN:
        err = bpf_prog_test_run(&attr);
        break;
    default:
        return -(s64)EINVAL;
    }
    
    if (err < 0) return err;
    
    if (is_fd && new_file) {
        /* Assign FD */
        process_t *proc = sched_current_process();
        int fd = syscall_fd_install(proc, new_file, 0);
        if (fd < 0) {
            if (new_file->f_op->release) new_file->f_op->release(NULL, new_file);
            kfree(new_file);
            return -(s64)EMFILE;
        }
        return fd;
    }
    
    return err;
}
