// ============================================================================
//  xbox_crt_shim.cpp  --  modern UCRT/vcruntime entry points the v143 compiler
//  emits that RXDK's 2003-era CRT does not provide. Most forward to the classic
//  RXDK CRT function; logging-to-stream is dropped (no console on Xbox); a few
//  Win32/winsock entry points are stubbed (those subsystems are disabled).
//
//  This TU deliberately does NOT include <cstdio>/<windows.h> so our extern "C"
//  definitions can use opaque void* where the real headers use FILE*/_locale_t
//  without triggering type-mismatch redeclaration errors. extern "C" symbol
//  names are unaffected by parameter types.
// ============================================================================
#include <cstdarg>
#include <cstddef>

// ---- classic RXDK CRT functions we forward to --------------------------------
extern "C" int    _vsnprintf(char*, size_t, const char*, va_list);
extern "C" double strtod(const char*, char**);
extern "C" __int64          _strtoi64(const char*, char**, int);
extern "C" void* fopen(const char*, const char*);
extern "C" double floor(double);

// ===========================================================================
//  Modern stdio family  (___stdio_common_*).
//  *sprintf* must really format (used to build strings/paths); *fprintf* is a
//  log sink we drop; *sscanf* is stubbed (no field parsed).
// ===========================================================================
extern "C" long long __cdecl __stdio_common_vsprintf(
    unsigned long long /*opts*/, char* buf, size_t n, const char* fmt, void* /*loc*/, va_list args)
{
    return _vsnprintf(buf, n, fmt, args);
}
extern "C" long long __cdecl __stdio_common_vsprintf_s(
    unsigned long long /*opts*/, char* buf, size_t n, const char* fmt, void* /*loc*/, va_list args)
{
    return _vsnprintf(buf, n, fmt, args);
}
extern "C" long long __cdecl __stdio_common_vfprintf(
    unsigned long long /*opts*/, void* /*stream*/, const char* /*fmt*/, void* /*loc*/, va_list /*args*/)
{
    return 0;   // logging output discarded on console
}
extern "C" long long __cdecl __stdio_common_vsscanf(
    unsigned long long /*opts*/, const char* /*buf*/, size_t /*n*/, const char* /*fmt*/, void* /*loc*/, va_list /*args*/)
{
    return 0;   // no fields parsed (best-effort; revisit if a parser needs it)
}
extern "C" void* __cdecl __acrt_iob_func(unsigned i)
{
    // vfprintf is a no-op, so the returned handle is only ever compared, never
    // dereferenced. Hand back distinct non-null dummies for stdin/out/err.
    static char dummy[3];
    return &dummy[i < 3 ? i : 0];
}
extern "C" int __cdecl _get_stream_buffer_pointers(void*, char***, char***, int**)
{
    return -1;  // "no buffer" -- nowide console path is excluded anyway
}

// ===========================================================================
//  Floating-point classification (fpclassify / signbit lowerings).
//  Return the UCRT FP_* codes: NAN=2, INFINITE=1, ZERO=0, SUBNORMAL=-2, NORMAL=-1.
// ===========================================================================
extern "C" short __cdecl _dclass(double x)
{
    union { double d; unsigned long long u; } v; v.d = x;
    unsigned exp = (unsigned)((v.u >> 52) & 0x7FF);
    unsigned long long man = v.u & 0xFFFFFFFFFFFFFULL;
    if (exp == 0x7FF) return man ? (short)2 : (short)1;
    if (exp == 0)     return man ? (short)-2 : (short)0;
    return (short)-1;
}
extern "C" short __cdecl _fdclass(float x)
{
    union { float f; unsigned u; } v; v.f = x;
    unsigned exp = (v.u >> 23) & 0xFF;
    unsigned man = v.u & 0x7FFFFF;
    if (exp == 0xFF) return man ? (short)2 : (short)1;
    if (exp == 0)    return man ? (short)-2 : (short)0;
    return (short)-1;
}
extern "C" short __cdecl _ldclass(long double x) { return _dclass((double)x); }
extern "C" int   __cdecl _fdsign(float x)
{
    union { float f; unsigned u; } v; v.f = x;
    return (v.u & 0x80000000u) ? 1 : 0;
}

