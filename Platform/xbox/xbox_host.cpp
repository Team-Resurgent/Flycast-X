// ============================================================================
//  xbox_host.cpp  --  Xbox host-abstraction implementations the Flycast core
//  expects the frontend to provide. Starting with the os_* surface from
//  oslib/oslib.h; virtmem / network / oslib stubs get added as the linker
//  reports them (the undefined-symbol list is the precise spec).
// ============================================================================
#include "types.h"
#include "oslib/oslib.h"

// ---- os_* event/input/window/lifecycle hooks -------------------------------
// Headless boot: only os_DoEvents / os_SetThreadName / os_notify are actually
// invoked; the rest just need to link.
void os_DoEvents() {}
void os_CreateWindow() {}
void os_DestroyWindow() {}
void os_SetupInput() {}
void os_TermInput() {}
void os_UpdateInputState() {}
// os_InstallFaultHandler / os_UninstallFaultHandler now live in
// xbox_fault_handler.cpp (real SetUnhandledExceptionFilter, for fastmem/JIT).
void os_RunInstance(int /*argc*/, const char* /*argv*/[]) {}
void os_SetThreadName(const char* /*name*/) {}

extern "C" void __stdcall OutputDebugStringA(const char*);
void os_notify(const char* msg, int /*durationMs*/, const char* /*details*/)
{
    OutputDebugStringA("FLYCAST os_notify: ");
    OutputDebugStringA(msg ? msg : "(null)");
    OutputDebugStringA("\n");
}

// ===========================================================================
//  GUI (ImGui frontend) -- there is no GUI on the headless console build, so
//  every entry point is a correct no-op and the state is simply "closed".
// ===========================================================================
#include <string>
#include <memory>
enum GuiState { Closed, Commands, Settings };
GuiState gui_state = Closed;
void gui_init() {}
void gui_term() {}
void gui_open_settings() {}
void gui_open_onboarding() {}
void gui_cancel_load() {}
void gui_takeScreenshot() {}
void gui_togglePause() {}
void gui_loadState(bool) {}
void gui_saveState(bool, bool) {}
void gui_cycleSaveStateSlot(int) {}
void gui_set_mouse_button(int, bool, bool) {}
void gui_set_mouse_position(int, int, bool) {}
void gui_set_mouse_wheel(float) {}
bool gui_mouse_captured() { return false; }

// ===========================================================================
//  GGPO netplay -- inactive on Xbox. "Not in a session, no rollback" is the
//  correct state for local single-player.
// ===========================================================================
struct MapleInputState;
namespace ggpo {
    bool inRollback = false;
    bool active() { return false; }
    bool nextFrame() { return false; }
    void endOfFrame() {}
    void getInput(MapleInputState* const) {}
}

// ===========================================================================
//  Networking handshake / NAOMI net -- unsupported on this build.
// ===========================================================================
struct NetworkHandshake { static void init(); static void term(); };
void NetworkHandshake::init() {}
void NetworkHandshake::term() {}
bool NaomiNetworkSupported() { return false; }

// ===========================================================================
//  Embedded resource loader -- reios boot needs none; return empty.
// ===========================================================================
namespace resource {
    std::unique_ptr<u8[]> load(const std::string& /*name*/, unsigned& outSize)
    {
        outSize = 0;
        return std::unique_ptr<u8[]>();
    }
}

// ===========================================================================
//  flycast:: filesystem helpers + nowide::getenv  (real-ish XDK behavior).
// ===========================================================================
extern "C" int    __stdcall CreateDirectoryA(const char*, void*);
extern "C" unsigned __stdcall GetFileAttributesA(const char*);
namespace flycast {
    int access(const char* path, int /*mode*/)
    {
        return (GetFileAttributesA(path) == 0xFFFFFFFF) ? -1 : 0;
    }
    int mkdir(const char* path, unsigned short /*mode*/)
    {
        return CreateDirectoryA(path, nullptr) ? 0 : -1;
    }
}
namespace nowide {
    char* getenv(const char* /*name*/) { return nullptr; }  // no environment on Xbox
}
