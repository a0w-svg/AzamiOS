/* Fixed request ownership shared by the IPI path and the host race tests. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

enum { RPC_RESERVED=1, RPC_QUEUED, RPC_RUNNING, RPC_DONE, RPC_ABANDONED };

static inline bool rpc_cas(uint64_t *state, uint64_t from, uint64_t to)
{
    return __atomic_compare_exchange_n(state,&from,to,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static inline bool rpc_claim(uint64_t *state, uint64_t tag)
{ return rpc_cas(state,0,tag|RPC_RESERVED); }
static inline bool rpc_enter(uint64_t *state, uint64_t tag)
{ return rpc_cas(state,tag|RPC_QUEUED,tag|RPC_RUNNING); }
/* false means DONE won the race; the sender must consume that result.
 * An abandoned running request stays owned by the receiver until completion. */
static inline bool rpc_cancel(uint64_t *state, uint64_t tag)
{
    if(rpc_cas(state,tag|RPC_QUEUED,0)) return true;
    return rpc_cas(state,tag|RPC_RUNNING,tag|RPC_ABANDONED);
}
static inline void rpc_complete(uint64_t *state, uint64_t tag)
{
    if(!rpc_cas(state,tag|RPC_RUNNING,tag|RPC_DONE))
        rpc_cas(state,tag|RPC_ABANDONED,0);
}
