/* ============================================================================
 * AzamiOS — True Userfaultfd Implementation
 * File: fs/userfaultfd.c
 * ============================================================================ */

#include "vfs.h"
#include "../kernel/mm/vma.h"
#include "../kernel/sched/sched.h"
#include "../arch/x86_64/mm/vmm.h"
#include "../kernel/mm/pmm.h"
#include "../kernel/mm/kmalloc.h"
#include "../arch/x86_64/cpu/spinlock.h"
#include "../include/azami/uapi/userfaultfd.h"

#define POLLIN     0x0001
#define POLLNVAL   0x0020
#define _IOC_NR(cmd) ((cmd) & 0xFF)

extern int copy_to_user(void *dst, const void *src, size_t size);
extern int copy_from_user(void *dst, const void *src, size_t size);

typedef struct uffd_msg_node {
    struct uffd_msg msg;
    thread_t *faulting_thread;
    struct uffd_msg_node *next;
} uffd_msg_node_t;

typedef struct userfaultfd_context {
    spinlock_t lock;
    uffd_msg_node_t *msg_queue_head;
    uffd_msg_node_t *msg_queue_tail;
    thread_t *waiting_thread; /* userspace thread blocked in read/poll */
} userfaultfd_context_t;

static s64 uffd_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    userfaultfd_context_t *ctx = (userfaultfd_context_t *)filp->private_data;
    if (!ctx || len < sizeof(struct uffd_msg)) return -(s64)EINVAL;

    irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);
    
    while (!ctx->msg_queue_head) {
        if (filp->f_flags & O_NONBLOCK) {
            spinlock_unlock_irqrestore(&ctx->lock, fl);
            return -(s64)EAGAIN;
        }
        ctx->waiting_thread = sched_current_thread();
        ctx->waiting_thread->state = THREAD_SLEEPING;
        spinlock_unlock_irqrestore(&ctx->lock, fl);
        sched_yield();
        fl = spinlock_lock_irqsave(&ctx->lock);
    }

    uffd_msg_node_t *node = ctx->msg_queue_head;
    ctx->msg_queue_head = node->next;
    if (!ctx->msg_queue_head) ctx->msg_queue_tail = NULL;
    
    spinlock_unlock_irqrestore(&ctx->lock, fl);

    if (copy_to_user(buf, &node->msg, sizeof(struct uffd_msg)) != 0) {
        kfree(node);
        return -(s64)EFAULT;
    }
    
    kfree(node);
    return sizeof(struct uffd_msg);
}

static int uffd_poll(file_t *filp)
{
    userfaultfd_context_t *ctx = (userfaultfd_context_t *)filp->private_data;
    if (!ctx) return POLLNVAL;
    
    irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);
    int mask = 0;
    if (ctx->msg_queue_head) {
        mask |= POLLIN;
    } else {
        ctx->waiting_thread = sched_current_thread();
    }
    spinlock_unlock_irqrestore(&ctx->lock, fl);
    return mask;
}

