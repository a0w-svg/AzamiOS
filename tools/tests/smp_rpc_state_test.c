/* Exercise the production ownership transitions, including delayed receivers. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include "../../arch/x86_64/cpu/smp_rpc_state.h"
static uint64_t state, tag;
static int begin, finished, stop;
static void *receiver(void *unused)
{
    (void)unused;
    for (;;) {
        while(!__atomic_load_n(&begin,__ATOMIC_ACQUIRE)) {
            if(__atomic_load_n(&stop,__ATOMIC_ACQUIRE)) return NULL;
            sched_yield();
        }
        uint64_t current=tag;
        if(rpc_enter(&state,current)) rpc_complete(&state,current);
        __atomic_store_n(&begin,0,__ATOMIC_RELEASE);
        __atomic_store_n(&finished,1,__ATOMIC_RELEASE);
    }
}
int main(void)
{
    assert(rpc_claim(&state,8));
    __atomic_store_n(&state,8|RPC_QUEUED,__ATOMIC_RELEASE);
    assert(rpc_cancel(&state,8));
    assert(rpc_claim(&state,16));
    __atomic_store_n(&state,16|RPC_QUEUED,__ATOMIC_RELEASE);
    assert(!rpc_enter(&state,8)); /* stale IPI cannot claim a reused slot */
    assert(rpc_enter(&state,16));
    assert(rpc_cancel(&state,16));
    assert(!rpc_claim(&state,24)); /* delayed callback retains its own storage */
    rpc_complete(&state,16);
    assert(rpc_claim(&state,24));
    __atomic_store_n(&state,24|RPC_QUEUED,__ATOMIC_RELEASE);
    assert(rpc_enter(&state,24)); rpc_complete(&state,24);
    assert(!rpc_cancel(&state,24)); /* completed output belongs to sender */
    assert(state==(24|RPC_DONE));
    __atomic_store_n(&state,0,__ATOMIC_RELEASE);
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,receiver,NULL));
    for(unsigned i=0;i<100000;i++) {
        tag=(uint64_t)(i+4)*8;
        assert(rpc_claim(&state,tag));
        __atomic_store_n(&state,tag|RPC_QUEUED,__ATOMIC_RELEASE);
        __atomic_store_n(&finished,0,__ATOMIC_RELAXED);
        __atomic_store_n(&begin,1,__ATOMIC_RELEASE);
        bool canceled=rpc_cancel(&state,tag);
        while(!__atomic_load_n(&finished,__ATOMIC_ACQUIRE)) sched_yield();
        uint64_t result=__atomic_load_n(&state,__ATOMIC_ACQUIRE);
        assert(result==(canceled ? 0 : (tag|RPC_DONE)));
        __atomic_store_n(&state,0,__ATOMIC_RELEASE);
    }
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);
    assert(!pthread_join(thread,NULL));
    puts("smp_rpc ownership: delayed completion, stale generation, completion/cancel race; 100000 rounds passed");
    return 0;
}
