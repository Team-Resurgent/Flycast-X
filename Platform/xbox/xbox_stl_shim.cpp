// ============================================================================
//  xbox_stl_shim.cpp  --  the entire gap between the modern VS2022 (v143) C++
//  standard library and RXDK's ancient Xbox CRT.
//
//  The modern STL headers compile fine under v143, but a handful of their
//  out-of-line runtime entry points live in VS2022's libcpmt/vcruntime, which
//  we do NOT link (we link RXDK's Xbox CRT instead). This file provides those
//  entry points, backed by Xbox primitives -- same idea as xbox_libc.c.
//
//  Discovered empirically: this is EXACTLY the set of LNK2019 symbols the
//  link probe reported, nothing more.
// ============================================================================
#include <exception>     // ::__std_exception_data + extern "C" decls
#include <stdexcept>     // std::length_error
#include <functional>    // std::bad_function_call
#include <new>           // operator delete
#include <chrono>        // declares _Query_perf_counter/_frequency (xtimec.h)
#include <mutex>         // declares _Mtx_lock/_Mtx_unlock + _Mtx_t (xthreads.h)
#include <thread>        // _Thrd_* decls
#include <condition_variable> // _Cnd_* decls
#include <cstring>
#include <cstdlib>
#include <cstdint>

// ---- Xbox kernel perf counter (declared locally to avoid pulling <xtl.h>
//      into a TU full of modern STL headers; QuadPart is binary-compatible
//      with long long). Provided by RXDK's xapilib. ------------------------
extern "C" int __stdcall QueryPerformanceCounter(long long*);
extern "C" int __stdcall QueryPerformanceFrequency(long long*);

// ===========================================================================
//  <chrono> steady_clock backing
// ===========================================================================
// Signatures must match xtimec.h exactly (C linkage + noexcept).
long long __cdecl _Query_perf_counter() noexcept
{
    long long v = 0;
    QueryPerformanceCounter(&v);
    return v;
}

long long __cdecl _Query_perf_frequency() noexcept
{
    long long v = 0;
    QueryPerformanceFrequency(&v);
    return v ? v : 1;   // never divide-by-zero in duration math
}

// ===========================================================================
//  std::mutex primitives.
//  NO-OP for the single-threaded headless-boot path (no contention possible).
//  MUST be replaced with CRITICAL_SECTION-backed real locks before we enable
//  AICA/render threads (Stage 5+).
// ===========================================================================
// _Mtx_t / _Thrd_result come from <xthreads.h> (via <mutex>); match exactly.
_Thrd_result __cdecl _Mtx_lock(_Mtx_t)   noexcept { return _Thrd_result::_Success; }
_Thrd_result __cdecl _Mtx_unlock(_Mtx_t) noexcept { return _Thrd_result::_Success; }

// ===========================================================================
//  vcruntime exception message-buffer management
// ===========================================================================
extern "C" void __cdecl __std_exception_copy(const __std_exception_data* from,
                                             __std_exception_data* to)
{
    if (from->_DoFree && from->_What) {
        const size_t n = std::strlen(from->_What) + 1;
        char* p = static_cast<char*>(std::malloc(n));
        if (p) {
            std::memcpy(p, from->_What, n);
            to->_What   = p;
            to->_DoFree = true;
            return;
        }
    }
    to->_What   = from->_What;
    to->_DoFree = false;
}

extern "C" void __cdecl __std_exception_destroy(__std_exception_data* data)
{
    if (data->_DoFree)
        std::free(const_cast<char*>(data->_What));
    data->_What   = nullptr;
    data->_DoFree = false;
}

// ===========================================================================
//  STL throw helpers
// ===========================================================================
namespace std {
    [[noreturn]] void __cdecl _Xlength_error(const char* msg) { throw std::length_error(msg); }
    [[noreturn]] void __cdecl _Xbad_function_call()          { throw std::bad_function_call(); }
    // Mutex/condvar fatal path -- never reached on the single-threaded boot.
    // Throw something light to avoid pulling in the <system_error> machinery.
    [[noreturn]] void __cdecl _Throw_Cpp_error(int code) {
        throw std::runtime_error("std::_Throw_Cpp_error");
        (void)code;
    }
}

// ===========================================================================
//  CRT fatal-error trap (invalid parameter / unrecoverable STL state)
// ===========================================================================
extern "C" [[noreturn]] void __cdecl _invoke_watson(const wchar_t*, const wchar_t*,
                                                    const wchar_t*, unsigned int,
                                                    unsigned int)
{
    for (;;) { }   // halt; a real build can HalReboot/__debugbreak here
}