// ===========================================================================
//  Math the RXDK CRT lacks (C99)
// ===========================================================================
extern "C" float  fmaf(float a, float b, float c)  { return a * b + c; }
extern "C" long   lround(double x)                 { return (long)(x >= 0 ? floor(x + 0.5) : -floor(-x + 0.5)); }
extern "C" long   lroundf(float x)                 { return lround((double)x); }
extern "C" float  roundf(float x)                  { return (float)(x >= 0 ? floor((double)x + 0.5) : -floor(-(double)x + 0.5)); }
extern "C" float  strtof(const char* s, char** e)  { return (float)strtod(s, e); }
extern "C" long long strtoll(const char* s, char** e, int b) { return (long long)_strtoi64(s, e, b); }

// ===========================================================================
//  fopen_s -> classic fopen
// ===========================================================================
extern "C" int __cdecl fopen_s(void** f, const char* name, const char* mode)
{
    void* h = fopen(name, mode);
    if (f) *f = h;
    return h ? 0 : 22 /*EINVAL*/;
}

// ===========================================================================
//  terminate handlers (with a settable handler so we can diagnose aborts)
// ===========================================================================
typedef void (*fly_term_handler)();
static fly_term_handler g_term_handler = 0;
extern "C" fly_term_handler __cdecl set_terminate(fly_term_handler h) { fly_term_handler o = g_term_handler; g_term_handler = h; return o; }
extern "C" [[noreturn]] void __cdecl terminate()      { if (g_term_handler) g_term_handler(); for (;;) {} }
extern "C" [[noreturn]] void __cdecl __std_terminate() { if (g_term_handler) g_term_handler(); for (;;) {} }
extern "C" [[noreturn]] void __stdcall __std_init_once_link_alternate_names_and_abort() { for (;;) {} }

