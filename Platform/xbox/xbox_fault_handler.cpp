// ============================================================================
//  xbox_fault_handler.cpp  --  fastmem / SMC access-violation handler.
//
//  With fastmem on, the x86 JIT emits direct `mov reg,[ecx + ram_base]` loads
//  (rec-x86 x86_ops.cpp MemType::Fast). Those hit committed RAM/VRAM directly
//  but FAULT on:
//    * MMIO + uncommitted RAM mirrors  -> sh4Dynarec->rewrite() patches the call
//      site to use the slow handler (one fault per access site, amortized).
//    * the reserved FPCB jump table    -> bm_lockedWrite() commits+fills the page
//      on demand (this is how the 64MB fpcb stays mostly unbacked).
//    * code-protected RAM (SMC)        -> bm_RamWriteAccess() unprotects+discards.
//    * texture-protected VRAM          -> VramLockedWrite() invalidates the tex.
//
//  Flycast's desktop build does this from a SetUnhandledExceptionFilter handler
//  (windows/fault_handler.cpp). On Xbox we can't use that (and a __try wrapper
//  catches first anyway), so the SEH __except filter in main_xbox.cpp calls
//  xbox_FastmemFault(); returning EXCEPTION_CONTINUE_EXECUTION resumes the
//  (rewritten) faulting instruction without unwinding.
// ============================================================================
#include "types.h"

#if FEAT_SHREC != DYNAREC_NONE

#include "oslib/host_context.h"
#include "hw/sh4/dyna/ngen.h"          // sh4Dynarec, Sh4Dynarec::rewrite
#include "hw/sh4/dyna/blockmanager.h"  // bm_RamWriteAccess
#include "hw/mem/addrspace.h"          // addrspace::bm_lockedWrite
#include "hw/mem/mem_watch.h"          // memwatch::writeAccess
#include "rend/TexCache.h"             // VramLockedWrite
#include <xtl.h>

static void readContext(const EXCEPTION_POINTERS* ep, host_context_t& c)
{
    c.pc  = ep->ContextRecord->Eip;
    c.esp = ep->ContextRecord->Esp;
    c.eax = ep->ContextRecord->Eax;
    c.ecx = ep->ContextRecord->Ecx;
}

static void writeContext(EXCEPTION_POINTERS* ep, const host_context_t& c)
{
    ep->ContextRecord->Eip = c.pc;
    ep->ContextRecord->Esp = c.esp;
    ep->ContextRecord->Eax = c.eax;
    ep->ContextRecord->Ecx = c.ecx;
}

// Diagnostic counters (read from the render loop; never printed from the filter).
extern "C" {
volatile unsigned g_fault_total    = 0;
volatile unsigned g_fault_fpcb     = 0;   // FPCB demand-page commits
volatile unsigned g_fault_rewrite  = 0;   // fastmem -> slow rewrites
volatile unsigned g_fault_ramsmc   = 0;   // RAM SMC unprotects
volatile unsigned g_fault_vram     = 0;   // VRAM texture invalidations
volatile unsigned g_fault_unhandled= 0;   // genuine faults
volatile unsigned g_fault_lastaddr = 0;   // last faulting address
volatile unsigned g_fault_lastpc   = 0;   // last faulting Eip
}

// Returns EXCEPTION_CONTINUE_EXECUTION (-1) if the fault was resolved (resume),
// or 0 if it's a genuine crash the caller should handle/report.
extern "C" int xbox_FastmemFault(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return 0;

    u8* address = (u8*)ep->ExceptionRecord->ExceptionInformation[1];
    ++g_fault_total;

    // ram watcher (GGPO; no-op here) / SMC code protection / texture protection
    if (memwatch::writeAccess(address))            return EXCEPTION_CONTINUE_EXECUTION;
    if (bm_RamWriteAccess(address))                { ++g_fault_ramsmc; return EXCEPTION_CONTINUE_EXECUTION; }
    if (VramLockedWrite(address))                  { ++g_fault_vram;   return EXCEPTION_CONTINUE_EXECUTION; }
    // FPCB jump-table demand paging (commit + fill the reserved page).
    if (addrspace::bm_lockedWrite(address))        { ++g_fault_fpcb;   return EXCEPTION_CONTINUE_EXECUTION; }

    // fastmem fast-path access to MMIO / uncommitted mirror: rewrite to slow.
    host_context_t ctx;
    readContext(ep, ctx);
    if (sh4Dynarec != nullptr && sh4Dynarec->rewrite(ctx, address))
    {
        ++g_fault_rewrite;
        writeContext(ep, ctx);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    ++g_fault_unhandled;
    return 0;   // genuine fault
}

// ---- Top-level (unhandled) exception filter ------------------------------
// CRITICAL: the JIT mainloop runs on a custom stack frame, so a fault inside
// JIT code can't be walked back to a frame-based __try/__except (the FS:[0]
// chain isn't reachable). A process-wide unhandled-exception filter fires
// regardless of stack state -- this is how Flycast's desktop x86 build does it
// (windows/fault_handler.cpp). Returning EXCEPTION_CONTINUE_EXECUTION resumes
// the (rewritten) faulting instruction.
static LPTOP_LEVEL_EXCEPTION_FILTER s_prevFilter = nullptr;

static LONG WINAPI xbox_UnhandledFilter(EXCEPTION_POINTERS* ep)
{
    static unsigned s_uf = 0;
    if (++s_uf <= 50) OutputDebugStringA("UNHANDLED-FILTER reached\n");
    if (xbox_FastmemFault(ep) == EXCEPTION_CONTINUE_EXECUTION)
        return EXCEPTION_CONTINUE_EXECUTION;
    // genuine fault: already counted/logged as BAD above; let default run.
    if (s_prevFilter)
        return s_prevFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void os_InstallFaultHandler()
{
    s_prevFilter = SetUnhandledExceptionFilter(xbox_UnhandledFilter);
    OutputDebugStringA("FLYCAST: fault handler installed (SetUnhandledExceptionFilter)\n");
}
void os_UninstallFaultHandler()
{
    SetUnhandledExceptionFilter(s_prevFilter);
}

#else  // interpreter build: no JIT/fastmem, nothing to resolve.

struct _EXCEPTION_POINTERS;
extern "C" int xbox_FastmemFault(struct _EXCEPTION_POINTERS*) { return 0; }
void os_InstallFaultHandler()   {}
void os_UninstallFaultHandler() {}

#endif
