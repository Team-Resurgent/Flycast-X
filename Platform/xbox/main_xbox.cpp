// ============================================================================
//  main_xbox.cpp  --  Flycast-on-Xbox entry point + headless boot driver.
//
//  Flycast runs with the NULL renderer (NO_REND), so it produces no picture.
//  We drive D3D8 ourselves purely as a STATUS INDICATOR so a single glance at
//  xemu tells us how far the boot got:
//      BLUE   = D3D up, about to bring up the emulator
//      YELLOW = emu.init + loadGame + start succeeded, running SH-4 frames
//      GREEN  = ran 120 frames of the Dreamcast core with no crash  ✅
//      RED    = a FlycastException/std::exception escaped            ❌
// ============================================================================
#include <vector>
#include <exception>
#include <sstream>
#include <locale>
#include <string>
#include <cstdio>
#include <cctype>
#include <cerrno>
#include <cstdlib>

#include "build.h"
#include "types.h"
#include "emulator.h"
#include "cfg/option.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"   // sh4_sched_now64() -- emulated-time clock for realtime pacing
#include "hw/mem/mem_watch.h"
#include "audio/audiostream.h"
#include "stdclass.h"
#include "oslib/oslib.h"
#include "log/LogManager.h"

#include <xtl.h>
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

// Debug output -> captured by xbwatson / the Xbox debug monitor.
extern "C" void __stdcall OutputDebugStringA(const char*);
#define DBG(s) OutputDebugStringA(s)


// Pipe Flycast's own logging (NOTICE_LOG/INFO_LOG/...) to the debug monitor so
// we can see exactly how far dc_reset/loadGame gets.
class XbeDbgLog : public LogListener
{
public:
    void Log(LogTypes::LOG_LEVELS, const char* msg) override { OutputDebugStringA(msg); }
};

static void setupLogging()
{
    // ConsoleListener::Log() already pipes to OutputDebugStringA, so once the
    // LogManager exists and CONSOLE_LISTENER is enabled, Flycast's own logs
    // appear in xbwatson. Print around each call to find any hang.
    DBG("FLYCAST: LogManager::Init()...\n");
    LogManager::Init();
    DBG("FLYCAST: LogManager::Init() returned\n");
    LogManager* lm = LogManager::GetInstance();
    if (!lm) { DBG("FLYCAST: LogManager is null\n"); return; }
    DBG("FLYCAST: SetLogLevel...\n");
    // WARNING for speed: every log line goes out OutputDebugStringA (serial, slow)
    // and stalls the emulator. INFO was for tracing the MvC2 boot hang (fixed:
    // RAM-mirror fault). Raise to LINFO + re-enable categories below if you need
    // to trace a new boot hang again.
    lm->SetLogLevel(LogTypes::LWARNING);
    static const LogTypes::LOG_TYPE kQuiet[] = {
        LogTypes::AICA, LogTypes::AICA_ARM, LogTypes::AUDIO, LogTypes::DYNAREC,
        LogTypes::INPUT, LogTypes::INTERPRETER, LogTypes::JVS, LogTypes::MAPLE,
        LogTypes::PVR, LogTypes::RENDERER, LogTypes::SH4, LogTypes::PROFILER,
        LogTypes::MODEM, LogTypes::NETWORK, LogTypes::SAVESTATE,
    };
    for (size_t i = 0; i < sizeof(kQuiet) / sizeof(kQuiet[0]); i++)
        lm->SetEnable(kQuiet[i], false);
    DBG("FLYCAST: EnableListener(console)...\n");
    lm->EnableListener(LogListener::CONSOLE_LISTENER, true);
    DBG("FLYCAST: logging ready\n");
}

static void DBGHEX(const char* tag, unsigned v)
{
    static const char hx[] = "0123456789ABCDEF";
    char b[96]; int i = 0;
    while (tag[i]) { b[i] = tag[i]; ++i; }
    b[i++] = '0'; b[i++] = 'x';
    for (int k = 0; k < 8; ++k) b[i++] = hx[(v >> ((7 - k) * 4)) & 0xF];
    b[i++] = '\n'; b[i] = 0;
    OutputDebugStringA(b);
}

// SEH capture: an access violation in loadGame/dc_reset is a STRUCTURED
// exception, not a C++ one, so our catch(...) misses it. Catch it here and
// print the faulting code+address (map via image = runtime + 0x3EF000).
extern "C" int xbox_FastmemFault(struct _EXCEPTION_POINTERS* ep);
extern "C" volatile unsigned g_fault_total;   // xbox_fault_handler.cpp
// Per-category fault counters (xbox_fault_handler.cpp). Breaking the storm down
// by kind tells us what to optimize: fpcb=JIT block-table demand paging,
// rewrite=fastmem->slow MMIO patch sites, ramsmc=self-modifying-code unprotects,
// vram=texture/framebuffer invalidations, unhandled=genuine (crash) faults.
extern "C" volatile unsigned g_fault_fpcb;
extern "C" volatile unsigned g_fault_rewrite;
extern "C" volatile unsigned g_fault_ramsmc;
extern "C" volatile unsigned g_fault_vram;
extern "C" volatile unsigned g_fault_unhandled;
extern "C" volatile unsigned g_fault_lastaddr;
extern "C" volatile unsigned g_fault_lastpc;

// Component profiler: rdtsc cycles accumulated inside the interpreted hot paths so
// the PERF line can show where a frame's time actually goes (SH-4 JIT vs the still-
// interpreted ARM7/AICA vs render vs texture). rdtsc runs at the HOST clock under
// xemu (not 733MHz), so we calibrate cycles->us against QPC at startup.
#include <intrin.h>
extern "C" volatile long long g_prof_arm_cyc  = 0;   // ARM7 sound CPU (interpreted)
extern "C" volatile long long g_prof_aica_cyc = 0;   // AICA sample gen + DSP (interpreted)
extern "C" volatile long long g_prof_tex_cyc  = 0;   // texture upload + format convert
extern "C" volatile long long g_prof_ta_cyc   = 0;   // TA FIFO copy+FSM (ta.cpp, part of "sh4")
extern "C" unsigned           g_cnt_ta        = 0;   // ta_vtx_data32 calls (32-byte chunks)

// Renderer::Process / ta_parse time (part of "sh4"). QPC-based: the rdtsc-delta
// version printed garbage on real hardware (the known rdtsc probe artifact).
extern "C" unsigned g_stat_parseUs = 0;
static LARGE_INTEGER s_parseT0;
extern "C" void xbox_parseProbeBegin()
{
    QueryPerformanceCounter(&s_parseT0);
}
extern "C" void xbox_parseProbeEnd()
{
    static LARGE_INTEGER s_qf = { 0 };
    if (s_qf.QuadPart == 0)
        QueryPerformanceFrequency(&s_qf);
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    g_stat_parseUs += (unsigned)((t1.QuadPart - s_parseT0.QuadPart) * 1000000 / s_qf.QuadPart);
}

// Nested probe inside parse: index building + translucency sort (ta_vtx.cpp
// parseRenderPass). parse/f minus sort/f = the TaCmd vertex-decode share.
extern "C" unsigned g_stat_sortUs = 0;
static LARGE_INTEGER s_sortT0;
extern "C" void xbox_sortProbeBegin()
{
    QueryPerformanceCounter(&s_sortT0);
}
extern "C" void xbox_sortProbeEnd()
{
    static LARGE_INTEGER s_qf = { 0 };
    if (s_qf.QuadPart == 0)
        QueryPerformanceFrequency(&s_qf);
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    g_stat_sortUs += (unsigned)((t1.QuadPart - s_sortT0.QuadPart) * 1000000 / s_qf.QuadPart);
}