// ===========================================================================
//  std::condition_variable primitives (single-threaded boot: no real waits).
//  Match xthreads.h exactly. "Always succeeds" per the ABI comments there.
// ===========================================================================
_Thrd_result __cdecl _Cnd_wait(_Cnd_t, _Mtx_t) noexcept       { return _Thrd_result::_Success; }
_Thrd_result __cdecl _Cnd_broadcast(_Cnd_t) noexcept          { return _Thrd_result::_Success; }
_Thrd_result __cdecl _Cnd_signal(_Cnd_t) noexcept             { return _Thrd_result::_Success; }
void __cdecl _Cnd_register_at_thread_exit(_Cnd_t, _Mtx_t, int*) noexcept {}
void __cdecl _Cnd_unregister_at_thread_exit(_Mtx_t) noexcept  {}
void __cdecl _Cnd_do_broadcast_at_thread_exit() noexcept      {}
_Thrd_result __stdcall _Cnd_timedwait_for_unchecked(_Cnd_t, _Mtx_t, unsigned int) noexcept { return _Thrd_result::_Success; }

// ===========================================================================
//  std::thread primitives. No threads are spawned on the headless boot path,
//  so these only need to link (join/id return benign values; sleep no-ops).
// ===========================================================================
_Thrd_id_t __cdecl _Thrd_id() noexcept            { return 1; }
_Thrd_result __cdecl _Thrd_join(_Thrd_t, int*) noexcept { return _Thrd_result::_Success; }
void __stdcall _Thrd_sleep_for(unsigned long) noexcept  {}

// ===========================================================================
//  vcruntime trivial-range algorithm helpers (vectorized find/search/reverse).
//  std::find/search on byte/dword-trivial ranges lowers to these. Plain loops.
// ===========================================================================
extern "C" const void* __stdcall __std_find_trivial_1(const void* first, const void* last, uint8_t val) noexcept
{
    const uint8_t* p = static_cast<const uint8_t*>(first);
    const uint8_t* e = static_cast<const uint8_t*>(last);
    for (; p != e; ++p) if (*p == val) return p;
    return e;
}
extern "C" const void* __stdcall __std_find_trivial_4(const void* first, const void* last, uint32_t val) noexcept
{
    const uint32_t* p = static_cast<const uint32_t*>(first);
    const uint32_t* e = static_cast<const uint32_t*>(last);
    for (; p != e; ++p) if (*p == val) return p;
    return e;
}
extern "C" const void* __stdcall __std_find_last_trivial_1(const void* first, const void* last, uint8_t val) noexcept
{
    const uint8_t* b = static_cast<const uint8_t*>(first);
    const uint8_t* p = static_cast<const uint8_t*>(last);
    while (p != b) { --p; if (*p == val) return p; }
    return last;
}
extern "C" const void* __stdcall __std_search_1(const void* first1, const void* last1,
                                                const void* first2, size_t count2) noexcept
{
    const uint8_t* b1 = static_cast<const uint8_t*>(first1);
    const uint8_t* e1 = static_cast<const uint8_t*>(last1);
    const uint8_t* p2 = static_cast<const uint8_t*>(first2);
    if (count2 == 0) return b1;
    for (; (size_t)(e1 - b1) >= count2; ++b1)
        if (std::memcmp(b1, p2, count2) == 0) return b1;
    return e1;
}
extern "C" void __cdecl __std_reverse_trivially_swappable_1(void* first, void* last) noexcept
{
    uint8_t* a = static_cast<uint8_t*>(first);
    uint8_t* b = static_cast<uint8_t*>(last);
    while (a < b) { --b; uint8_t t = *a; *a = *b; *b = t; ++a; }
}
extern "C" size_t __stdcall __std_find_last_of_trivial_pos_1(
    const void* hay, size_t haySize, const void* needle, size_t needleSize) noexcept
{
    const uint8_t* h = static_cast<const uint8_t*>(hay);
    const uint8_t* n = static_cast<const uint8_t*>(needle);
    for (size_t i = haySize; i-- > 0; )
        for (size_t j = 0; j < needleSize; ++j)
            if (h[i] == n[j]) return i;
    return (size_t)-1;   // npos
}

// ===========================================================================
//  std::sto* (string -> integer) backends. Forward to RXDK's _strtoi64 family.
// ===========================================================================
extern "C" __int64          _strtoi64(const char*, char**, int);
extern "C" unsigned __int64 _strtoui64(const char*, char**, int);

extern "C" {
long           __cdecl _Stolx  (const char* s, char** end, int base, int*) { return (long)_strtoi64(s, end, base); }
unsigned long  __cdecl _Stoulx (const char* s, char** end, int base, int*) { return (unsigned long)_strtoui64(s, end, base); }
long long      __cdecl _Stollx (const char* s, char** end, int base, int*) { return (long long)_strtoi64(s, end, base); }
unsigned long long __cdecl _Stoullx(const char* s, char** end, int base, int*) { return (unsigned long long)_strtoui64(s, end, base); }
}

// ===========================================================================
//  C++14 sized operator delete -> forward to the plain one RXDK provides
// ===========================================================================
void __cdecl operator delete(void* p, size_t) noexcept { ::operator delete(p); }
