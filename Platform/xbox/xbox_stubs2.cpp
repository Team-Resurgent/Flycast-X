// ============================================================================
//  xbox_stubs2.cpp  --  the final tail: device subsystems absent on a stock
//  Dreamcast-on-Xbox boot (broadband adapter, modem, NAOMI net), plus a few
//  real runtime helpers. "Device absent" => reads return open-bus (0xFFFFFFFF),
//  which is the genuinely correct behavior, not a fake.
// ============================================================================
#include "types.h"
#include <cstdio>
#include <string>
#include <stdexcept>

class Serializer;
class Deserializer;
class Archive;

// ---- Broadband Adapter (BBA) -- not fitted -------------------------------
void bba_Init() {}
void bba_Term() {}
void bba_Reset(bool) {}
void bba_Serialize(Serializer&) {}
void bba_Deserialize(Deserializer&) {}
u32  bba_ReadMem(u32, u32)      { return 0xFFFFFFFF; }
void bba_WriteMem(u32, u32, u32) {}

// ---- Modem -- not fitted ---------------------------------------------------
void ModemInit() {}
void ModemTerm() {}
void ModemReset() {}
void ModemSerialize(Serializer&) {}
void ModemDeserialize(Deserializer&) {}
u32  ModemReadMem_A0_006(u32, u32)       { return 0xFFFFFFFF; }
void ModemWriteMem_A0_006(u32, u32, u32) {}
void serialModemInit() {}
void serialModemTerm() {}

// ---- RetroAchievements serialize hooks (feature disabled) ------------------
namespace achievements {
    void serialize(Serializer&) {}
    void deserialize(Deserializer&) {}
}

// ---- misc frontend hooks ---------------------------------------------------
namespace i18n { void reloadLanguage() {} }
void mainui_stop() {}
Archive* OpenArchive(const std::string&) { return nullptr; }   // .zip/.7z disabled

// ---- xBRZ upscaler -- unused (NV2A does its own scaling) -------------------
namespace xbrz {
    enum ColorFormat { RGB, ARGB, ARGB_UNBUFFERED };
    struct ScalerCfg;
    void scale(unsigned, const u32*, u32*, int, int, ColorFormat, const ScalerCfg&, int, int) {}
}

// ---- os_DebugBreak / fatal_error : announce then halt ----------------------
extern "C" void __stdcall OutputDebugStringA(const char*);
void os_DebugBreak() { OutputDebugStringA("FLYCAST: os_DebugBreak (halt)\n"); for (;;) {} }
void fatal_error(const char* msg, ...) { OutputDebugStringA("FLYCAST FATAL: "); OutputDebugStringA(msg ? msg : "?"); OutputDebugStringA("\n"); for (;;) {} }

// ---- std throw helpers (same family as _Xlength_error) ---------------------
namespace std {
    [[noreturn]] void __cdecl _Xbad_alloc()                  { throw std::bad_alloc(); }
    [[noreturn]] void __cdecl _Xinvalid_argument(const char* m) { throw std::invalid_argument(m); }
    [[noreturn]] void __cdecl _Xout_of_range(const char* m)  { throw std::out_of_range(m); }
    [[noreturn]] void __cdecl _Xruntime_error(const char* m) { throw std::runtime_error(m); }
}

// ---- std::_Fiopen : wide-path file open for std::fstream -------------------
namespace std {
    FILE* __cdecl _Fiopen(const wchar_t* name, int mode, int /*prot*/)
    {
        char narrow[300];
        int i = 0;
        while (name[i] && i < 299) { narrow[i] = (char)name[i]; ++i; }
        narrow[i] = '\0';
        // ios_base::in=1 out=2 ate=4 app=8 trunc=16 binary=32
        const bool bin = (mode & 32) != 0;
        const char* m;
        if ((mode & 2) && (mode & 1)) m = bin ? "r+b" : "r+";
        else if (mode & 2)            m = bin ? "wb"  : "w";
        else                          m = bin ? "rb"  : "r";
        return std::fopen(narrow, m);
    }
}
