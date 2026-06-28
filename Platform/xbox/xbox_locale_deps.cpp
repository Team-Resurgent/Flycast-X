// ============================================================================
//  xbox_locale_deps.cpp  --  the CRT/kernel dependencies the real C++ locale
//  objects (pulled from the static language runtime) need, provided in Xbox
//  terms. The kernel32 imports are bridged via __imp_ pointers; the locale data
//  APIs Xbox lacks (GetStringTypeW / LCMapStringEx / ...) are implemented as a
//  real ASCII "C" locale -- this is what actually powers std::tolower etc.
// ============================================================================
#include <cstring>
#include <cstdarg>
extern "C" size_t wcslen(const wchar_t*);   // RXDK CRT (avoid <cwchar> clashes)

typedef unsigned long  DWORD;
typedef unsigned short WORD;
typedef int            BOOL;
typedef void*          HANDLE;

// ---------------------------------------------------------------------------
//  Critical sections: single-threaded boot -> no real locking needed.
// ---------------------------------------------------------------------------
extern "C" void __stdcall xb_EnterCriticalSection(void*) {}
extern "C" void __stdcall xb_LeaveCriticalSection(void*) {}
extern "C" void __stdcall xb_DeleteCriticalSection(void*) {}
extern "C" BOOL __stdcall xb_InitializeCriticalSectionAndSpinCount(void*, DWORD) { return 1; }

// ---------------------------------------------------------------------------
//  Module / process / misc kernel funcs -> Xbox-correct trivial behavior.
// ---------------------------------------------------------------------------
extern "C" HANDLE __stdcall xb_GetCurrentProcess(void)                 { return (HANDLE)(~(unsigned)0); }
extern "C" void*  __stdcall xb_GetProcAddress(void*, const char*)      { return 0; }   // no module system
extern "C" BOOL   __stdcall xb_FreeLibrary(void*)                      { return 1; }
extern "C" void*  __stdcall xb_LoadLibraryExW(const wchar_t*, void*, DWORD) { return 0; } // -> built-in fallback
extern "C" BOOL   __stdcall xb_TerminateProcess(void*, unsigned)       { for(;;){} }
extern "C" BOOL   __stdcall xb_IsProcessorFeaturePresent(DWORD)        { return 1; }
extern "C" void*  __stdcall xb_EncodePointer(void* p)                  { return p; }
extern "C" void*  __stdcall xb_DecodePointer(void* p)                  { return p; }
extern "C" void*  __stdcall xb_InterlockedFlushSList(void*)            { return 0; }
extern "C" void*  __stdcall xb_InterlockedPushEntrySList(void*, void*) { return 0; }

// std::call_once / once-init (single-threaded: always run once, succeed)
extern "C" BOOL __stdcall xb_InitOnceBeginInitialize(void*, DWORD, BOOL* pending, void**) { if (pending) *pending = 1; return 1; }
extern "C" BOOL __stdcall xb_InitOnceComplete(void*, DWORD, void*) { return 1; }

// ---------------------------------------------------------------------------
//  Locale data APIs Xbox lacks -> real ASCII "C" locale.
// ---------------------------------------------------------------------------
// ctype classification bits (Win32 C1_*)
enum { C1_UPPER=1, C1_LOWER=2, C1_DIGIT=4, C1_SPACE=8, C1_PUNCT=16, C1_CNTRL=32, C1_BLANK=64, C1_XDIGIT=128, C1_ALPHA=256 };

