// ============================================================================
//  xbox_dynarec_shim.cpp  --  Xbox overrides for the SH-4 JIT on the MALLOC
//  fallback (no fastmem). Used by the "JIT, no-fastmem" build: the JIT runs
//  with function-call memory access (MemType::Slow) so NOTHING faults -- which
//  matters because RXDK does not deliver access violations whose faulting PC is
//  in JIT code (see flycast-xbox-jit memory). The base dynarec win (skipping
//  per-instruction decode) needs none of that fault machinery.
//
//  Two overrides, both required ONLY on the malloc fallback (ram_base == null):
//    1. bm_vmem_pagefill: the Sh4RCB is _aligned_malloc'd with only the lower
//       16MB (4M entries) of the 64MB FPCB committed (alloc_sh4rcb in
//       xbox_crt_shim.cpp). The stock fill writes all 16M entries -> overruns
//       into uncommitted pages. Fill only the committed 4M. DC BIOS/game code
//       maps to low FPCB indices, so 4M entries covers it.
//    2. rdv_SetFailedToFindBlockHandler: on the malloc fallback the FPCB is
//       filled at init (before generate_mainloop) with the INITIAL
//       ngen_FailedToFindBlock value = ngen_FailedToFindBlock_internal, a C
//       function ending in RET. The mainloop JMPs (not CALLs) to FPCB entries,
//       so that RET corrupts the stack -> hang. generate_mainloop() calls this
//       right after building the JMP-safe JIT stub; we re-fill the committed
//       FPCB with that stub. Runs once (generate_mainloop is guarded by
//       ::mainloop!=null); later cache resets fill with the now-correct value.
// ============================================================================
#include "types.h"            // pulls in build.h (FEAT_SHREC, DYNAREC_NONE)

#if FEAT_SHREC != DYNAREC_NONE

#include "hw/sh4/sh4_if.h"    // p_sh4rcb, Sh4RCB
#include "hw/sh4/dyna/ngen.h" // ngen_FailedToFindBlock, rdv_SetFailedToFindBlockHandler

// 32MB committed / 4 bytes per entry = 8M entries (covers all 16MB of DC RAM).
// MUST match k_fpcb_lower_bytes in xbox_crt_shim.cpp.
static const size_t k_committed_entries = (size_t)8 * 1024 * 1024;

// Fill only the committed lower 8M FPCB entries (ignore the caller's size).
void bm_vmem_pagefill(void** ptr, unsigned int /*size_bytes*/)
{
    for (size_t i = 0; i < k_committed_entries; ++i)
        ptr[i] = (void*)ngen_FailedToFindBlock;
}

// Record the JIT handler. On the FIRST call (right after generate_mainloop) also
// re-fill the committed FPCB to repair the init-time stale-handler fill (the
// FPCB was filled with ngen_FailedToFindBlock_internal before the JIT stub
// existed). Subsequent calls (cache resets) must NOT re-fill: bm_ResetCache
// already re-fills via bm_vmem_pagefill with the now-correct handler, and wiping
// 8M live entries here would discard every cached block (thrash).
void rdv_SetFailedToFindBlockHandler(void (*handler)())
{
    ngen_FailedToFindBlock = handler;
    static bool refilled = false;
    if (!refilled)
    {
        refilled = true;
        void** fpcb = (void**)p_sh4rcb->fpcb;
        for (size_t i = 0; i < k_committed_entries; ++i)
            fpcb[i] = (void*)handler;
    }
}

#endif // FEAT_SHREC != DYNAREC_NONE