// ===========================================================================
//  Diagnostic traps: name the abort source (bad CRT param / pure virtual call)
//  instead of the generic CRT abort message.
// ===========================================================================
extern "C" void __stdcall OutputDebugStringA(const char*);
extern "C" void __cdecl _invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, unsigned)
{ OutputDebugStringA("FLYCAST: *** _invalid_parameter (bad CRT arg) ***\n"); for (;;) {} }
extern "C" void __cdecl _invalid_parameter_noinfo()
{ OutputDebugStringA("FLYCAST: *** _invalid_parameter_noinfo ***\n"); for (;;) {} }
extern "C" [[noreturn]] void __cdecl _invalid_parameter_noinfo_noreturn()
{ OutputDebugStringA("FLYCAST: *** _invalid_parameter_noinfo_noreturn ***\n"); for (;;) {} }
extern "C" void* _ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)
extern "C" int __cdecl _purecall()
{
    unsigned a = (unsigned)(size_t)_ReturnAddress();
    static const char hx[] = "0123456789ABCDEF";
    char buf[16];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; ++i) buf[2 + i] = hx[(a >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\n'; buf[11] = '\0';
    OutputDebugStringA("FLYCAST: *** _purecall from ");
    OutputDebugStringA(buf);
    for (;;) {}
}

// Override abort() to reveal WHO called it (cross-ref the .map file).
extern "C" void* _ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)
extern "C" __declspec(noreturn) void __cdecl abort(void)
{
    unsigned a = (unsigned)(size_t)_ReturnAddress();
    static const char hex[] = "0123456789ABCDEF";
    char buf[16];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; ++i) buf[2 + i] = hex[(a >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\n'; buf[11] = '\0';
    OutputDebugStringA("FLYCAST: *** abort() called from ");
    OutputDebugStringA(buf);
    for (;;) {}
}

// ===========================================================================
//  Win32 bits the modern CRT startup probes for -- harmless stubs on Xbox.
// ===========================================================================
extern "C" void*         GetModuleHandle(void*)                 { return (void*)0x10000; }
extern "C" void*         GetProcAddress(void*, const char*)     { return 0; }
extern "C" unsigned long GetVersion(void)                       { return 0x00000005; } // "NT 5.x"
extern "C" int           IsProcessorFeaturePresent(unsigned)    { return 1; }

// ===========================================================================
//  Winsock stubs (networking disabled). stdcall decorations: @0/@4/@16.
// ===========================================================================
extern "C" int __stdcall WSAGetLastError(void)                          { return 0; }
extern "C" int __stdcall closesocket(unsigned int)                      { return 0; }
extern "C" int __stdcall send(unsigned int, const char*, int, int)      { return -1; }
extern "C" unsigned int   __stdcall socket(int, int, int)                            { return ~0u; }   // INVALID_SOCKET
extern "C" int            __stdcall bind(unsigned int, const void*, int)             { return -1; }
extern "C" unsigned int   __stdcall accept(unsigned int, void*, int*)                { return ~0u; }
extern "C" int            __stdcall listen(unsigned int, int)                        { return -1; }
extern "C" int            __stdcall setsockopt(unsigned int, int, int, const char*, int) { return -1; }
extern "C" int            __stdcall ioctlsocket(unsigned int, long, unsigned long*)  { return -1; }
extern "C" unsigned short __stdcall htons(unsigned short v)                          { return (unsigned short)((v >> 8) | (v << 8)); }
extern "C" unsigned long  __stdcall inet_addr(const char*)                           { return 0xFFFFFFFF; }

// ===========================================================================
//  time / stat the RXDK CRT lacks (modern secure / 64-bit variants).
//  Boot-path stubs: zeroed time; "file not found" stat (callers fall back
//  gracefully -- flash is auto-created, BIOS uses reios HLE).
// ===========================================================================
extern "C" int __cdecl _localtime64_s(void* dst, const __int64* /*src*/)
{
    if (dst) { char* p = static_cast<char*>(dst); for (int i = 0; i < 36; ++i) p[i] = 0; }
    return 0;
}
extern "C" int __cdecl _wstat64i32(const wchar_t* /*path*/, void* /*buf*/)
{
    return -1;  // ENOENT
}

// ===========================================================================
//  Float/double -> (u)int64 conversion helpers the compiler emits.
//  On the SSE1-only PIII the value arrives on the x87 stack (ST0); convert with
//  truncation toward zero (C semantics), result in EDX:EAX.
// ===========================================================================
extern "C" __declspec(naked) void _ftol2_sse()
{
    __asm {
        sub  esp, 16
        fnstcw word ptr [esp]          // save FPU control word
        movzx eax, word ptr [esp]
        or   ah, 0x0C                  // RC = 11b -> round toward zero (truncate)
        mov  word ptr [esp+4], ax
        fldcw word ptr [esp+4]
        fistp qword ptr [esp+8]        // store truncated int64
        fldcw word ptr [esp]           // restore control word
        mov  eax, dword ptr [esp+8]
        mov  edx, dword ptr [esp+12]
        add  esp, 16
        ret
    }
}
extern "C" __declspec(naked) void _ftoul2_legacy()
{
    __asm {
        sub  esp, 16
        fnstcw word ptr [esp]
        movzx eax, word ptr [esp]
        or   ah, 0x0C
        mov  word ptr [esp+4], ax
        fldcw word ptr [esp+4]
        fistp qword ptr [esp+8]
        fldcw word ptr [esp]
        mov  eax, dword ptr [esp+8]
        mov  edx, dword ptr [esp+12]
        add  esp, 16
        ret
    }
}

// ===========================================================================
//  std::call_once backing (__std_init_once_*). Single-threaded: always run the
//  initializer, never already-complete. The header references these via the
//  dllimport indirection __imp_<decorated>, so we publish function-pointer
//  variables and /alternatename the __imp_ symbols onto them.
// ===========================================================================
extern "C" int __stdcall _fly_init_once_begin(void** /*once*/, unsigned long /*flags*/, int* pending, void** /*ctx*/)
{
    if (pending) *pending = 1;   // caller must run the one-time init
    return 1;                    // TRUE
}
extern "C" int __stdcall _fly_init_once_complete(void** /*once*/, unsigned long /*flags*/, void* /*ctx*/)
{
    return 1;                    // TRUE
}
extern "C" void* fly_imp_init_once_begin    = (void*)&_fly_init_once_begin;
extern "C" void* fly_imp_init_once_complete = (void*)&_fly_init_once_complete;
// Force-keep the pointer vars so /OPT:REF can't strip them before the
// alternatename binds the __imp_ symbols onto them. (C data 'fly_x' -> '_fly_x'.)
#pragma comment(linker, "/include:_fly_imp_init_once_begin")
#pragma comment(linker, "/include:_fly_imp_init_once_complete")
#pragma comment(linker, "/alternatename:__imp____std_init_once_begin_initialize@16=_fly_imp_init_once_begin")
#pragma comment(linker, "/alternatename:__imp____std_init_once_complete@12=_fly_imp_init_once_complete")

// ===========================================================================
//  Caching aligned allocator (override RXDK's _aligned_malloc/_aligned_free).
//
//  Flycast's addrspace::initMappings() runs on EVERY setPlatform() -- called
//  twice on the boot path (Emulator::init + Emulator::loadGame) -- and each
//  call re-allocates ~90MB of guest memory: 16MB RAM + 8MB VRAM + 2MB ARAM +
//  ~64MB Sh4RCB (its fpcb[] is RAM_SIZE_MAX/2 pointers, used ONLY by the
//  recompiler -- dead weight for our interpreter build) + elan ERAM. The
//  allocators are NOT idempotent (RamRegion::alloc/malloc_pages just overwrite
//  the pointer), so the second call leaks the first ~90MB -> ~180MB total ->
//  exhausts even a 128MB devkit -> malloc never returns -> boot hangs.
//
//  Fix: cache LARGE aligned allocations by size. The second initMappings() then
//  reuses the first call's buffers (it zero-fills them anyway), so total guest
//  memory stays ~90MB. Small allocations pass straight through. Cached buffers
//  are intentionally never freed -- they're the eternal guest address space.
// ===========================================================================
extern "C" void* malloc(size_t);
extern "C" void  free(void*);

namespace {
    struct ACacheEnt { size_t size; void* ptr; };
    ACacheEnt g_acache[24];
    int       g_acount = 0;

    void* aa_raw(size_t size, size_t align)
    {
        if (align < sizeof(void*)) align = sizeof(void*);
        void* raw = malloc(size + align + sizeof(void*));
        if (!raw) return 0;
        size_t a = ((size_t)raw + sizeof(void*) + align - 1) & ~(align - 1);
        ((void**)a)[-1] = raw;               // stash original for free
        return (void*)a;
    }
    void aa_rawfree(void* p) { if (p) free(((void**)p)[-1]); }
}

// Sh4RCB size: 16M FPCB entries × 4 bytes + 64KB pad/context = 64MB + 64KB.
// Commit the lower 32MB (8M entries) of the 64MB FPCB + the 64KB Sh4Context.
// 8M entries cover FPCB indices 0..0x7FFFFF, which maps to ALL 16MB of DC RAM:
//   DC RAM: PC 0x8C000000..0x8CFFFFFF (masked & 0x1FFFFFE) -> idx 0..0x7FFFFF.
// CRITICAL for the JIT: a block compiled at an address whose FPCB index is in
// an UNCOMMITTED page can't store its code pointer -> it recompiles every run
// (thrash). The old 16MB/4M commit covered only the lower 8MB of RAM, so any
// BIOS/game code in the upper 8MB thrashed. 32MB covers the whole RAM. Budget:
// 32MB FPCB + 11MB JIT cache + 26MB RAM/VRAM/ARAM + core ~= 90MB < 128MB devkit.
static const size_t k_sh4rcb_size       = (size_t)64*1024*1024 + 65536;  // 64MB+64KB
static const size_t k_fpcb_lower_bytes  = (size_t)32*1024*1024;          // 32MB committed (8M entries)
static const size_t k_sh4ctx_offset     = (size_t)64*1024*1024;          // context start
static const size_t k_sh4ctx_bytes      = (size_t)65536;                 // 64KB committed

// VirtualAlloc is available after xtl.h (included at the bottom of this file).
// Forward-declare so the compiler doesn't need xtl.h at this point.
extern "C" void* __stdcall VirtualAlloc(void*, unsigned long, unsigned long, unsigned long);
static const unsigned long MEM_RESERVE_  = 0x2000;
static const unsigned long MEM_COMMIT_   = 0x1000;
static const unsigned long PAGE_READWRITE_ = 0x04;
static const unsigned long PAGE_NOACCESS_  = 0x01;

static void* alloc_sh4rcb()
{
    void* p = VirtualAlloc(0, k_sh4rcb_size, MEM_RESERVE_, PAGE_NOACCESS_);
    if (!p) return 0;
    if (!VirtualAlloc(p, k_fpcb_lower_bytes, MEM_COMMIT_, PAGE_READWRITE_))          { return 0; }
    if (!VirtualAlloc((char*)p + k_sh4ctx_offset, k_sh4ctx_bytes, MEM_COMMIT_, PAGE_READWRITE_)) { return 0; }
    return p;
}

extern "C" void* __cdecl _aligned_malloc(size_t size, size_t align)
{
    if (size >= (1u << 20))                  // >= 1MB: cache by size
    {
        for (int i = 0; i < g_acount; ++i)
            if (g_acache[i].size == size) return g_acache[i].ptr;
        void* p = (size == k_sh4rcb_size) ? alloc_sh4rcb() : aa_raw(size, align);
        if (g_acount < 24) { g_acache[g_acount].size = size; g_acache[g_acount].ptr = p; ++g_acount; }
        return p;
    }
    return aa_raw(size, align);
}

extern "C" void __cdecl _aligned_free(void* p)
{
    for (int i = 0; i < g_acount; ++i)
        if (g_acache[i].ptr == p) return;    // cached buffer -> keep it
    aa_rawfree(p);
}
