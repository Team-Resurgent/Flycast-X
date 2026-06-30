// ============================================================================
//  xbox_eh_shim.cpp  --  a correct __CxxFrameHandler3 for RXDK.
//
//  PROBLEM (isolated 2026-06-29 on xemu): RXDK's 2003-era __CxxFrameHandler
//  HANGS while processing a FOREIGN (non-C++) exception during the exception
//  SEARCH phase, when the faulting instruction is in dynamically-generated
//  RWX (JIT) memory. The v143 compiler emits __CxxFrameHandler3 in its EH
//  tables, and we'd been /ALTERNATENAME-bridging that straight to the old
//  __CxxFrameHandler. That bridge works for faults originating in .text, but
//  hangs for faults originating in JIT code -- which is exactly the fastmem
//  path (JIT code runs under run()'s and X86Dynarec::mainloop()'s try/catch).
//  Result: an access violation in JIT code never reaches our SEH handler, so
//  fastmem MMIO faults can't be serviced.
//
//  FIX: own __CxxFrameHandler3. For a foreign exception in the search phase,
//  return ExceptionContinueSearch immediately (bypassing the old handler's
//  hanging code) so the AV propagates to our outer fastmem handler, which
//  resolves it and resumes (EXCEPTION_CONTINUE_EXECUTION -- never unwinds).
//  Real C++ exceptions, and ALL unwinds, tail-jump to the genuine
//  __CxxFrameHandler with the FuncInfo (passed in EAX on x86) preserved.
// ============================================================================

extern "C" {

// The real RXDK personality routine. x86 ABI: per-function thunk loads the
// FuncInfo pointer into EAX, then jumps here. Declared so the linker binds the
// CRT symbol; we never call it via C (only tail-jump in asm, EAX intact).
int __cdecl __CxxFrameHandler(void* pExceptionRecord, void* pEstablisherFrame,
                              void* pContext, void* pDispatcherContext);

// EXCEPTION_RECORD layout (x86): +0 ExceptionCode, +4 ExceptionFlags.
// C++ exception code = 0xE06D7363 ('msc'|0xE0000000).
// EXCEPTION_UNWINDING (0x2) | EXCEPTION_EXIT_UNWIND (0x4) = 0x6.
__declspec(naked) int __cdecl __CxxFrameHandler3()
{
    __asm {
        mov  ecx, dword ptr [esp+4]     // EXCEPTION_RECORD*
        mov  edx, dword ptr [ecx]       // ExceptionCode
        cmp  edx, 0E06D7363h            // a C++ exception?
        je   delegate
        test dword ptr [ecx+4], 6       // unwinding/exit-unwind?
        jnz  delegate
        // foreign exception, search phase: pass through cleanly.
        mov  eax, 1                     // ExceptionContinueSearch
        ret
    delegate:
        // EAX still holds FuncInfo from the thunk; hand off to the real handler.
        jmp  __CxxFrameHandler
    }
}

} // extern "C"
