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
    // INFO so the boot/GD-ROM path is visible (debugging MvC2's boot hang). Logs
    // go via OutputDebugStringA (serial, slow), so SILENCE the high-frequency
    // categories -- keep only BOOT/COMMON/GDROM/REIOS/HOLLY/MEMORY/FLASHROM/
    // NAOMI/VMEM, which is exactly the boot+disc activity we want to trace.
    lm->SetLogLevel(LogTypes::LINFO);
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
extern "C" {
    long __stdcall DmSuspendThread(unsigned long dwThreadId);
    long __stdcall DmResumeThread(unsigned long dwThreadId);
    long __stdcall DmGetThreadContext(unsigned long dwThreadId, CONTEXT* pCtx);
}
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
        DBG("FLYCAST: running file browser...\n");
        const char* gamePath = xbox_RunFileBrowser();   // blocks until selection
        DBG("FLYCAST: emu.loadGame(\"");
        DBG(gamePath[0] ? gamePath : "<DREAMCAST BIOS>");
        DBG("\")\n");
        emu.loadGame(gamePath);
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
        g_emuTid = GetCurrentThreadId();                        // for the watchdog's host-EIP read
        CreateThread(NULL, 0, watchdogThread, NULL, 0, NULL);   // SH-4 hang detector
        for (;;)                       // run the BIOS forever; renderer presents each frame
        {
            g_mainLoopBeat++;          // watchdog liveness tick
            os_DoEvents();
            xbox_PollInput();          // Xbox pad -> mapleInputState[]

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
            if ((s_hb++ & 7) == 0)
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

            if (++frame % 60 == 0)
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

                char buf[256];
                wsprintfA(buf, "FLYCAST PERF: %dms REAL %dfps speed=%d%% | flt=%u fpcb=%u rw=%u smc=%u vram=%u bad=%u pc=%08x\n",
                          totalMs, realFps, speedPct, faults,
                          dFpcb, dRewr, dSmc, dVram, dUnh, (unsigned)Sh4cntx.pc);
                OutputDebugStringA(buf);
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