// Second nested split of parse/f (fights spend ~1.3-1.9ms in "decode" =
// parse - sort, and GetTexture runs INSIDE the decode loop): dec/f = the TaCmd
// vertex-decode while-loop; texlk/f = renderer GetTexture (cache lookup +
// volatile-texture hashing + reconvert + upload); hashKB/w = bytes hashed by
// the volatile hash-skip in TexCache::Update. dec - texlk = pure vertex
// conversion cost. Xbox QPC is a direct rdtsc wrapper (no user/kernel
// transition), so the per-poly texlk probe (~500 QPC/frame) stays ~20-30us.
extern "C" unsigned g_stat_decodeUs = 0;
static LARGE_INTEGER s_decodeT0;
extern "C" void xbox_decodeProbeBegin()
{
    QueryPerformanceCounter(&s_decodeT0);
}
extern "C" void xbox_decodeProbeEnd()
{
    static LARGE_INTEGER s_qf = { 0 };
    if (s_qf.QuadPart == 0)
        QueryPerformanceFrequency(&s_qf);
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    g_stat_decodeUs += (unsigned)((t1.QuadPart - s_decodeT0.QuadPart) * 1000000 / s_qf.QuadPart);
}

extern "C" unsigned g_stat_texLkUs = 0;
static LARGE_INTEGER s_texLkT0;
extern "C" void xbox_texLkProbeBegin()
{
    QueryPerformanceCounter(&s_texLkT0);
}
extern "C" void xbox_texLkProbeEnd()
{
    static LARGE_INTEGER s_qf = { 0 };
    if (s_qf.QuadPart == 0)
        QueryPerformanceFrequency(&s_qf);
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    g_stat_texLkUs += (unsigned)((t1.QuadPart - s_texLkT0.QuadPart) * 1000000 / s_qf.QuadPart);
}

extern "C" unsigned g_stat_volHashBytes = 0;    // bytes hashed by volatile hash-skip (TexCache.cpp)

// Idle-loop skip (consumed by rec_x86.cpp block prologues; 0 = off). Set per
// game ID after loadGame. Found via the HOTBLOCKS JIT audit: MvC2 spends ~55%
// of guest time spinning in its wait-for-vblank loop; draining the timeslice
// on the poll block fast-forwards guest time through the scheduler instead of
// emulating no-ops. Semantically transparent (loop exits at the same guest
// time the vblank ISR sets the flag).
extern "C" u32 g_idleSkipPc = 0;
extern "C" u16 g_idleSkipOp = 0;    // first SH4 word of the verified poll loop (guard)

// Auto-detected idle loops (filled by bm_AutoIdleScan in blockmanager.cpp;
// consumed by rec_x86.cpp). Works for every game -- no per-game table needed.
// 32 entries: BIOS boot + menus alone armed 8 in Crazy Taxi and filled the
// original table before gameplay started. g_autoIdleOp records the poll
// loop's first instruction so overlay-replaced code at an armed pc is never
// wrongly drained.
extern "C" u32 g_autoIdlePc[32] = {};
extern "C" u16 g_autoIdleOp[32] = {};
extern "C" u32 g_autoIdleCount = 0;

extern "C" volatile unsigned  g_arm_fallbacks = 0;   // ARM7 JIT -> interpreter fallbacks

// ARM7 JIT execution-shape counters (emitted incs, gated by kArmJitCounters in
// arm7_rec_x86.cpp): blocks entered, flag-saving ops, conditional ops.
extern "C" unsigned g_cnt_armBlk  = 0;
extern "C" unsigned g_cnt_armSop  = 0;
extern "C" unsigned g_cnt_armCond = 0;

// MMU diagnostics (mmu.cpp): direct measurement of mmu_data_translation, since
// the TLB last-hit cache showed no measurable change and indirect (game vs
// game) comparison isn't pinning down the real cost driver.
extern "C" volatile long long g_prof_mmu_cyc  = 0;
extern "C" volatile unsigned  g_prof_mmu_calls = 0;
extern "C" volatile unsigned  g_prof_mmu_slow  = 0;

// ---- Sampling profiler ----------------------------------------------------
// Every ~2ms a background thread freezes the emu thread (DmSuspendThread, the
// proven watchdog technique), reads its host EIP, and buckets it: SH4 JIT block
// bodies vs generated memory handlers vs ARM7 JIT vs C code (by 4K page).
// This is the measurement that ends the guessing: three structural JIT
// optimizations (deferred MMU spills, TLB cache, inline dispatch) all measured
// ZERO, so the time must be inside the block bodies or in C helpers -- this
// says which, with addresses.
// RESULT (2026-07-02): the sampler is BLIND -- ~100% of samples land on kernel
// page 0x8001f regardless of what the emu thread is doing, because
// DmGetThreadContext on a running thread returns the kernel's APC-delivery
// context, not the interrupted user EIP. (This also means the watchdog's
// "HOST EIP" reads were trap-frame artifacts.) Left off; superseded by the
// emitted execution-shape counters (g_cnt_* below).
static const bool kSampleProfiler = false;

// Execution-shape counters, bumped by `inc` instructions EMITTED INTO the JIT
// code itself (rec_x86.cpp block prologue, x86_ops.cpp mem handlers + ifb) --
// this cannot lie about what the generated code executes.
extern "C" unsigned g_cnt_block = 0;   // SH4 block entries
extern "C" unsigned g_cnt_memh  = 0;   // generated memory-handler entries
extern "C" unsigned g_cnt_ifb   = 0;   // interpreter fallbacks (shop_ifb)

// MMU block-linking diagnostics (driver.cpp rdv_LinkBlock): stub = link-stub
// invocations (should collapse to ~0 in steady scenes once edges are wired);
// wired = links persisted; rej = persistence rejected (syscall traps etc.,
// permanent slow edges).
extern "C" unsigned g_cnt_linkStub  = 0;
extern "C" unsigned g_cnt_linkWired = 0;
extern "C" unsigned g_cnt_linkRej   = 0;

// Compile-time emission counters (rec_x86.cpp relinkBlock) + live MMU state:
// diagnoses whether gameplay blocks are even compiled in MMU mode. wired=0 +
// rej=0 + MMU calls/f=0 all along suggests WinCE may run gameplay with the
// MMU OFF -- which would reframe the entire MMU optimization campaign.
extern "C" unsigned g_cnt_emitMmuLink  = 0;
extern "C" unsigned g_cnt_emitMmuNoUpd = 0;
extern "C" unsigned g_cnt_emitNonMmu   = 0;
extern bool mmuOn;   // hw/sh4/modules/mmu.cpp

// Texture-cache accounting (xbox_d3d8_renderer.cpp). Drives the hard byte
// budget below: MvC2's sprite streaming can outgrow RAM (hardware run showed
// freeMB 27 -> 0 -> crash across a session), so evict deterministically by
// BYTES before the system runs dry, instead of only reacting to low freeMB
// (by which time fragmentation and other allocations are already failing).
extern "C" unsigned g_texCacheBytes;
extern "C" unsigned g_texCacheCount;
static const unsigned kTexBudgetBytes = 8u << 20;   // 8MB of D3D textures

// Code-range registration (assigned by driver.cpp / x86_ops.cpp / arm7_rec.cpp).
extern "C" u8 *g_sh4CacheStart = nullptr;
extern "C" u32 g_sh4CacheSize = 0;
extern "C" const u8 *g_memHandlerStart = nullptr;
extern "C" const u8 *g_memHandlerEnd = nullptr;
extern "C" u8 *g_arm7CacheStart = nullptr;
extern "C" u32 g_arm7CacheSize = 0;

// Sample buckets (reset every PERF window by the render thread; races with the
// sampler are acceptable for diagnostics).
static volatile unsigned g_smp_block = 0;   // SH4 JIT compiled block bodies (+dispatch blob)
static volatile unsigned g_smp_memh  = 0;   // generated memory-access handlers
static volatile unsigned g_smp_arm7  = 0;   // ARM7 JIT cache
static volatile unsigned g_smp_c     = 0;   // static C/C++ code
static volatile u32      g_smp_pages[48][2]; // C samples by 4K page: {page, count}

