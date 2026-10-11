/* Bounded, by-value CPU requests. The legacy stack-based smp_call_* API is
 * unchanged: it has callers that require unconditional acknowledgement. */
#include "smp.h"
#include "smp_rpc_state.h"
#include "spinlock.h"
#include "../../../kernel/time/timekeeping.h"
#include "../../../kernel/lib/string.h"
#include "../../../kernel/cmdline.h"
#include "../../../include/azami/uapi/errno.h"

typedef struct {
    u64 state;
    void (*func)(void *);
    u64 payload[SMP_RPC_BYTES/sizeof(u64)];
} rpc_slot_t;
static rpc_slot_t g_rpc_slots[SMP_MAX_CPUS];
static u64 g_rpc_tag;
static s32 g_rpc_drop_cpu = -1;

void smp_rpc_init(void)
{
    /* Explicit fault injection applies only to this new request mechanism. */
    long cpu=cmdline_get_long("smp_rpc_drop_cpu",-1);
    if(cpu>=0 && cpu<SMP_MAX_CPUS) g_rpc_drop_cpu=(s32)cpu;
}
void smp_rpc_drain(void)
{
    u32 cpu=smp_current_cpu_id();
    if(cpu>=SMP_MAX_CPUS || (s32)cpu==g_rpc_drop_cpu) return;
    rpc_slot_t *slot=&g_rpc_slots[cpu];
    u64 state=__atomic_load_n(&slot->state,__ATOMIC_ACQUIRE);
    if((state&7)!=RPC_QUEUED) return;
    u64 tag=state&~7ULL;
    if(!rpc_enter(&slot->state,tag)) return;
    /* Sender cannot reuse or change payload while RUNNING/ABANDONED. */
    slot->func(slot->payload);
    rpc_complete(&slot->state,tag);
}

int smp_rpc_call(u32 cpu, void (*func)(void *), void *value, size_t size, u64 deadline_ns)
{
    if(!func || !value || !size || size>SMP_RPC_BYTES || cpu>=SMP_MAX_CPUS) return -EINVAL;
    if(!smp_cpu_online(cpu)) return -ENODEV;
    u64 now;
    if(!ktime_try_get_ns(&now)) return -EAGAIN;
    if(now>=deadline_ns) return -ETIMEDOUT;
    rpc_slot_t *slot=&g_rpc_slots[cpu];
    u64 tag=__atomic_add_fetch(&g_rpc_tag,8,__ATOMIC_RELAXED);
    if(!rpc_claim(&slot->state,tag)) return -EBUSY;
    slot->func=func;
    memcpy(slot->payload,value,size);
    __atomic_store_n(&slot->state,tag|RPC_QUEUED,__ATOMIC_RELEASE);
    if(cpu==smp_current_cpu_id()) {
        irqflags_t fl=spinlock_irq_save();
        smp_rpc_drain();
        spinlock_irq_restore(fl);
    } else smp_send_ipi(cpu,SMP_VEC_CALL_FUNC);
    /* Only bounded callbacks are drained here; legacy callbacks are arbitrary
     * code and could turn a deadline wait into another unbounded wait. */
    for(u32 spins=0;;spins++) {
        u64 state=__atomic_load_n(&slot->state,__ATOMIC_ACQUIRE);
        if(state==(tag|RPC_DONE)) {
            memcpy(value,slot->payload,size);
            __atomic_store_n(&slot->state,0,__ATOMIC_RELEASE);
            return 0;
        }
        if((ktime_try_get_ns(&now) && now>=deadline_ns) || spins>=1000000) {
            if(rpc_cancel(&slot->state,tag)) return -ETIMEDOUT;
            continue; /* completion won cancellation; consume its result */
        }
        irqflags_t fl=spinlock_irq_save();
        smp_rpc_drain();
        spinlock_irq_restore(fl);
        cpu_pause();
    }
}