extern "C" BOOL __stdcall xb_GetStringTypeW(DWORD /*infoType*/, const wchar_t* src, int count, WORD* out)
{
    if (count < 0) { count = (int)wcslen(src) + 1; }
    for (int i = 0; i < count; ++i) {
        wchar_t c = src[i]; WORD t = 0;
        if (c >= L'A' && c <= L'Z') t |= C1_UPPER | C1_ALPHA;
        else if (c >= L'a' && c <= L'z') t |= C1_LOWER | C1_ALPHA;
        if (c >= L'0' && c <= L'9') t |= C1_DIGIT | C1_XDIGIT;
        else if ((c>=L'a'&&c<=L'f')||(c>=L'A'&&c<=L'F')) t |= C1_XDIGIT;
        if (c == L' ' || (c >= 9 && c <= 13)) t |= C1_SPACE;
        if (c == L' ' || c == L'\t') t |= C1_BLANK;
        if (c < 32 || c == 127) t |= C1_CNTRL;
        if (c >= 33 && c <= 126 && !(t & (C1_ALPHA|C1_DIGIT))) t |= C1_PUNCT;
        out[i] = t;
    }
    return 1;
}

enum { LCMAP_LOWERCASE=0x100, LCMAP_UPPERCASE=0x200 };
extern "C" int __stdcall xb_LCMapStringEx(const wchar_t*, DWORD flags, const wchar_t* src, int srcLen,
                                          wchar_t* dst, int dstLen, void*, void*, long)
{
    if (srcLen < 0) srcLen = (int)wcslen(src) + 1;
    if (!dst || dstLen == 0) return srcLen;
    int n = srcLen < dstLen ? srcLen : dstLen;
    for (int i = 0; i < n; ++i) {
        wchar_t c = src[i];
        if ((flags & LCMAP_LOWERCASE) && c >= L'A' && c <= L'Z') c += 32;
        else if ((flags & LCMAP_UPPERCASE) && c >= L'a' && c <= L'z') c -= 32;
        dst[i] = c;
    }
    return n;
}

extern "C" int __stdcall xb_CompareStringEx(const wchar_t*, DWORD, const wchar_t* s1, int n1,
                                            const wchar_t* s2, int n2, void*, void*, long)
{
    if (n1 < 0) n1 = (int)wcslen(s1);
    if (n2 < 0) n2 = (int)wcslen(s2);
    int n = n1 < n2 ? n1 : n2;
    for (int i = 0; i < n; ++i) { if (s1[i] != s2[i]) return s1[i] < s2[i] ? 1 : 3; }
    if (n1 == n2) return 2;          // CSTR_EQUAL
    return n1 < n2 ? 1 : 3;          // LESS_THAN / GREATER_THAN
}

extern "C" int __stdcall xb_GetLocaleInfoEx(const wchar_t*, DWORD, wchar_t* data, int cch)
{
    if (data && cch > 0) data[0] = 0;
    return 0;                        // "C" defaults -> STL uses built-ins
}

extern "C" BOOL __stdcall xb_GetCPInfo(unsigned, void* cpInfo)
{
    if (cpInfo) { unsigned char* p = (unsigned char*)cpInfo; std::memset(p, 0, 20); p[0] = 1; } // MaxCharSize=1
    return 1;
}

// ---------------------------------------------------------------------------
//  UCRT locale helpers
// ---------------------------------------------------------------------------
extern "C" const wchar_t* __cdecl xb_W_Getempty() { static const wchar_t e[] = L""; return e; }
extern "C" void* __cdecl _W_Getdays()   { return (void*)xb_W_Getempty(); }
extern "C" void* __cdecl _W_Getmonths() { return (void*)xb_W_Getempty(); }
extern "C" void* __cdecl _W_Gettnames() { return (void*)0; }
extern "C" size_t __cdecl _Wcsftime(wchar_t* s, size_t n, const wchar_t*, const void*, void*) { if (s && n) s[0]=0; return 0; }
extern "C" const char* __cdecl ___lc_locale_name_func() { return 0; }
extern "C" size_t __cdecl __strncnt(const char* s, size_t n) { size_t i=0; while (i<n && s[i]) ++i; return i; }
extern "C" size_t __cdecl wcsnlen(const wchar_t* s, size_t n) { size_t i=0; while (i<n && s[i]) ++i; return i; }
extern "C" int __cdecl strcpy_s(char* d, size_t n, const char* s) { if(!d||!n) return 22; size_t i=0; for(;s[i]&&i<n-1;++i) d[i]=s[i]; d[i]=0; return 0; }
extern "C" int _vsnwprintf(wchar_t*, size_t, const wchar_t*, va_list);   // RXDK
extern "C" int __cdecl vswprintf(wchar_t* b, size_t n, const wchar_t* f, va_list a) { return _vsnwprintf(b, n, f, a); }
extern "C" int __cdecl _vswprintf(wchar_t* b, const wchar_t* f, va_list a)          { return _vsnwprintf(b, 0x7fffffff, f, a); }