// Anchors: runtime addresses of known subsystems, printed once, so the C-page
// histogram can be attributed to functions straight from the log.
int UpdateSystem_INTC();
namespace aica { namespace sgc { void AICA_Sample(); } }

// Render diagnostics (xbox_d3d8_renderer.cpp): what is "rend" cost actually
// bound by -- draw-call count, state-change count, or vertex/index throughput?
extern "C" volatile unsigned g_stat_drawCalls;
extern "C" volatile unsigned g_stat_stateCalls;
extern "C" volatile unsigned g_stat_stateSkipped;
extern "C" volatile unsigned g_stat_verts;
extern "C" volatile unsigned g_stat_idx;
extern "C" volatile unsigned g_stat_polys;
extern "C" unsigned g_stat_vbLockUs;   // GPU-sync stall time in VB/IB Lock
extern "C" unsigned g_stat_volatileTex;	// uploads that skipped re-protection (TexCache.cpp)
extern "C" unsigned g_stat_volSkip;		// volatile updates skipped via content hash (TexCache.cpp)

static unsigned g_sehCode = 0, g_sehAddr = 0;
static int sehFilter(struct _EXCEPTION_POINTERS* ep)
{
    // fastmem fast-path rewrite / FPCB demand-commit / SMC+texture protection.
    // Resolving returns CONTINUE_EXECUTION -> resume the faulting instruction
    // WITHOUT unwinding (the RWX-origin unwind path still hangs, and fastmem
    // never needs it).
    if (xbox_FastmemFault(ep) == EXCEPTION_CONTINUE_EXECUTION)
        return EXCEPTION_CONTINUE_EXECUTION;

    g_sehCode = ep->ExceptionRecord->ExceptionCode;
    g_sehAddr = (unsigned)(size_t)ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

static LPDIRECT3D8       g_d3d = NULL;
static LPDIRECT3DDEVICE8 g_dev = NULL;

// Shared with the D3D8 renderer (xbox_d3d8_renderer.cpp). Non-static so it can
// extern this. Set once the device is created.
IDirect3DDevice8* g_xbox_d3d_dev = NULL;

static void clearScreen(D3DCOLOR c)
{
    if (!g_dev) return;
    g_dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, c, 1.0f, 0);
    g_dev->Present(NULL, NULL, NULL, NULL);
}

static bool initD3D()
{
    g_d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!g_d3d) return false;

    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth                 = 640;
    pp.BackBufferHeight                = 480;
    pp.BackBufferFormat                = D3DFMT_LIN_X8R8G8B8;
    pp.BackBufferCount                 = 1;
    pp.EnableAutoDepthStencil          = TRUE;
    pp.AutoDepthStencilFormat          = D3DFMT_D24S8;
    pp.SwapEffect                      = D3DSWAPEFFECT_COPY;   // persistent backbuffer (like the PC framebuffer)
    pp.FullScreen_RefreshRateInHz      = 60;
    // IMMEDIATE (no vsync): INTERVAL_ONE blocks Present() until vblank, quantizing
    // the frame rate to 60/N (so ~42ms of real work showed as 50ms=20fps, ~21ms as
    // 33ms=30fps). Uncapping presents the true rate -- the menu's ~42ms -> ~24fps,
    // the boot's ~21ms -> ~45fps. (May tear; fine for now / can cap to game rate later.)
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    Direct3D_SetPushBufferSize(768 * 1024, 128 * 1024);

    if (FAILED(g_d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev)))
        return false;
    g_xbox_d3d_dev = g_dev;     // hand the device to the renderer
    return true;
}

// SEH-wrapped emulator boot. Returns true on success; on a structured
// exception, fills g_sehCode/g_sehAddr and returns false. No locals with
// destructors here (emu calls take const char*/no args) -> no /EH unwind clash.
extern Renderer* rend_DirectX9();   // our D3D8 renderer (xbox_d3d8_renderer.cpp)

// fastmem / SMC / FPCB-demand fault handler installer (xbox_fault_handler.cpp).
void os_InstallFaultHandler();
// Xbox controller -> DC maple input (xbox_input.cpp).
void xbox_InitInput();
void xbox_PollInput();
// Pre-boot file browser (xbox_filebrowser.cpp). Returns the chosen disc-image
// path, or "" to boot the Dreamcast BIOS/dashboard.
extern "C" const char* xbox_RunFileBrowser();
// CPU mode picked in the browser (X toggle): true = dynarec/JIT, false = interpreter.
extern "C" bool xbox_UseDynarec();

// --- Watchdog -------------------------------------------------------------
// Catches a render()/JIT hang (main loop fully frozen, e.g. MvC2 halting at
// 8c1742c0). A separate thread watches a beat counter and, if it stops, dumps
// the SH-4 pc + R0..R15 + PR, then the ASCII of the RAM that R2/R3 point at --
// MvC2 halts in an error loop printing a "---" banner, so this should reveal
// the actual error message text and tell us WHY it bailed.
namespace addrspace { extern u8* ram_base; }   // fastmem base (read guest RAM)

// Xbox debug-monitor thread API (xbdm.lib) -- lets the watchdog read the FROZEN
// emu thread's HOST x86 EIP, to see where the dynarec is actually stuck (a JIT
// block vs the block compiler vs a C++ helper).
// USE_XBDM MUST stay 0 for shippable builds: an XBE that imports xbdm cannot be
// loaded by a retail kernel -- it bounces straight back to the dashboard before
// any of our code runs. Enable only for devkit/debug-BIOS diagnostic sessions.
#define USE_XBDM 0
#if USE_XBDM
extern "C" {
    long __stdcall DmSuspendThread(unsigned long dwThreadId);
    long __stdcall DmResumeThread(unsigned long dwThreadId);
    long __stdcall DmGetThreadContext(unsigned long dwThreadId, CONTEXT* pCtx);
}
#else
// Retail-safe stubs: same signatures, no xbdm import. GetThreadContext reports
// failure (Eip stays 0), which every caller already treats as "no sample".
static long DmSuspendThread(unsigned long) { return 0; }
static long DmResumeThread(unsigned long) { return 0; }
static long DmGetThreadContext(unsigned long, CONTEXT*) { return -1; }
#endif
static unsigned long g_emuTid = 0;   // emu (main) thread id, set before the loop
extern "C" volatile unsigned g_uintc_calls;   // scheduler-tick counter (sh4_interpreter.cpp)

// Dump `count` SH-4 instruction words (u16) at `vaddr` as hex, to decode the loop.
static void dumpGuestHex16(const char* tag, u32 vaddr, int count)
{
    OutputDebugStringA(tag);
    if (addrspace::ram_base == nullptr) { OutputDebugStringA("(no ram_base)\n"); return; }
    const u16* p = (const u16*)(addrspace::ram_base + 0x0C000000u + (vaddr & 0x00FFFFFFu));
    char line[96]; int n = 0;
    for (int i = 0; i < count; i++)
    {
        n += wsprintfA(line + n, "%04x ", p[i]);
        if ((i & 7) == 7) { line[n++] = '\n'; line[n] = 0; OutputDebugStringA(line); n = 0; }
    }
    if (n) { line[n++] = '\n'; line[n] = 0; OutputDebugStringA(line); }
}

