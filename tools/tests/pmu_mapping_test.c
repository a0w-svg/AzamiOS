/* Link the real vendor mapper; discard privileged boot/MSR paths with GC. */
#include <assert.h>
#include <stdio.h>
#include "../../arch/x86_64/cpu/pmu.h"
int main(void)
{
    const u64 amd[10]={0x76,0xc0,0,0,0xc2,0xc3,0,0,0,0};
    const u64 intel[10]={0x3c,0xc0,0x4f2e,0x412e,0xc4,0xc5,0,0,0,0x013c};
    const int bit[10]={0,1,3,4,5,6,-1,-1,-1,2};
    g_pmu.present=false;
    for(unsigned i=0;i<10;i++) assert(!pmu_hw_event(i));
    g_pmu.present=true; g_pmu.is_amd=true;
    for(unsigned i=0;i<10;i++) assert(pmu_hw_event(i)==amd[i]);
    assert(!pmu_hw_event(~0ULL));
    g_pmu.is_amd=false;
    for(unsigned unavailable=0;unavailable<128;unavailable++) {
        g_pmu.unavail_mask=unavailable;
        for(unsigned i=0;i<10;i++)
            assert(pmu_hw_event(i)==(bit[i]>=0 && (unavailable&(1u<<bit[i])) ? 0 : intel[i]));
    }
    assert(!pmu_hw_event(~0ULL));
    for(unsigned width=32;width<=64;width++) {
        u64 mask=width==64 ? ~0ULL : (1ULL<<width)-1;
        assert(pmu_counter_delta(15,mask-15,mask)==31);
        assert(pmu_counter_delta(32,16,mask)==16);
        assert(pmu_counter_delta(mask,mask,mask)==0);
    }
    puts("PASS: production AMD/Intel mappings, all 128 architectural availability masks, widths 32-64");
}
