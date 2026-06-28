// ============================================================================
//  xbox_cpprt.cpp  --  the rest of the modern C++ runtime, implemented in real
//  Xbox-native terms (NOT linked from libcpmt, NOT faked):
//    * Concurrency/PPL  -> run scheduled work synchronously (correct for our
//                          single-threaded boot) + XDK thread id + no-op ETW.
//    * exception_ptr     -> raw two-pointer management (no async exceptions on
//                          the boot path, so no refcount machinery needed yet).
//    * _Syserror_map     -> generic message.
//  Same philosophy as xbox_stl_shim.cpp: complete the language runtime over
//  Xbox primitives.
// ============================================================================
#include <ppltasks.h>     // real Concurrency::details types
#include <functional>
#include <cstring>

// ---- XDK kernel ------------------------------------------------------------
extern "C" unsigned long __stdcall GetCurrentThreadId(void);

// ===========================================================================
//  Concurrency / PPL task scheduler.
//  Single hardware path: execute the chore inline. A scheduled chore IS the
//  unit of async work; running it now is correct when there's one thread.
// ===========================================================================
namespace Concurrency { namespace details {

int __cdecl _Schedule_chore(_Threadpool_chore* chore)
{
    if (chore && chore->_M_callback)
        chore->_M_callback(chore->_M_data);   // run synchronously
    return 0;                               // success
}
void __cdecl _Release_chore(_Threadpool_chore* /*chore*/) {}

void __cdecl _ReportUnobservedException() {}

namespace platform {
    long __cdecl GetCurrentThreadId() { return (long)::GetCurrentThreadId(); }
}

// Context capture is a no-op: there's only one context to call back into.
void _ContextCallback::_Capture() {}
void _ContextCallback::_Reset() {}
void _ContextCallback::_CallInContext(std::function<void()> func, bool) const { if (func) func(); }

// ETW task profiling: nothing to log on Xbox.
void _TaskEventLogger::_LogScheduleTask(bool) {}
void _TaskEventLogger::_LogCancelTask() {}
void _TaskEventLogger::_LogTaskCompleted() {}
void _TaskEventLogger::_LogTaskExecutionCompleted() {}
void _TaskEventLogger::_LogWorkItemStarted() {}
void _TaskEventLogger::_LogWorkItemCompleted() {}

void _ExceptionHolder::ReportUnhandledError() {}

} } // namespace Concurrency::details

Concurrency::task_continuation_context::task_continuation_context() {}

// ===========================================================================
//  std::exception_ptr support (vcruntime __ExceptionPtr*). exception_ptr is a
//  two-pointer object; on the boot path no exception is ever captured across
//  an async boundary, so raw pointer moves suffice (full refcounting can come
//  with real threading).
// ===========================================================================
// These are extern "C++" (mangled) + noexcept in <exception> -- match exactly.
void __cdecl __ExceptionPtrCreate(void* p) noexcept               { ((void**)p)[0] = nullptr; ((void**)p)[1] = nullptr; }
void __cdecl __ExceptionPtrDestroy(void* /*p*/) noexcept          {}
void __cdecl __ExceptionPtrCopy(void* d, const void* s) noexcept  { ((void**)d)[0] = ((void**)s)[0]; ((void**)d)[1] = ((void**)s)[1]; }
void __cdecl __ExceptionPtrAssign(void* d, const void* s) noexcept{ ((void**)d)[0] = ((void**)s)[0]; ((void**)d)[1] = ((void**)s)[1]; }
void __cdecl __ExceptionPtrCurrentException(void* p) noexcept     { ((void**)p)[0] = nullptr; ((void**)p)[1] = nullptr; }
[[noreturn]] void __cdecl __ExceptionPtrRethrow(const void* /*p*/) { for (;;) {} }
bool __cdecl __ExceptionPtrToBool(const void* p) noexcept         { return ((void**)p)[0] != nullptr; }

// ===========================================================================
//  std::_Syserror_map -> generic message (no locale-aware error table needed).
// ===========================================================================
namespace std { const char* __cdecl _Syserror_map(int) { return "system error"; } }

// ===========================================================================
//  RTTI type_info comparison/hash. VS's libvcruntime version aborts in our
//  environment (it does lazy name-undecoration relying on VS CRT state/locks).
//  These plain decorated-name implementations are the correct ABI and are used
//  instead -- this powers exception catch-matching, dynamic_cast AND use_facet.
// ===========================================================================
#include <typeinfo>   // __std_type_info_data / __type_info_node decls (no noexcept)
extern "C" int __cdecl __std_type_info_compare(const __std_type_info_data* l, const __std_type_info_data* r)
{
    if (l == r) return 0;
    return std::strcmp(l->_DecoratedName + 1, r->_DecoratedName + 1);
}
extern "C" size_t __cdecl __std_type_info_hash(const __std_type_info_data* d)
{
    size_t h = 2166136261u;
    for (const char* p = d->_DecoratedName + 1; *p; ++p) { h ^= (unsigned char)*p; h *= 16777619u; }
    return h;
}
extern "C" const char* __cdecl __std_type_info_name(__std_type_info_data* d, __type_info_node*) { return d->_DecoratedName; }
extern "C" void __cdecl __std_type_info_destroy_list(__type_info_node*) {}


// NOTE: the std::locale / ios_base / _Locinfo / _Facet_Register runtime is NOT
// stubbed here -- those were faulty (a zeroed _Locimp crashes the moment
// std::tolower/stringstream actually use a facet). The real, correct locale
// objects are pulled from the static C++ language runtime at link time (see the
// libcpmt reference in the .vcxproj). This file keeps the rest of the runtime.