// Dump `bytes` of guest RAM at virtual addr `vaddr` as ASCII. RAM only.
static void dumpGuestAscii(const char* tag, u32 vaddr, int bytes)
{
    OutputDebugStringA(tag);
    if (addrspace::ram_base == nullptr) { OutputDebugStringA("(no ram_base)\n"); return; }
    const u8* p = addrspace::ram_base + 0x0C000000u + (vaddr & 0x00FFFFFFu);
    char line[40];
    for (int row = 0; row * 16 < bytes; row++)
    {
        int n = 0;
        for (int c = 0; c < 16; c++)
        {
            u8 ch = p[row * 16 + c];
            line[n++] = (ch >= 32 && ch < 127) ? (char)ch : '.';
        }
        line[n++] = '\n'; line[n] = 0;
        OutputDebugStringA(line);
    }
}

// Per-frame SH-4 heartbeat + wedge register-dump tracing. OFF for speed (each
// print blocks on the serial debug link; the wedge detector also false-fires in
// any hot game loop). Flip to true to debug a new boot/hang.
static const bool kDebugTrace = false;

// Per-window diagnostic prints (PERF/FRAME/MMU/JITCNT/MMUSTATE/MEMSTAT/RENDER
// lines). OFF for release: every line is a blocking serial write. Flip to true
// (together with kJitCounters in rec_x86.cpp/x86_ops.cpp for the JITCNT
// counts) when profiling. The texture byte-budget / memory-pressure logic is
// NOT affected -- it runs unconditionally in the frame loop.
// Currently TRUE: validating the AICA channel-skip + volatile-texture round
// (watch aica=, vram=, volTex/w=). Flip back to false for release.
static const bool kPerfLog = true;

static volatile unsigned g_mainLoopBeat = 0;
static DWORD WINAPI watchdogThread(LPVOID)
{
    unsigned last = 0;
    int stalls = 0;
    bool dumped = false;
    for (;;)
    {
        Sleep(1000);
        unsigned now = g_mainLoopBeat;
        if (now != last) { last = now; stalls = 0; dumped = false; continue; }
        if (++stalls >= 4 && !dumped)   // ~4 s with no main-loop progress
        {
            dumped = true;
            OutputDebugStringA("*** WATCHDOG: main loop STALLED (render hang) ***\n");
            char b[96];

            // Is the scheduler tick still running during the hang? Sample, wait,
            // sample again. delta==0 => the dynarec is spinning in a JIT block that
            // never reaches intc_sched (block prologue); delta>0 => it IS reaching
            // the scheduler but not exiting.
            unsigned u0 = g_uintc_calls;
            unsigned rw0 = g_fault_rewrite, smc0 = g_fault_ramsmc, fp0 = g_fault_fpcb;
            Sleep(300);
            unsigned u1 = g_uintc_calls;
            unsigned rw1 = g_fault_rewrite, smc1 = g_fault_ramsmc, fp1 = g_fault_fpcb;
            wsprintfA(b, "UpdateSystem_INTC delta=%u (sched %s)\n",
                u1 - u0, (u1 == u0) ? "FROZEN -> block bypasses prologue" : "ticking");
            OutputDebugStringA(b);
            // Faults still firing DURING the freeze? A rewrite/SMC storm here means
            // the block is being recompiled+refaulted in a loop (patch never sticks).
            wsprintfA(b, "FAULTS in 300ms freeze: rw=%u smc=%u fpcb=%u\n",
                rw1 - rw0, smc1 - smc0, fp1 - fp0);
            OutputDebugStringA(b);
            // WHERE are the SMC faults landing? Sample the last faulting guest addr
            // (host VA) + the host PC doing the write, 6x across 180ms. All the same
            // => one page re-protected in a loop; spread => many pages churning.
            for (int s = 0; s < 6; ++s)
            {
                wsprintfA(b, "  smc-fault[%d] addr=%08x hostpc=%08x\n",
                    s, g_fault_lastaddr, g_fault_lastpc);
                OutputDebugStringA(b);
                Sleep(30);
            }

            // HOST x86 state of the frozen emu thread. Suspend/read/resume FIRST,
            // then print (don't print while it's suspended -> serial-lock deadlock).
            if (g_emuTid)
            {
                CONTEXT hc;
                memset(&hc, 0, sizeof(hc));
                hc.ContextFlags = CONTEXT_CONTROL;
                DmSuspendThread(g_emuTid);
                long hr = DmGetThreadContext(g_emuTid, &hc);
                DmResumeThread(g_emuTid);
                // &watchdogThread is a static-image anchor: EIP near it => stuck in
                // C++ code; EIP far away => stuck in a JIT block (dynamic cache).
                wsprintfA(b, "HOST EIP=%08x ESP=%08x (img~%08x hr=%08x)\n",
                    (unsigned)hc.Eip, (unsigned)hc.Esp,
                    (unsigned)(uintptr_t)&watchdogThread, (unsigned)hr);
                OutputDebugStringA(b);
            }
            wsprintfA(b, "pc=%08x pr=%08x\n", (unsigned)Sh4cntx.pc, (unsigned)Sh4cntx.pr);
            OutputDebugStringA(b);
            for (int i = 0; i < 16; i += 4)
            {
                wsprintfA(b, "R%d=%08x R%d=%08x R%d=%08x R%d=%08x\n",
                    i,   (unsigned)Sh4cntx.r[i],   i+1, (unsigned)Sh4cntx.r[i+1],
                    i+2, (unsigned)Sh4cntx.r[i+2], i+3, (unsigned)Sh4cntx.r[i+3]);
                OutputDebugStringA(b);
            }
            // Interrupt state + the loop instructions: is it waiting on an IRQ
            // (jmp-to-self) or polling a flag (mov.l @Rn / tst / bt)?
            wsprintfA(b, "int_pend=%08x CpuRunning=%08x\n",
                (unsigned)Sh4cntx.interrupt_pend, (unsigned)Sh4cntx.CpuRunning);
            OutputDebugStringA(b);
            dumpGuestHex16("CODE @pc:\n", (u32)Sh4cntx.pc, 16);
            dumpGuestAscii("MEM @R2:\n", (u32)Sh4cntx.r[2], 128);
        }
    }
}

// EIP sampler: freeze the emu thread every ~2ms, classify where it was.
// Never prints while the target is suspended (serial-lock deadlock).
static DWORD WINAPI samplerThread(LPVOID)
{
    for (;;)
    {
        Sleep(2);
        if (!g_emuTid)
            continue;
        CONTEXT cx;
        memset(&cx, 0, sizeof(cx));
        cx.ContextFlags = CONTEXT_CONTROL;
        DmSuspendThread(g_emuTid);
        DmGetThreadContext(g_emuTid, &cx);
        DmResumeThread(g_emuTid);
        u32 eip = (u32)cx.Eip;
        if (eip == 0)
            continue;

        if (g_sh4CacheStart != nullptr
            && eip >= (u32)(uintptr_t)g_sh4CacheStart
            && eip <  (u32)(uintptr_t)g_sh4CacheStart + g_sh4CacheSize)
        {
            if (g_memHandlerStart != nullptr
                && eip >= (u32)(uintptr_t)g_memHandlerStart
                && eip <  (u32)(uintptr_t)g_memHandlerEnd)
                g_smp_memh++;
            else
                g_smp_block++;
        }
        else if (g_arm7CacheStart != nullptr
            && eip >= (u32)(uintptr_t)g_arm7CacheStart
            && eip <  (u32)(uintptr_t)g_arm7CacheStart + g_arm7CacheSize)
        {
            g_smp_arm7++;
        }
        else
        {
            g_smp_c++;
            u32 page = eip >> 12;
            for (int i = 0; i < 48; i++)
            {
                if (g_smp_pages[i][0] == page) { g_smp_pages[i][1]++; break; }
                if (g_smp_pages[i][0] == 0)    { g_smp_pages[i][0] = page; g_smp_pages[i][1] = 1; break; }
            }
        }
    }
    return 0;
}

