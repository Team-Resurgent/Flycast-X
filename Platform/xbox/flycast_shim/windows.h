// flycast_shim/windows.h -- map desktop <windows.h> to the Xbox XTL.
// Flycast's windows-only files (ConsoleListenerWin.cpp -> OutputDebugStringA,
// oslib/unwind_info.h -> include only, multiboard.h, etc.) include <windows.h>;
// on RXDK the Win32-ish surface lives in xtl.h.
#pragma once
#include <xtl.h>

// RXDK's WinBase.h defines P/LPSECURITY_ATTRIBUTES (as LPVOID) but NOT the
// SECURITY_ATTRIBUTES value type that multiboard.h declares as a local. Provide
// just that (don't touch the pointer typedefs -- they already exist).
#ifndef _FLY_SECURITY_ATTRIBUTES_DEFINED
#define _FLY_SECURITY_ATTRIBUTES_DEFINED
typedef struct _FLY_SECURITY_ATTRIBUTES {
    DWORD  nLength;
    LPVOID lpSecurityDescriptor;
    BOOL   bInheritHandle;
} SECURITY_ATTRIBUTES;
#endif

// Win32 console API: only referenced by serial.cpp's optional _WIN32 serial-
// debug console (config::SerialConsole, off on Xbox). Stub as no-ops so it
// compiles; it never executes.
#ifndef STD_OUTPUT_HANDLE
#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)
#endif
#ifdef __cplusplus
inline BOOL   AllocConsole(void)               { return FALSE; }
inline BOOL   SetConsoleTitleA(LPCSTR)         { return FALSE; }
inline HANDLE GetStdHandle(DWORD)              { return (HANDLE)0; }
inline BOOL   SetStdHandle(DWORD, HANDLE)      { return FALSE; }
#else
static __inline BOOL   AllocConsole(void)          { return FALSE; }
static __inline BOOL   SetConsoleTitleA(LPCSTR t)  { (void)t; return FALSE; }
static __inline HANDLE GetStdHandle(DWORD n)       { (void)n; return (HANDLE)0; }
static __inline BOOL   SetStdHandle(DWORD n, HANDLE h) { (void)n; (void)h; return FALSE; }
#endif
#ifndef SetConsoleTitle
#define SetConsoleTitle SetConsoleTitleA
#endif

// OSVERSIONINFO / GetVersionEx: lzma CpuArch.c uses them to gate SSE on the OS
// version. Stub GetVersionEx to fail -> the deps fall back to scalar code paths
// (correct on the SSE1-only Xbox CPU).
#ifndef _FLY_OSVERSIONINFO_DEFINED
#define _FLY_OSVERSIONINFO_DEFINED
typedef struct _FLY_OSVERSIONINFOA {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    char  szCSDVersion[128];
} OSVERSIONINFOA, OSVERSIONINFO, *POSVERSIONINFOA, *LPOSVERSIONINFOA;
#ifdef __cplusplus
inline BOOL GetVersionExA(LPOSVERSIONINFOA p) { (void)p; return FALSE; }
#else
static __inline BOOL GetVersionExA(LPOSVERSIONINFOA p) { (void)p; return FALSE; }
#endif
#ifndef GetVersionEx
#define GetVersionEx GetVersionExA
#endif
#endif // _FLY_OSVERSIONINFO_DEFINED

// SYSTEM_INFO / GetSystemInfo: the Xbyak JIT assembler uses it for the page size.
// Xbox pages are 4KB; allocation granularity 64KB.
#ifndef _FLY_SYSTEM_INFO_DEFINED
#define _FLY_SYSTEM_INFO_DEFINED
typedef struct _FLY_SYSTEM_INFO {
    DWORD          dwOemId;
    DWORD          dwPageSize;
    LPVOID         lpMinimumApplicationAddress;
    LPVOID         lpMaximumApplicationAddress;
    size_t         dwActiveProcessorMask;
    DWORD          dwNumberOfProcessors;
    DWORD          dwProcessorType;
    DWORD          dwAllocationGranularity;
    unsigned short wProcessorLevel;
    unsigned short wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;
#ifdef __cplusplus
inline void GetSystemInfo(LPSYSTEM_INFO si)
#else
static __inline void GetSystemInfo(LPSYSTEM_INFO si)
#endif
{
    if (si) {
        si->dwPageSize = 4096;
        si->dwAllocationGranularity = 65536;
        si->dwNumberOfProcessors = 1;
    }
}
#endif // _FLY_SYSTEM_INFO_DEFINED
