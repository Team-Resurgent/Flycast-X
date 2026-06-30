// ============================================================================
//  xbox_oslib.cpp  --  Xbox replacements for the desktop oslib/virtmem/i18n
//  files we can't compile (shlobj/VirtualAlloc/tinygettext). Minimal but real:
//  paths live under D:\Media\ (the disc, where the BIOS/flash are baked into the
//  ISO); virtmem forces the core's malloc fallback; i18n is a passthrough.
//  Enough to bring the emulator up headless. NOTE: D:\ is read-only, so flash
//  SAVES won't persist -- fine for booting to the BIOS swirl.
// ============================================================================
#include "types.h"
#include "oslib/oslib.h"
#include "oslib/virtmem.h"
#include "oslib/directory.h"
#include "oslib/i18n.h"
#include "hw/sh4/sh4_if.h"     // Sh4RCB, FPCB_SIZE (for the fastmem window layout)
#include "hw/mem/addrspace.h"  // addrspace::ram_base (commit targets)
#include <string>
#include <vector>
#include <cstdio>
#include <xtl.h>

#define XB_DATA "D:\\Media\\"

// ---- hostfs: paths under the Xbox data partition --------------------------
namespace hostfs
{
    // Parse a ";"-separated list of name patterns ('%' -> prefix), return the
    // full path of the first one that EXISTS under XB_DATA. Used to locate both
    // the flash ("%flash.bin;...") and the BIOS ("%boot.bin;%bios.bin;..."), so
    // it MUST honor `names` -- returning a fixed "flash.bin" makes the BIOS load
    // read dc_flash.bin (128KB) as the 2MB bootrom and wedge.
    std::string findFlash(const std::string& prefix, const std::string& names)
    {
        size_t start = 0;
        while (start <= names.size())
        {
            size_t end = names.find(';', start);
            std::string pat = names.substr(start, end == std::string::npos ? std::string::npos : end - start);
            std::string fname;
            for (size_t i = 0; i < pat.size(); ++i)
                if (pat[i] == '%') fname += prefix; else fname += pat[i];
            if (!fname.empty())
            {
                std::string full = std::string(XB_DATA) + fname;
                FILE* f = std::fopen(full.c_str(), "rb");
                if (f) { std::fclose(f); return full; }
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return std::string();
    }

    std::string getVmuPath(const std::string& port, bool /*save*/) { return std::string(XB_DATA) + "vmu_" + port + ".bin"; }
    std::string getArcadeFlashPath()                               { return std::string(XB_DATA) + "arcadeflash"; }
    std::string getFlashSavePath(const std::string& prefix, const std::string& name) { return std::string(XB_DATA) + prefix + name; }
    std::string findNaomiBios(const std::string& /*name*/)         { return std::string(); }
    std::string getSavestatePath(int index, bool /*writable*/)     { return std::string(XB_DATA) + "state_" + std::to_string(index) + ".sav"; }
    std::string getTextureLoadPath(const std::string& /*gameId*/)  { return std::string(); }
    std::string getTextureDumpPath()                               { return std::string(XB_DATA); }
    std::string getShaderCachePath(const std::string& filename)    { return std::string(XB_DATA) + filename; }
    void saveScreenshot(const std::string& /*name*/, const std::vector<u8>& /*data*/) {}
    const std::vector<std::string>& getCdromDrives()               { static std::vector<std::string> none; return none; }
    void importHomeDirectory() {}
    void exportHomeDirectory() {}
}

// ---- i18n: passthrough (no translations on Xbox) --------------------------
namespace i18n
{
    const char* T(const char* s)        { return s; }
    std::string Ts(const std::string& s){ return s; }
}

// ---- virtmem: FASTMEM. Reserve a 512MB virtual window (ram_base) and commit
//      the DC RAM/VRAM/ARAM backing regions at their DC offsets; leave the rest
//      reserved (PAGE_NOACCESS) so MMIO + RAM mirror accesses FAULT into the
//      SEH handler -> rewriteMemAccess -> slow handler. The x86 JIT then emits
//      direct `mov reg,[ecx + ram_base]` loads (x86_ops.cpp MemType::Fast) for
//      the hot RAM/VRAM paths instead of a function call per access.
//
//      Only VirtualAlloc/Protect/Free are needed (RXDK has no CreateFileMapping/
//      MapViewOfFileEx), so the DC's hardware memory MIRRORS (which Windows
//      realizes by multi-mapping one section) are NOT aliased here: we commit
//      the PRIMARY copy each backing pointer targets, and the rare mirror
//      accesses simply fault to the (correct) slow handler. -------------------
namespace virtmem
{
    static void* g_window = nullptr;   // base of the whole reservation (for destroy)

    bool init(void** vmem_base_addr, void** sh4rcb_addr, size_t /*ramSize*/)
    {
        const size_t fpcbBytes = (size_t)FPCB_SIZE * sizeof(void*);   // 64MB
        const size_t rcbBytes  = sizeof(Sh4RCB);                      // 64MB + 64KB
        // 512MB DC fast window + the Sh4RCB (fpcb+context) + ARAM tail (the
        // writable AICA mirror sits at offset 0x20000000 = exactly 512MB).
        const size_t winBytes  = (size_t)512 * 1024 * 1024 + rcbBytes + ARAM_SIZE_MAX;

        char* base = (char*)VirtualAlloc(NULL, winBytes, MEM_RESERVE, PAGE_NOACCESS);
        if (!base)
        {
            OutputDebugStringA("FLYCAST: virtmem::init reserve FAILED -> malloc fallback\n");
            if (vmem_base_addr) *vmem_base_addr = nullptr;   // signal fallback
            return false;
        }

        *sh4rcb_addr    = base;                 // Sh4RCB lives at the very start
        *vmem_base_addr = base + rcbBytes;      // ram_base follows it

        // Commit the Sh4Context (everything in Sh4RCB after the fpcb[] array).
        // The 64MB fpcb stays RESERVED -> bm_lockedWrite commits its pages on
        // demand as the JIT first touches each block-dispatch slot.
        void* ctx = VirtualAlloc(base + fpcbBytes, rcbBytes - fpcbBytes, MEM_COMMIT, PAGE_READWRITE);
        if (ctx != base + fpcbBytes)
        {
            OutputDebugStringA("FLYCAST: virtmem::init context commit FAILED\n");
            VirtualFree(base, 0, MEM_RELEASE);
            if (vmem_base_addr) *vmem_base_addr = nullptr;
            return false;
        }

        g_window = base;
        OutputDebugStringA("FLYCAST: virtmem::init OK -- FASTMEM window reserved\n");
        return true;
    }

    void destroy()
    {
        if (g_window) { VirtualFree(g_window, 0, MEM_RELEASE); g_window = nullptr; }
    }

    // Decommit on-demand pages (fpcb reset).
    void reset_mem(void* ptr, unsigned size_bytes)
    {
        if (ptr && size_bytes) VirtualFree(ptr, size_bytes, MEM_DECOMMIT);
    }

    // Commit a page on a fault (fpcb demand paging via bm_lockedWrite).
    void ondemand_page(void* address, unsigned size_bytes)
    {
        VirtualAlloc(address, size_bytes, MEM_COMMIT, PAGE_READWRITE);
    }

    // Commit the DC RAM/VRAM/ARAM/ERAM backing regions inside the window. These
    // offsets match the setRegion() targets in addrspace::initMappings, so the
    // pointers the renderer/AICA dereference directly land on committed pages.
    // Everything else in the window stays reserved -> faults to the slow path.
    void create_mappings(const Mapping* maps, unsigned count)
    {
        for (unsigned i = 0; i < count; ++i)
        {
            const Mapping& m = maps[i];
            if (m.memsize == 0)
                continue;
            const u64 a = m.start_address;
            if (a != 0x04000000     // VRAM    (vram backing, FAST primary)
             && a != 0x0A000000     // Elan ERAM
             && a != 0x0C000000     // main RAM (mem_b backing, FAST primary)
             && a != 0x20000000)    // writable AICA RAM (aica_ram backing)
                continue;
            void* want = &addrspace::ram_base[a];
            void* got  = VirtualAlloc(want, (SIZE_T)m.memsize, MEM_COMMIT, PAGE_READWRITE);
            if (got != want)
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "FLYCAST: virtmem commit @0x%08x (%uKB) FAILED\n",
                         (unsigned)a, (unsigned)(m.memsize / 1024));
                OutputDebugStringA(msg);
            }
        }
    }