// ===========================================================================
//  Bridge the dllimport (__imp_) thunks onto the functions above. Each
//  __imp_<decorated> is a POINTER to the function, force-kept so /OPT:REF
//  can't drop it before /alternatename binds.
// ===========================================================================
#define FLY_IMP(impname, ptrname, fn) \
    extern "C" void* ptrname = (void*)&fn; \
    __pragma(comment(linker, "/include:_" #ptrname)) \
    __pragma(comment(linker, "/alternatename:" impname "=_" #ptrname))

FLY_IMP("__imp__EnterCriticalSection@4",                  imp_EnterCS,    xb_EnterCriticalSection)
FLY_IMP("__imp__LeaveCriticalSection@4",                  imp_LeaveCS,    xb_LeaveCriticalSection)
FLY_IMP("__imp__DeleteCriticalSection@4",                 imp_DeleteCS,   xb_DeleteCriticalSection)
FLY_IMP("__imp__InitializeCriticalSectionAndSpinCount@8", imp_InitCS,     xb_InitializeCriticalSectionAndSpinCount)
FLY_IMP("__imp__GetCurrentProcess@0",                     imp_GetCurProc, xb_GetCurrentProcess)
FLY_IMP("__imp__GetProcAddress@8",                        imp_GetProcAddr,xb_GetProcAddress)
FLY_IMP("__imp__FreeLibrary@4",                           imp_FreeLib,    xb_FreeLibrary)
FLY_IMP("__imp__LoadLibraryExW@12",                       imp_LoadLibW,   xb_LoadLibraryExW)
FLY_IMP("__imp__TerminateProcess@8",                      imp_TermProc,   xb_TerminateProcess)
FLY_IMP("__imp__IsProcessorFeaturePresent@4",             imp_IsProcFeat, xb_IsProcessorFeaturePresent)
FLY_IMP("__imp__EncodePointer@4",                         imp_EncPtr,     xb_EncodePointer)
FLY_IMP("__imp__DecodePointer@4",                         imp_DecPtr,     xb_DecodePointer)
FLY_IMP("__imp__InterlockedFlushSList@4",                 imp_FlushSList, xb_InterlockedFlushSList)
FLY_IMP("__imp__InterlockedPushEntrySList@8",             imp_PushSList,  xb_InterlockedPushEntrySList)
FLY_IMP("__imp__InitOnceBeginInitialize@16",              imp_InitOnceB,  xb_InitOnceBeginInitialize)
FLY_IMP("__imp__InitOnceComplete@12",                     imp_InitOnceC,  xb_InitOnceComplete)
FLY_IMP("__imp__GetStringTypeW@16",                       imp_GetStrType, xb_GetStringTypeW)
FLY_IMP("__imp__LCMapStringEx@36",                        imp_LCMapEx,    xb_LCMapStringEx)
FLY_IMP("__imp__CompareStringEx@36",                      imp_CompareEx,  xb_CompareStringEx)
FLY_IMP("__imp__GetLocaleInfoEx@16",                      imp_GetLocInfo, xb_GetLocaleInfoEx)
FLY_IMP("__imp__GetCPInfo@8",                             imp_GetCPInfo,  xb_GetCPInfo)
