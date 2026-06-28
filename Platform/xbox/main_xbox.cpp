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

#include "build.h"
#include "types.h"
#include "emulator.h"
#include "cfg/option.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_mem.h"
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
    // LWARNING, not LDEBUG: every log line is emitted via OutputDebugStringA,
    // which is SERIAL debug output and very slow on xemu/hardware. At LDEBUG the
    // core spams the loop and throttles emulation. Warnings/errors only.
    lm->SetLogLevel(LogTypes::LWARNING);
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
static unsigned g_sehCode = 0, g_sehAddr = 0;
static int sehFilter(struct _EXCEPTION_POINTERS* ep)
{
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
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
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

static bool runEmu()
{
    __try
    {
        // Reserve the guest address space (virtmem 512MB window; more RAM-
        // efficient than the malloc fallback). Must precede emu.init()/initMappings.
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
        DBG("FLYCAST: emu.loadGame(\"\")\n");
        emu.loadGame("");
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
        DBG("FLYCAST: emu.start()\n");
        emu.start();
        DBG("FLYCAST: emu.start() RETURNED; entering render loop\n");
        // YELLOW baseline until the D3D8 renderer draws the first DC frame over it.
        clearScreen(D3DCOLOR_XRGB(160, 160, 0));
        // PERF PROBE: split per-frame wall time into render(D3D8) vs emulation
        // (SH-4 + AICA + TA). g_renderUs is accumulated inside the D3D8 renderer.
        extern volatile long long g_renderUs;
        LARGE_INTEGER qfreq; QueryPerformanceFrequency(&qfreq);
        long long renderBase = g_renderUs;
        long long emuUsAcc = 0;
        unsigned frame = 0;
        for (;;)                       // run the BIOS forever; renderer presents each frame
        {
            os_DoEvents();
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
                char buf[160];
                wsprintfA(buf, "FLYCAST PERF: %dms/frame total | render=%dms emu=%dms (%d fps)\n",
                          totalMs, rendMs, totalMs - rendMs, totalMs > 0 ? 1000 / totalMs : 0);
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
