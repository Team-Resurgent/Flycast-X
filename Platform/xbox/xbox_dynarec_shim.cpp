// ============================================================================
//  xbox_dynarec_shim.cpp  --  intentionally empty for the FASTMEM (virtmem) JIT.
//
//  With the 512MB virtmem window enabled (addrspace::reserve -> virtmem::init),
//  the 64MB FPCB lives RESERVED inside the window and is committed ON DEMAND,
//  one page at a time, by addrspace::bm_lockedWrite() (driven from
//  xbox_FastmemFault when the JIT first touches a dispatch slot). That path uses
//  the STOCK bm_vmem_pagefill(addr, PAGE_SIZE) and the STOCK
//  rdv_SetFailedToFindBlockHandler -- and the stale-handler bug can't occur,
//  because pages are filled on demand AFTER generate_mainloop() installs the JIT
//  handler. So no overrides here.
//
//  (The 8M-entry-fill / once-refill overrides that previously lived here were
//  for the MALLOC-FALLBACK no-fastmem JIT, where the whole FPCB is committed up
//  front. They MUST NOT be used with virtmem: bm_vmem_pagefill is called per
//  PAGE there, and filling 8M entries from a single page address overruns it.)
// ============================================================================