static s64 uffd_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    userfaultfd_context_t *ctx = (userfaultfd_context_t *)filp->private_data;
    if (!ctx) return -(s64)EBADF;

    if (cmd == UFFDIO_API) {
        struct uffdio_api api;
        if (copy_from_user(&api, (void*)arg, sizeof(api)) != 0) return -(s64)EFAULT;
        
        api.features = UFFD_API_FEATURES;
        api.ioctls = (1ULL << _IOC_NR(UFFDIO_REGISTER)) | (1ULL << _IOC_NR(UFFDIO_COPY));
        
        if (copy_to_user((void*)arg, &api, sizeof(api)) != 0) return -(s64)EFAULT;
        return 0;
    } 
    else if (cmd == UFFDIO_REGISTER) {
        struct uffdio_register reg;
        if (copy_from_user(&reg, (void*)arg, sizeof(reg)) != 0) return -(s64)EFAULT;
        
        process_t *proc = sched_current_process();
        if (!proc) return -(s64)EPERM;
        
        u64 start = ALIGN_DOWN(reg.range.start, PAGE_SIZE);
        u64 end = ALIGN_UP(reg.range.start + reg.range.len, PAGE_SIZE);
        
        vma_add(proc, start, end, VMA_PROT_READ | VMA_PROT_WRITE, VMA_F_ANON | VMA_F_UFFD_MISSING);
        proc->uffd_ctx = ctx; 
        
        reg.ioctls = (1ULL << _IOC_NR(UFFDIO_COPY));
        if (copy_to_user((void*)arg, &reg, sizeof(reg)) != 0) return -(s64)EFAULT;
        return 0;
    }
    else if (cmd == UFFDIO_COPY) {
        struct uffdio_copy copy;
        if (copy_from_user(&copy, (void*)arg, sizeof(copy)) != 0) return -(s64)EFAULT;
        
        process_t *proc = sched_current_process();
        if (!proc) return -(s64)EPERM;
        
        u64 dst = ALIGN_DOWN(copy.dst, PAGE_SIZE);
        u64 src = ALIGN_DOWN(copy.src, PAGE_SIZE);
        u64 len = ALIGN_UP(copy.len, PAGE_SIZE);
        s64 copied = 0;
        
        for (u64 offset = 0; offset < len; offset += PAGE_SIZE) {
            u64 phys = pmm_alloc_page();
            if (!phys) break;
            
            void *frame_virt = (void *)PHYS_TO_VIRT(phys);
            if (copy_from_user(frame_virt, (void*)(src + offset), PAGE_SIZE) != 0) {
                pmm_free_page(phys);
                break;
            }
            
            vmm_map(proc->pml4_phys, dst + offset, phys, VMM_F_USER | VMM_F_WRITE);
            copied += PAGE_SIZE;
        }
        
        copy.copy = copied > 0 ? copied : -(s64)EFAULT;
        
        /* Wake the faulting thread(s). Walking proc->threads here needs the
         * scheduler lock — the reaper relinks that list from other CPUs — and
         * the old open-coded walk also missed a thread still in
         * THREAD_BLOCKED_PENDING, which then never woke at all. */
        sched_wake_proc_waiters(proc);
        
        if (copy_to_user((void*)arg, &copy, sizeof(copy)) != 0) return -(s64)EFAULT;
        return copied > 0 ? 0 : -(s64)EAGAIN;
    }
    
    return -(s64)ENOSYS;
}

static s64 uffd_release(struct inode *inode, file_t *filp)
{
    (void)inode;
    userfaultfd_context_t *ctx = (userfaultfd_context_t *)filp->private_data;
    if (ctx) {
        irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);
        uffd_msg_node_t *node = ctx->msg_queue_head;
        while (node) {
            uffd_msg_node_t *next = node->next;
            kfree(node);
            node = next;
        }
        spinlock_unlock_irqrestore(&ctx->lock, fl);
        kfree(ctx);
    }
    return 0;
}

static const file_operations_t uffd_fops = {
    .read = uffd_read,
    .poll = uffd_poll,
    .ioctl = uffd_ioctl,
    .release = uffd_release,
};

int userfaultfd_create(file_t **out_file)
{
    userfaultfd_context_t *ctx = kzalloc(sizeof(userfaultfd_context_t));
    if (!ctx) return -(s64)ENOMEM;
    spinlock_init(&ctx->lock);
    
    file_t *filp = kzalloc(sizeof(file_t));
    if (!filp) {
        kfree(ctx);
        return -(s64)ENOMEM;
    }
    filp->f_op = (file_operations_t *)&uffd_fops;
    filp->private_data = ctx;
    filp->f_count = 1;
    
    *out_file = filp;
    return 0;
}

bool userfaultfd_handle_fault(process_t *proc, u64 fault_addr, u64 err_code)
{
    userfaultfd_context_t *ctx = (userfaultfd_context_t *)proc->uffd_ctx;
    if (!ctx) return false;
    
    uffd_msg_node_t *node = kzalloc(sizeof(uffd_msg_node_t));
    if (!node) return false;
    
    node->msg.event = UFFD_EVENT_PAGEFAULT;
    node->msg.arg.pagefault.address = fault_addr;
    node->msg.arg.pagefault.flags = (err_code & 2) ? 1 : 0;
    node->faulting_thread = sched_current_thread();
    
    irqflags_t fl = spinlock_lock_irqsave(&ctx->lock);
    if (ctx->msg_queue_tail) {
        ctx->msg_queue_tail->next = node;
        ctx->msg_queue_tail = node;
    } else {
        ctx->msg_queue_head = ctx->msg_queue_tail = node;
    }
    
    if (ctx->waiting_thread) {
        sched_unblock(ctx->waiting_thread);
        ctx->waiting_thread = NULL;
    }
    spinlock_unlock_irqrestore(&ctx->lock, fl);
    
    sched_block(THREAD_BLOCKED);
    
    return true; 
}