static bool runEmu()
{
    __try
    {
        // Install the process-wide fault handler BEFORE any JIT runs. Now that
        // xbox_eh_shim.cpp's __CxxFrameHandler3 lets foreign (access-violation)
        // exceptions from JIT code propagate cleanly through C++ frames, fastmem
        // MMIO faults + reserved-FPCB reads reach xbox_FastmemFault and resume.
        DBG("FLYCAST: os_InstallFaultHandler()\n");
        os_InstallFaultHandler();

        // Reserve the guest address space (virtmem 512MB fastmem window). Must
        // precede emu.init()/initMappings, which branch on ram_base.
        DBG("FLYCAST: addrspace::reserve()\n");
        if (!addrspace::reserve())
            DBG("FLYCAST: *** addrspace::reserve FAILED ***\n");

        DBG("FLYCAST: rend_init_renderer()\n");
        rend_init_renderer();
        // NO_REND forces the null renderer; replace it with our D3D8 renderer.
        DBG("FLYCAST: installing D3D8 renderer...\n");
        if (renderer) { renderer->Term(); delete renderer; }
        renderer = rend_DirectX9();
        if (!renderer->Init())
            DBG("FLYCAST: *** D3D8 renderer Init FAILED ***\n");
        DBG("FLYCAST: emu.init()\n");
        emu.init();
        // Bring up the controller(s) early so the pre-boot file browser can read
        // input, then let the user navigate the drives and pick a disc image --
        // or choose "Boot Dreamcast BIOS" which returns "" (boots the DC menu).
        xbox_InitInput();
        // (An autoboot experiment briefly lived here 2026-07-03 and was
        // rejected by the user -- the file browser IS the boot flow. Do not
        // bypass it again.)
        DBG("FLYCAST: running file browser...\n");
        const char* gamePath = xbox_RunFileBrowser();   // blocks until selection
        DBG("FLYCAST: emu.loadGame(\"");
        DBG(gamePath[0] ? gamePath : "<DREAMCAST BIOS>");
        DBG("\")\n");
        emu.loadGame(gamePath);
        // Idle-loop skip table (g_idleSkipPc, consumed by the SH4 JIT block
        // prologue). Per-game entries found with the HOTBLOCKS audit; the
        // address is the game's wait-for-vblank POLL block.
        if (settings.content.gameId == "T1212N")        // Marvel vs Capcom 2
        {
            g_idleSkipPc = 0x8c19162e;
            g_idleSkipOp = 0x61f2;      // mov.l @r15,r1 -- verified poll loop head
            DBG("FLYCAST: idle-skip armed @8c19162e (MvC2 vblank-wait loop)\n");
        }
        // loadGame() runs config::Settings::reset(), which re-defaults
        // ThreadedRendering back to TRUE -- undoing the override we set before
        // loadGame. Re-assert it FALSE here so emu.start() takes the
        // single-threaded path (no std::async emu thread; our _Thrd shim is
        // single-threaded and the async branch would hang).
        config::ThreadedRendering.override(false);
        DBG((bool)config::ThreadedRendering ? "FLYCAST: ThreadedRendering=TRUE (bad!)\n"
                                            : "FLYCAST: ThreadedRendering=FALSE (good)\n");
        // (AICA DSP left ENABLED: the perf experiment showed disabling it did not
        // move the frame time -- audio is not the bottleneck, the SH-4 is.)

        // CPU mode chosen in the file browser (X toggles JIT/interpreter).
        // getSh4Executor() reads config::DynarecEnabled at runtime, so this takes
        // effect for emu.start(). JIT for normal games; interpreter (slow) boots
        // the Shinobi-class games the dynarec still wedges on.
        config::DynarecEnabled.override(xbox_UseDynarec());
        DBG((bool)config::DynarecEnabled ? "FLYCAST: Dynarec=TRUE (JIT)\n"
                                         : "FLYCAST: Dynarec=FALSE (INTERPRETER)\n");

        // Emulated SH4 clock in MHz (200 = stock). The 150MHz experiment was
        // REJECTED: hardware showed only +3-5 speed points in mid scenes and
        // ~+1 in the worst ones (2026-07-03 CT log) -- nowhere near the
        // theoretical 25% -- and it degrades game fidelity. Keep at 200.
        static const int kSh4ClockMHz = 200;
        config::Sh4Clock.override(kSh4ClockMHz);
        {
            char ckbuf[64];
            wsprintfA(ckbuf, "FLYCAST: Sh4Clock=%dMHz (200=stock)\n", kSh4ClockMHz);
            DBG(ckbuf);
        }

        DBG("FLYCAST: emu.start()\n");
        emu.start();
        DBG("FLYCAST: emu.start() RETURNED; entering render loop\n");
        // (controllers already brought up before the file browser, above)
        // YELLOW baseline until the D3D8 renderer draws the first DC frame over it.
        clearScreen(D3DCOLOR_XRGB(160, 160, 0));
        // PERF PROBE: split per-frame wall time into render(D3D8) vs emulation
        // (SH-4 + AICA + TA). g_renderUs is accumulated inside the D3D8 renderer.
        extern volatile long long g_renderUs;
        LARGE_INTEGER qfreq; QueryPerformanceFrequency(&qfreq);
        long long renderBase = g_renderUs;
        long long emuUsAcc = 0;
        unsigned frame = 0;
        unsigned faultBase = g_fault_total;
        unsigned fpcbBase = g_fault_fpcb, rewriteBase = g_fault_rewrite;
        unsigned smcBase = g_fault_ramsmc, vramBase = g_fault_vram, unhBase = g_fault_unhandled;
        // Window bases for measuring *actual* presented fps and realtime speed:
        // speed% = emulated time advanced / real wall time over the window.
        // ~100% == locked to 1x (audio rate-correct); >100% == running fast.
        LARGE_INTEGER wallBase; QueryPerformanceCounter(&wallBase);
        u64 emuClkBase = sh4_sched_now64();
        // Calibrate rdtsc cycles-per-microsecond against QPC (spin ~50ms). Needed
        // because rdtsc = host clock under xemu, not the emulated 733MHz.
        long long cycPerUs;
        {
            LARGE_INTEGER a; QueryPerformanceCounter(&a);
            long long c0 = (long long)__rdtsc();
            LARGE_INTEGER b;
            do { QueryPerformanceCounter(&b); }
            while ((b.QuadPart - a.QuadPart) * 1000 / qfreq.QuadPart < 50);
            long long c1 = (long long)__rdtsc();
            long long us = (b.QuadPart - a.QuadPart) * 1000000 / qfreq.QuadPart;
            cycPerUs = us > 0 ? (c1 - c0) / us : 733;
            if (cycPerUs < 1) cycPerUs = 1;
        }
        long long armBase = g_prof_arm_cyc, aicaBase = g_prof_aica_cyc, texBase = g_prof_tex_cyc;
        long long taBase = g_prof_ta_cyc;  unsigned taCntBase = g_cnt_ta;
        unsigned armBlkBase = g_cnt_armBlk, armSopBase = g_cnt_armSop, armCondBase = g_cnt_armCond;
        unsigned armFbBase = g_arm_fallbacks;
        unsigned drawBase = g_stat_drawCalls, stateCallBase = g_stat_stateCalls;
        unsigned stateSkipBase = g_stat_stateSkipped, polyBase = g_stat_polys;
        long long mmuCycBase = g_prof_mmu_cyc;
        unsigned mmuCallBase = g_prof_mmu_calls, mmuSlowBase = g_prof_mmu_slow;
        unsigned cntBlockBase = g_cnt_block, cntMemhBase = g_cnt_memh, cntIfbBase = g_cnt_ifb;
        unsigned linkStubBase = g_cnt_linkStub, linkWiredBase = g_cnt_linkWired, linkRejBase = g_cnt_linkRej;
        g_emuTid = GetCurrentThreadId();                        // for the watchdog's host-EIP read
        if (kDebugTrace)
            CreateThread(NULL, 0, watchdogThread, NULL, 0, NULL);   // SH-4 hang detector
        if (kSampleProfiler)
            CreateThread(NULL, 0, samplerThread, NULL, 0, NULL);    // EIP sampling profiler
        for (;;)                       // run the BIOS forever; renderer presents each frame
        {
            g_mainLoopBeat++;          // watchdog liveness tick
            os_DoEvents();
            xbox_PollInput();          // Xbox pad -> mapleInputState[]

            // Drive adaptive texture eviction from real free RAM (cheap kernel
            // query). When headroom gets thin, the texcache evicts hard so big
            // WinCE games (SA1) stay bounded instead of OOMing. Hysteresis avoids
            // flapping right at the threshold.
            {
                extern volatile int textureMemPressure;
                MEMORYSTATUS mp; mp.dwLength = sizeof(mp); GlobalMemoryStatus(&mp);
                int fmb = (int)(mp.dwAvailPhys / (1024 * 1024));
                // Pressure on: texture bytes over the hard budget OR system RAM
                // getting thin. Off: comfortably under budget AND healthy freeMB
                // (hysteresis avoids flapping).
                if (g_texCacheBytes > kTexBudgetBytes || fmb < 14)
                    textureMemPressure = 1;
                else if (g_texCacheBytes < kTexBudgetBytes * 3 / 4 && fmb > 18)
                    textureMemPressure = 0;
            }

            // HEARTBEAT + WEDGE DETECT (boot-hang debug). Every 8 frames print the
            // SH-4 pc. If the pc stays put for ~64 frames the game is spinning in a
            // wait loop -- dump R0..R15 + PR once so we can see WHAT it polls: a
            // register holding 0xA05Fxxxx = a HOLLY/GD-ROM/AICA hw register; a
            // 0x8Cxxxxxx value = a RAM flag another component (ARM7/DMA) should set.
            static unsigned s_hb = 0;
            static unsigned s_lastHbPc = 0xFFFFFFFFu;
            static int  s_stuck = 0;
            static bool s_dumped = false;
            unsigned curPc = (unsigned)Sh4cntx.pc;
            if (kDebugTrace && (s_hb++ & 7) == 0)
            {
                char hb[80];
                wsprintfA(hb, "HB %u pc=%08x\n", s_hb, curPc);
                OutputDebugStringA(hb);
                if (curPc == s_lastHbPc)
                {
                    if (++s_stuck >= 2 && !s_dumped)
                    {
                        s_dumped = true;
                        OutputDebugStringA("*** SH4 WEDGED - register dump ***\n");
                        for (int i = 0; i < 16; i += 4)
                        {
                            wsprintfA(hb, "R%d=%08x R%d=%08x R%d=%08x R%d=%08x\n",
                                i,   (unsigned)Sh4cntx.r[i],   i+1, (unsigned)Sh4cntx.r[i+1],
                                i+2, (unsigned)Sh4cntx.r[i+2], i+3, (unsigned)Sh4cntx.r[i+3]);
                            OutputDebugStringA(hb);
                        }
                        wsprintfA(hb, "PR=%08x\n", (unsigned)Sh4cntx.pr);
                        OutputDebugStringA(hb);
                    }
                }
                else { s_stuck = 0; s_dumped = false; }
                s_lastHbPc = curPc;
            }

            // NO wall-clock frame limiter here: the audio backend is the single
            // pacing clock now. The AICA's push() blocks on the DirectSound FIFO
            // when it's full, so the emulator is paced to exactly the 44.1 kHz
            // hardware drain rate -- one clock, zero drift, correct pitch. If the
            // SH-4 can't sustain realtime the audio underruns (a clean dropout)
            // rather than the old wall-clock limiter masking it.
            LARGE_INTEGER a; QueryPerformanceCounter(&a);
            emu.render();
            LARGE_INTEGER b; QueryPerformanceCounter(&b);
            emuUsAcc += (b.QuadPart - a.QuadPart) * 1000000 / qfreq.QuadPart;

            if (kPerfLog && ++frame % 60 == 0)
            {
                long long renderUs = g_renderUs - renderBase;
                renderBase = g_renderUs;
                int totalMs  = (int)(emuUsAcc / 60 / 1000);
                int rendMs   = (int)(renderUs / 60 / 1000);
                unsigned faults = g_fault_total - faultBase;
                faultBase = g_fault_total;
                // Per-category deltas over this window (what kind of fault storm).
                unsigned dFpcb = g_fault_fpcb - fpcbBase;       fpcbBase = g_fault_fpcb;
                unsigned dRewr = g_fault_rewrite - rewriteBase; rewriteBase = g_fault_rewrite;
                unsigned dSmc  = g_fault_ramsmc - smcBase;      smcBase = g_fault_ramsmc;
                unsigned dVram = g_fault_vram - vramBase;       vramBase = g_fault_vram;
                unsigned dUnh  = g_fault_unhandled - unhBase;   unhBase = g_fault_unhandled;

                // Actual presented fps + realtime speed over this 60-frame window.
                LARGE_INTEGER wnow; QueryPerformanceCounter(&wnow);
                long long wallUs = (wnow.QuadPart - wallBase.QuadPart) * 1000000 / qfreq.QuadPart;
                u64 emuUs = (sh4_sched_now64() - emuClkBase) / (SH4_MAIN_CLOCK / 1000000); // cyc->us
                int realFps = wallUs > 0 ? (int)(60LL * 1000000 / wallUs) : 0;
                int speedPct = wallUs > 0 ? (int)((long long)emuUs * 100 / wallUs) : 0;
                wallBase = wnow;
                emuClkBase = sh4_sched_now64();

                // Free physical RAM (Xbox is 64MB retail / 128MB devkit). Watch
                // this trend: a steady decline under gameplay = a leak (textures,
                // blocks); a stable low floor = just a big working set.
                MEMORYSTATUS ms; ms.dwLength = sizeof(ms); GlobalMemoryStatus(&ms);
                int freeMB = (int)(ms.dwAvailPhys / (1024 * 1024));

                char buf[256];
                wsprintfA(buf, "FLYCAST PERF: %dms REAL %dfps speed=%d%% freeMB=%d | flt=%u fpcb=%u rw=%u smc=%u vram=%u bad=%u pc=%08x\n",
                          totalMs, realFps, speedPct, freeMB, faults,
                          dFpcb, dRewr, dSmc, dVram, dUnh, (unsigned)Sh4cntx.pc);
                OutputDebugStringA(buf);

                // Component breakdown, microseconds per frame. arm=ARM7 sound CPU,
                // aica=AICA sample gen+DSP (both interpreted), rend=D3D8 draw,
                // tex=texture upload/convert, sh4=everything else (SH-4 JIT +
                // scheduler + MMU), computed as the remainder. This is what tells
                // us where to spend optimization effort.
                long long armUsPf  = (g_prof_arm_cyc  - armBase)  / cycPerUs / 60;  armBase  = g_prof_arm_cyc;
                long long aicaUsPf = (g_prof_aica_cyc - aicaBase) / cycPerUs / 60;  aicaBase = g_prof_aica_cyc;
                long long texUsPf  = (g_prof_tex_cyc  - texBase)  / cycPerUs / 60;  texBase  = g_prof_tex_cyc;
                long long totUsPf  = emuUsAcc / 60;
                long long rendUsPf = renderUs / 60;
                long long sh4UsPfRaw = totUsPf - rendUsPf - armUsPf - aicaUsPf;   // pre-clamp, kept for the debug print below
                long long sh4UsPf  = sh4UsPfRaw < 0 ? 0 : sh4UsPfRaw;
                unsigned armFbPf = (g_arm_fallbacks - armFbBase) / 60;  armFbBase = g_arm_fallbacks;

                // ROOT-CAUSE FIX (2026-07-01): wsprintfA's "%lld" only consumes 4
                // bytes per argument instead of 8, so every 64-bit value after the
                // first shifted the rest of this line's fields by one slot (proven
                // via the SH4DBG(i32) cross-check: printed "arm" was actually the
                // true sh4 remainder, printed "rend" was actually the true arm cost,
                // printed "armFallback/f" was actually the true aica cost -- the real
                // rend/tex/fallback values never appeared at all). All these values
                // fit comfortably in 32 bits (frame times are at most ~100ms), so
                // %d with explicit casts sidesteps the bug entirely -- verified
                // correct via the SH4DBG(i32) line this replaces.
                wsprintfA(buf, "  FRAME us/f: total=%d sh4=%d arm=%d aica=%d rend=%d tex=%d | armFallback/f=%u\n",
                          (int)totUsPf, (int)sh4UsPf, (int)armUsPf, (int)aicaUsPf,
                          (int)rendUsPf, (int)texUsPf, armFbPf);
                OutputDebugStringA(buf);

                // TA vertex-parsing share of the sh4 bucket (direct measurement,
                // includes the rdtsc probe's own overhead -- treat as upper bound).
                long long taUsPf = (g_prof_ta_cyc - taBase) / cycPerUs / 60;  taBase = g_prof_ta_cyc;
                unsigned taCntPf = (g_cnt_ta - taCntBase) / 60;  taCntBase = g_cnt_ta;
                unsigned parsePf = g_stat_parseUs / 60;  g_stat_parseUs = 0;
                unsigned sortPf = g_stat_sortUs / 60;  g_stat_sortUs = 0;
                // dec = TaCmd decode loop (contains texlk); texlk = GetTexture
                // (lookup + volatile hash + convert + upload); hashKB = volatile
                // hash-skip bytes hashed per WINDOW. dec-texlk = pure decode.
                unsigned decPf = g_stat_decodeUs / 60;  g_stat_decodeUs = 0;
                unsigned texLkPf = g_stat_texLkUs / 60;  g_stat_texLkUs = 0;
                unsigned hashKb = g_stat_volHashBytes / 1024;  g_stat_volHashBytes = 0;
                wsprintfA(buf, "  TA: us/f=%d chunks/f=%u parse/f=%uus sort/f=%uus dec/f=%uus texlk/f=%uus hashKB/w=%u\n",
                          (int)taUsPf, taCntPf, parsePf, sortPf, decPf, texLkPf, hashKb);
                OutputDebugStringA(buf);

                // ARM7 JIT execution shape: blocks, flag-setting ops, conditional
                // ops per frame (emitted counters; ~0 if kArmJitCounters off).
                unsigned armBlkPf = (g_cnt_armBlk - armBlkBase) / 60;  armBlkBase = g_cnt_armBlk;
                unsigned armSopPf = (g_cnt_armSop - armSopBase) / 60;  armSopBase = g_cnt_armSop;
                unsigned armCondPf = (g_cnt_armCond - armCondBase) / 60;  armCondBase = g_cnt_armCond;
                wsprintfA(buf, "  ARMJIT: blk/f=%u sop/f=%u cond/f=%u\n", armBlkPf, armSopPf, armCondPf);
                OutputDebugStringA(buf);

                // Direct MMU data-translation measurement (mmu.cpp): mmuUs is
                // microseconds/frame ACTUALLY spent in mmu_data_translation (a
                // real, direct number, not inferred). calls/f = total invocations;
                // slow/f = how many reached the 64-entry mmu_full_lookup instead of
                // the fast_reg_lut early-out. If mmuUs is small relative to sh4's
                // total, the MMU per-access cost isn't the driver and the real cost
                // is elsewhere in JIT execution (register spilling, dispatch).
                long long mmuUsPf = (g_prof_mmu_cyc - mmuCycBase) / cycPerUs / 60;  mmuCycBase = g_prof_mmu_cyc;
                unsigned mmuCallPf = (g_prof_mmu_calls - mmuCallBase) / 60;  mmuCallBase = g_prof_mmu_calls;
                unsigned mmuSlowPf = (g_prof_mmu_slow  - mmuSlowBase) / 60;  mmuSlowBase = g_prof_mmu_slow;
                wsprintfA(buf, "  MMU: us/f=%d calls/f=%u slow/f=%u\n",
                          (int)mmuUsPf, mmuCallPf, mmuSlowPf);
                OutputDebugStringA(buf);

                // Execution shape: how many block entries / memory-handler entries /
                // interpreter fallbacks the generated code executed, per frame.
                // blocks/f tells us average block length (guest work / blocks);
                // memh/f the memory-access density; ifb/f the decoder-fallback rate
                // (each ifb is a full C call -- a high rate here would explain the
                // WinCE sh4 cost on its own).
                unsigned cbPf = (g_cnt_block - cntBlockBase) / 60;  cntBlockBase = g_cnt_block;
                unsigned cmPf = (g_cnt_memh  - cntMemhBase) / 60;  cntMemhBase = g_cnt_memh;
                unsigned ciPf = (g_cnt_ifb   - cntIfbBase)  / 60;  cntIfbBase  = g_cnt_ifb;
                // Link diagnostics: per-WINDOW (not per-frame) so one-time wiring
                // events stay visible instead of dividing down to 0.
                unsigned lsW = g_cnt_linkStub  - linkStubBase;   linkStubBase  = g_cnt_linkStub;
                unsigned lwW = g_cnt_linkWired - linkWiredBase;  linkWiredBase = g_cnt_linkWired;
                unsigned lrW = g_cnt_linkRej   - linkRejBase;    linkRejBase   = g_cnt_linkRej;
                wsprintfA(buf, "  JITCNT: blocks/f=%u memh/f=%u ifb/f=%u | link/win: stub=%u wired=%u rej=%u\n",
                          cbPf, cmPf, ciPf, lsW, lwW, lrW);
                OutputDebugStringA(buf);

                // Live MMU state + cumulative compile-branch counts: is gameplay
                // code even compiled in MMU mode?
                wsprintfA(buf, "  MMUSTATE: on=%d emitted mmuLink=%u mmuNoUpd=%u nonMmu=%u\n",
                          mmuOn ? 1 : 0, g_cnt_emitMmuLink, g_cnt_emitMmuNoUpd, g_cnt_emitNonMmu);
                OutputDebugStringA(buf);

                // Who owns the RAM: total physical (once), free, and the texture
                // cache's exact committed bytes + live texture count. If freeMB
                // sinks while texKB stays at/below budget, the leak is elsewhere
                // and this line points the hunt away from textures.
                {
                    extern volatile int textureMemPressure;
                    static bool s_totalPrinted = false;
                    if (!s_totalPrinted)
                    {
                        s_totalPrinted = true;
                        MEMORYSTATUS mt; mt.dwLength = sizeof(mt); GlobalMemoryStatus(&mt);
                        wsprintfA(buf, "  MEMSTAT: totalPhys=%uMB\n",
                                  (unsigned)(mt.dwTotalPhys / (1024 * 1024)));
                        OutputDebugStringA(buf);
                    }
                    wsprintfA(buf, "  MEMSTAT: free=%dMB tex=%uKB texN=%u pressure=%d\n",
                              freeMB, g_texCacheBytes >> 10, g_texCacheCount,
                              textureMemPressure);
                    OutputDebugStringA(buf);
                }

                if (kSampleProfiler)
                {
                    // Anchors once, so C pages in the histogram can be attributed
                    // to subsystems straight from the log.
                    static bool s_anchorsPrinted = false;
                    if (!s_anchorsPrinted)
                    {
                        s_anchorsPrinted = true;
                        wsprintfA(buf, "  SMP-ANCHORS: img=%08x UpdateSystem_INTC=%08x AICA_Sample=%08x sh4cache=%08x memh=%08x-%08x arm7=%08x\n",
                                  (unsigned)(uintptr_t)&watchdogThread,
                                  (unsigned)(uintptr_t)&UpdateSystem_INTC,
                                  (unsigned)(uintptr_t)&aica::sgc::AICA_Sample,
                                  (unsigned)(uintptr_t)g_sh4CacheStart,
                                  (unsigned)(uintptr_t)g_memHandlerStart,
                                  (unsigned)(uintptr_t)g_memHandlerEnd,
                                  (unsigned)(uintptr_t)g_arm7CacheStart);
                        OutputDebugStringA(buf);
                    }

                    // Snapshot + reset buckets, pick the top 4 C pages.
                    unsigned sBlock = g_smp_block; g_smp_block = 0;
                    unsigned sMemh  = g_smp_memh;  g_smp_memh  = 0;
                    unsigned sArm7  = g_smp_arm7;  g_smp_arm7  = 0;
                    unsigned sC     = g_smp_c;     g_smp_c     = 0;
                    u32 topPage[4] = {0,0,0,0}; u32 topCnt[4] = {0,0,0,0};
                    for (int i = 0; i < 48; i++)
                    {
                        u32 p = g_smp_pages[i][0], n = g_smp_pages[i][1];
                        g_smp_pages[i][0] = 0; g_smp_pages[i][1] = 0;
                        if (p == 0 || n == 0) continue;
                        for (int k = 0; k < 4; k++)
                            if (n > topCnt[k])
                            {
                                for (int m = 3; m > k; m--) { topCnt[m] = topCnt[m-1]; topPage[m] = topPage[m-1]; }
                                topCnt[k] = n; topPage[k] = p;
                                break;
                            }
                    }
                    wsprintfA(buf, "  SMP: block=%u memh=%u arm7=%u c=%u | c-top: %05x:%u %05x:%u %05x:%u %05x:%u\n",
                              sBlock, sMemh, sArm7, sC,
                              (unsigned)topPage[0], (unsigned)topCnt[0],
                              (unsigned)topPage[1], (unsigned)topCnt[1],
                              (unsigned)topPage[2], (unsigned)topCnt[2],
                              (unsigned)topPage[3], (unsigned)topCnt[3]);
                    OutputDebugStringA(buf);
                }

                // Render diagnostics: is rend cost bound by draw-call count,
                // state-change count, or vertex/index throughput? verts/idx are
                // instantaneous (last frame's counts, not summed over the window).
                unsigned drawPf = (g_stat_drawCalls - drawBase) / 60;  drawBase = g_stat_drawCalls;
                unsigned setPf  = (g_stat_stateCalls - stateCallBase) / 60;  stateCallBase = g_stat_stateCalls;
                unsigned skipPf = (g_stat_stateSkipped - stateSkipBase) / 60;  stateSkipBase = g_stat_stateSkipped;
                unsigned polyPf = (g_stat_polys - polyBase) / 60;  polyBase = g_stat_polys;
                unsigned vbLockPf = g_stat_vbLockUs / 60;  g_stat_vbLockUs = 0;
                unsigned volTexW = g_stat_volatileTex;  g_stat_volatileTex = 0;
                unsigned volSkipW = g_stat_volSkip;  g_stat_volSkip = 0;
                wsprintfA(buf, "  RENDER: draws/f=%u polys/f=%u stateSet/f=%u stateSkip/f=%u verts=%u idx=%u vbLock/f=%uus volTex/w=%u volSkip/w=%u\n",
                          drawPf, polyPf, setPf, skipPf, g_stat_verts, g_stat_idx, vbLockPf, volTexW, volSkipW);
                OutputDebugStringA(buf);

                // Auto idle-loop detection: once per window (~1s) arm any
                // candidate block spinning at wait-loop rates (logs each
                // arming as "auto idle-skip armed @pc"). Only while the game
                // is actively rendering (taCntPf = TA chunks/frame this
                // window); when rendering stops (loading screens), everything
                // auto-armed is disarmed -- loader pump loops look exactly
                // like wait loops but their iteration count drives progress
                // (Crazy Taxi CD loads). To re-run the full JIT audit, flip
                // kBlockProfile in rec_x86.cpp and call bm_DumpHotBlocks(10).
                {
                    extern void bm_AutoIdleScan(bool gameRendering);
                    bm_AutoIdleScan(taCntPf > 0);
                }
                emuUsAcc = 0;
            }
        }
        // unreachable
    }
    __except (sehFilter(GetExceptionInformation()))
    {
        return false;
    }
}