    // Real page protection (REQUIRED for the JIT now that xbox_eh_shim.cpp makes
    // faults dispatch): bm_LockPage marks RAM pages holding compiled code
    // read-only; a guest write (self-modifying code) faults -> xbox_FastmemFault
    // -> bm_RamWriteAccess discards the stale block + unprotects -> recompiled
    // with the new code. Without this the JIT runs STALE blocks after the BIOS
    // patches RAM, and spins forever in a wait loop.
    bool region_lock(void* start, std::size_t len)
    {
        DWORD old;
        return VirtualProtect(start, len, PAGE_READONLY, &old) != 0;
    }
    bool region_unlock(void* start, std::size_t len)
    {
        DWORD old;
        return VirtualProtect(start, len, PAGE_READWRITE, &old) != 0;
    }

    // JIT code cache: RWX from the kernel. (Only used if the SH-4 JIT is ever
    // re-enabled; the interpreter build never calls this.)
    bool prepare_jit_block(void* /*code_area*/, size_t size, void** code_area_rwx)
    {
        void* p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!p) return false;
        *code_area_rwx = p;
        return true;
    }

    // Dual-mapping variant (not needed on Xbox — one RWX region is enough).
    bool prepare_jit_block(void* code_area, size_t size, void** code_area_rw, ptrdiff_t* rx_offset)
    {
        if (!prepare_jit_block(code_area, size, code_area_rw)) return false;
        *rx_offset = 0;
        return true;
    }

    void release_jit_block(void* code_area, size_t /*size*/)
    {
        if (code_area) VirtualFree(code_area, 0, MEM_RELEASE);
    }
    void release_jit_block(void* code_area1, void* /*code_area2*/, size_t size)
    {
        release_jit_block(code_area1, size);
    }

    // x86: no explicit icache flush needed (coherent on write to RWX pages).
    void flush_cache(void* /*icache_start*/, void* /*icache_end*/,
                     void* /*dcache_start*/, void* /*dcache_end*/) {}

    // Memory is always RWX; no permission toggle needed.
    void jit_set_exec(void* /*code*/, size_t /*size*/, bool /*enable*/) {}

    bool region_set_exec(void* start, size_t len)
    {
        DWORD old;
        return VirtualProtect(start, len, PAGE_EXECUTE_READWRITE, &old) != 0;
    }
}