void __cdecl main()
{
    // Distinguish abort paths: if it's an uncaught/un-unwindable exception this
    // handler fires (and hangs instead of the CRT abort message); if we still
    // see the CRT "...terminate in an unusual way" message instead, it's a
    // direct abort (bad-param/assert), not a C++ exception.
    std::set_terminate([]() {
        OutputDebugStringA("FLYCAST: *** std::terminate (uncaught/non-unwindable exception) ***\n");
        for (;;) {}
    });
    if (!initD3D())
        return;

    clearScreen(D3DCOLOR_XRGB(0, 0, 160));   // BLUE: booting
    DBG("FLYCAST: D3D up, entering boot\n");

    // Quick locale sanity check (the hand-written std::locale runtime).
    {
        std::stringstream ss; ss << 1234; int v = 0; ss >> v;
        DBG(v == 1234 ? "FLYCAST: locale OK\n" : "FLYCAST: locale BROKEN\n");
    }
    // Locale works now, so Flycast's own logging is safe to enable. Its
    // NOTICE_LOG/INFO_LOG go to OutputDebugStringA (xbwatson) -> shows exactly
    // how far loadGame/dc_reset get before the crash.
    setupLogging();
    DBG("FLYCAST: logging enabled, proceeding to emulator\n");
    // (Fastmem feasibility proved: Xbox reserves the 512MB window + commits RAM
    //  into it. The real reservation now lives in virtmem::init -- xbox_oslib.cpp.)

    D3DCOLOR result = D3DCOLOR_XRGB(200, 0, 0);  // default RED until we succeed
    try
    {
        // Flycast data dir (BIOS/flash live here on the Xbox). reios HLE means
        // no real dc_boot.bin is required for this milestone.
        // BIOS/flash are packed into the disc's Media folder, mounted at D:\Media.
        // (D:\ is read-only -> reads work; flash SAVES won't persist, fine for boot.)
        DBG("FLYCAST: set_user_data_dir D:\\Media\\\n");
        set_user_data_dir("D:\\Media\\");
        set_user_config_dir("D:\\Media\\");

        // Single-threaded synchronous path: no AICA/render threads.
        DBG("FLYCAST: config ThreadedRendering=false\n");
        config::ThreadedRendering.override(false);

        // SEH-wrapped: catches an access violation in loadGame/dc_reset and
        // prints the faulting address so we can map it to a function.
        if (runEmu())
        {
            result = D3DCOLOR_XRGB(0, 200, 0);      // GREEN: success
        }
        else
        {
            DBG("FLYCAST: *** STRUCTURED EXCEPTION (access violation) ***\n");
            DBGHEX("FLYCAST: *** SEH code=", g_sehCode);
            DBGHEX("FLYCAST: *** SEH addr=", g_sehAddr);
            result = D3DCOLOR_XRGB(200, 0, 0);      // RED
        }
    }
    catch (const std::exception& e)
    {
        DBG("FLYCAST: std::exception: ");
        DBG(e.what());
        DBG("\n");
        result = D3DCOLOR_XRGB(200, 0, 0);          // RED: exception
    }
    catch (...)
    {
        DBG("FLYCAST: unknown exception\n");
        result = D3DCOLOR_XRGB(200, 0, 0);
    }

    for (;;)
        clearScreen(result);
}
