// ============================================================================
//  xbox_d3d8_renderer.cpp  — Flycast D3D8 renderer for OG Xbox
// ============================================================================
#include "hw/pvr/ta.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/ta_structs.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/TexCache.h"
#include "rend/texconv.h"      // palette32_ram[] -- the converted DC palette
#include <vector>
#include <string>
#include <cstdlib>
#include <cstring>

#include <xtl.h>
#include <XGraphics.h>
#include <intrin.h>      // __rdtsc (texture-upload profiler)
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

extern "C" void __stdcall OutputDebugStringA(const char*);
#define RDBG(s) OutputDebugStringA(s)

extern IDirect3DDevice8* g_xbox_d3d_dev;

// Bumped whenever the DC palette RAM changes (Renderer::updatePalette fires).
// Hardware palettes (g_pals) refresh their colours when they fall behind this.
static u32 g_palGen = 1;

// Perf probe: total microseconds spent inside our D3D8 Render()+Present() this
// session. main_xbox.cpp samples the delta every 60 frames to split render cost
// from emulation cost. Defined here, read via extern in main.
volatile long long g_renderUs = 0;
extern "C" volatile long long g_prof_tex_cyc;   // texture-upload profiler (defined in main_xbox)

// Render diagnostics: figure out what "rend" cost is actually bound by (call
// count vs vertex/index throughput) instead of guessing. main_xbox.cpp samples
// deltas every 60 frames.
extern "C" {
volatile unsigned g_stat_drawCalls  = 0;   // DrawIndexedPrimitive calls
volatile unsigned g_stat_stateCalls = 0;   // actual Set*() calls issued (post-cache)
volatile unsigned g_stat_stateSkipped = 0; // Set*() calls the cache skipped
volatile unsigned g_stat_verts      = 0;   // rc->verts.size() (clamped) per frame
volatile unsigned g_stat_idx        = 0;   // rc->idx.size() (clamped) per frame
volatile unsigned g_stat_polys      = 0;   // total PolyParam entries processed per frame
unsigned g_stat_vbLockUs            = 0;   // us spent blocked in VB/IB Lock (GPU-sync stalls)
}
static inline long long qpcNow()
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart;
}
static long long g_qpcFreq = 0;
static inline long long qpcToUs(long long ticks)
{
    if (!g_qpcFreq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcFreq = f.QuadPart; }
    return ticks * 1000000 / g_qpcFreq;
}

// ---------------------------------------------------------------------------
//  Pre-transformed screen-space vertex.
//  DC stores z as 1/w; that maps directly to XYZRHW's rhw field.
// ---------------------------------------------------------------------------
struct TLVert
{
    float    x, y, z, rhw;
    D3DCOLOR diffuse;
    D3DCOLOR specular;
    float    u, v;
};
static const DWORD k_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1;

// The TA parser defaults to RendererType=OpenGL so it packs vertex colors RGBA:
//   col[0]=R  col[1]=G  col[2]=B  col[3]=A
// D3DCOLOR wants ARGB (A<<24 | R<<16 | G<<8 | B). Convert byte-by-byte.
static inline D3DCOLOR cvtCol(const u8 c[4])
{
    return ((D3DCOLOR)c[3] << 24)
         | ((D3DCOLOR)c[0] << 16)
         | ((D3DCOLOR)c[1] <<  8)
         |  (D3DCOLOR)c[2];
}

// ---------------------------------------------------------------------------
//  Blend factor tables (indexed by TSP SrcInstr / DstInstr)
// ---------------------------------------------------------------------------
static const D3DBLEND k_src[8] = {
    D3DBLEND_ZERO,       D3DBLEND_ONE,
    D3DBLEND_DESTCOLOR,  D3DBLEND_INVDESTCOLOR,
    D3DBLEND_SRCALPHA,   D3DBLEND_INVSRCALPHA,
    D3DBLEND_DESTALPHA,  D3DBLEND_INVDESTALPHA,
};
static const D3DBLEND k_dst[8] = {
    D3DBLEND_ZERO,       D3DBLEND_ONE,
    D3DBLEND_SRCCOLOR,   D3DBLEND_INVSRCCOLOR,
    D3DBLEND_SRCALPHA,   D3DBLEND_INVSRCALPHA,
    D3DBLEND_DESTALPHA,  D3DBLEND_INVDESTALPHA,
};

// ---------------------------------------------------------------------------
//  Hardware palette cache (for gpuPalette / P8 textures)
//
//  The DC reuses ONE index texture with different 256-entry palette windows
//  (tcw.PalSelect) per draw. Instead of baking a separate RGBA texture per
//  PalSelect (which churns texture memory on every palette change), we keep one
//  native D3DPalette per window offset, shared across all paletted textures, and
//  only refresh its colours (a 256-DWORD write) when DC palette RAM changes.
//  PAL8 window = (PalSelect>>4)<<8 (256 entries); PAL4 = PalSelect<<4 (16 used).
// ---------------------------------------------------------------------------
struct XboxPal { u32 off; u32 gen; D3DPalette* pal; };
static std::vector<XboxPal> g_pals;

static D3DPalette* GetXboxPalette(u32 off)
{
    XboxPal* e = nullptr;
    for (auto& p : g_pals) if (p.off == off) { e = &p; break; }
    if (!e)
    {
        D3DPalette* pal = nullptr;
        if (FAILED(g_xbox_d3d_dev->CreatePalette(D3DPALETTE_256, &pal)) || !pal)
            return nullptr;
        g_pals.push_back({ off, 0u, pal });
        e = &g_pals.back();
    }
    if (e->gen != g_palGen)        // DC palette RAM changed -> refresh in place
    {
        D3DCOLOR* col = nullptr;
        if (SUCCEEDED(e->pal->Lock(&col, 0)) && col)
        {
            for (int i = 0; i < 256; ++i)
            {
                // palette32_ram is RGBA (0xAABBGGRR, OpenGL RendererType); swap
                // R<->B to D3D ARGB, exactly like cvtCol does for vertex colours.
                u32 p = palette32_ram[(i + off) & 1023];
                col[i] = (p & 0xFF00FF00u) | ((p & 0x000000FFu) << 16) | ((p >> 16) & 0xFFu);
            }
            e->pal->Unlock();
        }
        e->gen = g_palGen;
    }
    return e->pal;
}

// ---------------------------------------------------------------------------
//  Texture cache
//  NV2A mis-samples 16-bit swizzled formats — expand everything to A8R8G8B8
//  then XGSwizzleRect so the GPU samples correctly. Paletted textures stay P8.
// ---------------------------------------------------------------------------
// Texture-cache accounting (read by main_xbox for the byte-budget eviction and
// the MEMSTAT line): bytes of committed D3D texture memory + live texture count.
extern "C" unsigned g_texCacheBytes = 0;
extern "C" unsigned g_texCacheCount = 0;

class XboxTex final : public BaseTextureCacheData
{
public:
    // For non-paletted textures this is an A8R8G8B8 texture; for gpuPalette
    // (_8) textures it is a native P8 index texture (1 byte/pixel) whose colours
    // come from a hardware palette bound per-draw -- see GetXboxPalette / applyPoly.
    IDirect3DTexture8* d3dtex = nullptr;
    u32 texBytes = 0;   // committed size, for the byte-budget accounting

    XboxTex(TSP tsp={}, TCW tcw={}, int area=0) : BaseTextureCacheData(tsp, tcw, area) {}
    XboxTex(XboxTex&& o) : BaseTextureCacheData(std::move(o))
    {
        std::swap(d3dtex, o.d3dtex);
        std::swap(texBytes, o.texBytes);
    }

    std::string GetId() override { return std::to_string((uintptr_t)d3dtex); }

    bool createTex(int w, int h, D3DFORMAT fmt, u32 bpp)
    {
        if (d3dtex)
            return true;
        if (FAILED(g_xbox_d3d_dev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_DEFAULT, &d3dtex)))
        {
            // Out of memory: signal the byte-budget eviction to clear room; this
            // texture just doesn't draw this frame and retries on next lookup.
            extern volatile int textureMemPressure;
            textureMemPressure = 1;
            d3dtex = nullptr;
            return false;
        }
        texBytes = (u32)w * (u32)h * bpp;
        g_texCacheBytes += texBytes;
        g_texCacheCount++;
        return true;
    }

    // CreateTexture (lazily) + swizzle a finished A8R8G8B8 buffer into it.
    void commitPx(const u32* px, int w, int h)
    {
        if (!createTex(w, h, D3DFMT_A8R8G8B8, 4))
            return;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(d3dtex->LockRect(0, &lr, NULL, 0)))
        {
            XGSwizzleRect((void*)px, w * 4, NULL, lr.pBits, w, h, NULL, 4);
            d3dtex->UnlockRect(0);
        }
    }

    // Native 16-bit commit -- half the VRAM of expanding to A8R8G8B8. DC's
    // 565/1555/4444 layouts match D3DFMT_R5G6B5/A1R5G5B5/A4R4G4B4 bit-for-bit
    // (SetDirectXColorOrder), so this is a straight 2-byte swizzle, no conversion.
    void commitPx16(const void* px, int w, int h, D3DFORMAT fmt)
    {
        if (!createTex(w, h, fmt, 2))
            return;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(d3dtex->LockRect(0, &lr, NULL, 0)))
        {
            XGSwizzleRect((void*)px, w * 2, NULL, lr.pBits, w, h, NULL, 2); // 2 bytes/px
            d3dtex->UnlockRect(0);
        }
    }

    void UploadToGPU(int w, int h, const u8* src, bool, bool) override
    {
        struct _TexT { unsigned long long t0; ~_TexT(){ g_prof_tex_cyc += (long long)(__rdtsc() - t0); } } _texT{ __rdtsc() };
        const int n = w * h;

        // Paletted (PAL4/PAL8) textures reach us as palette INDICES (_8 /
        // gpuPalette). One index texture is reused with many palette windows
        // (tcw.PalSelect) per draw -- that's how e.g. Mr.Driller gets several
        // block colours from one sprite. Upload the indices ONCE as a native P8
        // texture (4x smaller than RGBA, no per-PalSelect duplication); the DC
        // palette is applied by the hardware palette bound in applyPoly. This is
        // exactly what the NV2A's paletted-texture path is for.
        if (tex_type == TextureType::_8)
        {
            if (!createTex(w, h, D3DFMT_P8, 1))
                return;
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(d3dtex->LockRect(0, &lr, NULL, 0)))
            {
                XGSwizzleRect((void*)src, w, NULL, lr.pBits, w, h, NULL, 1); // 1 byte/px
                d3dtex->UnlockRect(0);
            }
            return;
        }

        // Keep DC 16-bit textures NATIVE 16-bit (half the VRAM of expanding to
        // A8R8G8B8) -- decisive for big WinCE games (SA1) that otherwise exhaust
        // the 64MB Xbox. Layouts match D3D 16-bit formats 1:1 so it's a straight
        // swizzle. Only true 32-bit (_8888) stays A8R8G8B8.
        (void)n;
        switch (tex_type)
        {
        case TextureType::_565:  commitPx16(src, w, h, D3DFMT_R5G6B5);   break;
        case TextureType::_5551: commitPx16(src, w, h, D3DFMT_A1R5G5B5); break;
        case TextureType::_4444: commitPx16(src, w, h, D3DFMT_A4R4G4B4); break;
        case TextureType::_8888: commitPx((const u32*)src, w, h);        break;
        default: return;
        }
    }

    bool Delete() override
    {
        if (!BaseTextureCacheData::Delete()) return false;
        if (d3dtex)
        {
            d3dtex->Release();
            d3dtex = nullptr;
            g_texCacheBytes -= texBytes;
            g_texCacheCount--;
            texBytes = 0;
        }
        return true;
    }
};

class XboxTexCache final : public BaseTextureCache<XboxTex>
{
public:
    XboxTexCache()  { XboxTex::SetDirectXColorOrder(true); }
    ~XboxTexCache() { Clear(); }
};

// ---------------------------------------------------------------------------
//  Renderer
// ---------------------------------------------------------------------------
static const int MAX_V = 32768;
static const int MAX_I = 65536;

struct XboxD3D8Renderer final : Renderer
{
    IDirect3DDevice8*       dev  = nullptr;
    // Triple-buffered geometry: a single VB/IB locked with flag 0 every frame
    // BLOCKS until the GPU finishes reading it -- invisible in calm scenes (GPU
    // idles early) but multi-millisecond in texture-storm scenes (GPU saturated
    // by upload DMA), which is exactly the measured rend inflation 1.3ms->8-9ms
    // at equal draw counts. Rotating 3 buffers means the one we lock was last
    // touched by the GPU two frames ago: never contended. Cost: ~1.9MB extra.
    static const int GEO_BUFS = 3;
    IDirect3DVertexBuffer8* vbs[GEO_BUFS] = {};
    IDirect3DIndexBuffer8*  ibs[GEO_BUFS] = {};
    int                     geoBuf = 0;
    IDirect3DSurface8*      bb   = nullptr;   // backbuffer (D3DSWAPEFFECT_COPY keeps it alive)
    IDirect3DSurface8*      ds   = nullptr;   // depth+stencil
    XboxTexCache            tc;
    rend_context*           rc      = nullptr;
    int                     m_nv   = 0;
    int                     m_ni   = 0;
    int                     s_frame = 0;

    // applyPoly state cache: consecutive polys (often most of a mesh/list)
    // commonly share identical material state, but every D3D Set*() call has
    // real driver-side cost (nv2a pushbuffer command write + validation) even
    // when the value is unchanged. Skip the call when it is. Sentinels never
    // match a real value, so the very first applyPoly call always sets fresh
    // state regardless of whatever the file browser (a separate D3D user,
    // finished before the renderer starts) left behind.
    IDirect3DTexture8* c_tex      = (IDirect3DTexture8*)(intptr_t)-1;
    D3DPalette*        c_pal      = (D3DPalette*)(intptr_t)-1;
    DWORD c_addrU = 0xFFFFFFFF, c_addrV = 0xFFFFFFFF;
    DWORD c_minFilt = 0xFFFFFFFF, c_magFilt = 0xFFFFFFFF, c_mipFilt = 0xFFFFFFFF;
    DWORD c_colorArg1 = 0xFFFFFFFF, c_colorArg2 = 0xFFFFFFFF, c_colorOp = 0xFFFFFFFF;
    DWORD c_alphaArg1 = 0xFFFFFFFF, c_alphaArg2 = 0xFFFFFFFF, c_alphaOp = 0xFFFFFFFF;
    DWORD c_specular  = 0xFFFFFFFF, c_srcBlend   = 0xFFFFFFFF, c_dstBlend  = 0xFFFFFFFF;
    int                     s_fault = 0;

    bool Init() override
    {
        dev = g_xbox_d3d_dev;
        if (!dev) return false;

        for (int i = 0; i < GEO_BUFS; ++i)
        {
            if (FAILED(dev->CreateVertexBuffer(MAX_V * sizeof(TLVert), 0, 0,
                           D3DPOOL_DEFAULT, &vbs[i]))) return false;

            if (FAILED(dev->CreateIndexBuffer(MAX_I * 2, 0, D3DFMT_INDEX16,
                           D3DPOOL_DEFAULT, &ibs[i]))) return false;
        }

        dev->GetBackBuffer(0, (D3DBACKBUFFER_TYPE)0, &bb);
        dev->GetDepthStencilSurface(&ds);

        RDBG("FLYCAST: XboxD3D8Renderer::Init OK\n");
        return bb != nullptr;
    }

    void Term() override
    {
        for (int i = 0; i < GEO_BUFS; ++i)
        {
            if (vbs[i]) { vbs[i]->Release(); vbs[i] = nullptr; }
            if (ibs[i]) { ibs[i]->Release(); ibs[i] = nullptr; }
        }
        if (bb) { bb->Release(); bb = nullptr; }
        if (ds) { ds->Release(); ds = nullptr; }
    }

    BaseTextureCacheData* GetTexture(TSP tsp, TCW tcw, int area) override
    {
        XboxTex* t = tc.getTextureCacheData(tsp, tcw, area);
        if (t->NeedsUpdate())
            t->Update();
        // gpuPalette colour staleness is handled in GetXboxPalette(): the shared
        // hardware palette refreshes its entries when g_palGen advances. The P8
        // index texture itself never needs re-uploading on a palette change.
        return t;
    }

    void Process(TA_context* ctx) override
    {
        rc = &ctx->rend;

        // The DC palette changed -> bump the generation so the shared hardware
        // palettes refresh their colours next time they're bound (cheap).
        if (updatePalette) { ++g_palGen; updatePalette = false; }

        // ta_parse handles every real PVR param type; a genuinely malformed
        // stream throws TAParserException, which the catch below absorbs (the
        // EH machinery is fixed now -- see xbox_eh_shim.cpp). The old fixed-step
        // "bad ParaType 3/6" pre-scan is gone: it walked the variable-length TA
        // blocks in blind 32-byte steps, landed mid-vertex, misread coordinate
        // floats (e.g. -36.0f = 0xC2100000, top 3 bits = 6) as control words,
        // and dropped whole valid frames. Just let ta_parse do its job.
        try { ta_parse(ctx, false); }
        catch (...) { rc = nullptr; ++s_fault; RDBG("FLYCAST: ta_parse throw\n"); }
        tc.CollectCleanup();
    }

    void applyPoly(const PolyParam& pp)
    {
        XboxTex* t = (pp.pcw.Texture && pp.texture)
                   ? static_cast<XboxTex*>(pp.texture)
                   : nullptr;

        // Resolve the desired state into locals first (matching the original
        // textured/untextured branches exactly), then apply once with change
        // detection below. Consecutive polys in a mesh/list very often share
        // identical material state, but every Set*() call has real driver-side
        // cost (nv2a pushbuffer command write + validation) even when the value
        // is unchanged -- skip it when it is.
        IDirect3DTexture8* wantTex = nullptr;
        DWORD wantAddrU = 0, wantAddrV = 0, wantMinFilt = 0, wantMagFilt = 0, wantMipFilt = D3DTEXF_NONE;
        DWORD wantColorArg1 = 0, wantColorArg2 = 0, wantColorOp = 0;
        DWORD wantAlphaArg1 = 0, wantAlphaArg2 = 0, wantAlphaOp = 0;

        if (t && t->d3dtex)
        {
            // gpuPalette (P8) textures: bind the hardware palette for THIS poly's
            // PalSelect window before the texture. One index texture, many colours.
            if (t->gpuPalette)
            {
                u32 off = (pp.tcw.PixelFmt == PixelPal4)
                        ? ((u32)pp.tcw.PalSelect << 4)
                        : (((u32)pp.tcw.PalSelect >> 4) << 8);
                D3DPalette* pal = GetXboxPalette(off);
                if (pal && pal != c_pal) { dev->SetPalette(0, pal); c_pal = pal; }
            }

            wantTex = t->d3dtex;
            wantAddrU = pp.tsp.ClampU ? D3DTADDRESS_CLAMP : pp.tsp.FlipU ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP;
            wantAddrV = pp.tsp.ClampV ? D3DTADDRESS_CLAMP : pp.tsp.FlipV ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP;

            DWORD filt = pp.tsp.FilterMode ? D3DTEXF_LINEAR : D3DTEXF_POINT;
            wantMinFilt = filt;
            wantMagFilt = filt;
            wantMipFilt = D3DTEXF_NONE;

            // Colour combine — matches the PC dx11 pixel shader exactly:
            //   ShadInstr 0  decal:   out.rgb = tex.rgb
            //   ShadInstr 1  modulate: out.rgb = tex.rgb * diff.rgb
            //   ShadInstr 2  decal-a:  out.rgb = lerp(diff.rgb, tex.rgb, tex.a)
            //   ShadInstr 3  mod-a:    out.rgb = tex.rgb * diff.rgb
            wantColorArg1 = D3DTA_TEXTURE;
            wantColorArg2 = D3DTA_DIFFUSE;
            switch (pp.tsp.ShadInstr)
            {
            case 0: wantColorOp = D3DTOP_SELECTARG1; break;
            case 2: wantColorOp = D3DTOP_BLENDTEXTUREALPHA; break;
            default: wantColorOp = D3DTOP_MODULATE; break;
            }

            // Alpha combine
            wantAlphaArg1 = D3DTA_TEXTURE;
            wantAlphaArg2 = D3DTA_DIFFUSE;
            if (pp.tsp.ShadInstr == 2)
                wantAlphaOp = D3DTOP_SELECTARG2;
            else if (pp.tsp.ShadInstr == 3)
                wantAlphaOp = pp.tsp.IgnoreTexA ? D3DTOP_SELECTARG2 : D3DTOP_MODULATE;
            else
                wantAlphaOp = pp.tsp.IgnoreTexA ? D3DTOP_SELECTARG2 : D3DTOP_SELECTARG1;
        }
        else
        {
            wantColorOp   = D3DTOP_SELECTARG1;
            wantColorArg1 = D3DTA_DIFFUSE;
            wantAlphaOp   = D3DTOP_SELECTARG1;
            wantAlphaArg1 = D3DTA_DIFFUSE;
            // ARG2 intentionally left unresolved/uncompared here, matching the
            // original: the untextured path never touched COLORARG2/ALPHAARG2,
            // relying on SELECTARG1 not referencing them. See the "if (wantTex)"
            // guard below, which mirrors that exactly.
        }

#define APPLY_STATE(cache, want, callexpr) \
        do { if ((want) != (cache)) { callexpr; (cache) = (want); ++g_stat_stateCalls; } \
             else ++g_stat_stateSkipped; } while (0)

        APPLY_STATE(c_tex, wantTex, dev->SetTexture(0, wantTex));

        if (wantTex)   // sampler/filter state is only ever set in the textured path (matches original)
        {
            APPLY_STATE(c_addrU,   wantAddrU,   dev->SetTextureStageState(0, D3DTSS_ADDRESSU,  wantAddrU));
            APPLY_STATE(c_addrV,   wantAddrV,   dev->SetTextureStageState(0, D3DTSS_ADDRESSV,  wantAddrV));
            APPLY_STATE(c_minFilt, wantMinFilt, dev->SetTextureStageState(0, D3DTSS_MINFILTER, wantMinFilt));
            APPLY_STATE(c_magFilt, wantMagFilt, dev->SetTextureStageState(0, D3DTSS_MAGFILTER, wantMagFilt));
            APPLY_STATE(c_mipFilt, wantMipFilt, dev->SetTextureStageState(0, D3DTSS_MIPFILTER, wantMipFilt));

            APPLY_STATE(c_colorArg2, wantColorArg2, dev->SetTextureStageState(0, D3DTSS_COLORARG2, wantColorArg2));
            APPLY_STATE(c_alphaArg2, wantAlphaArg2, dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, wantAlphaArg2));
        }

        APPLY_STATE(c_colorArg1, wantColorArg1, dev->SetTextureStageState(0, D3DTSS_COLORARG1, wantColorArg1));
        APPLY_STATE(c_colorOp,   wantColorOp,   dev->SetTextureStageState(0, D3DTSS_COLOROP,   wantColorOp));
        APPLY_STATE(c_alphaArg1, wantAlphaArg1, dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, wantAlphaArg1));
        APPLY_STATE(c_alphaOp,   wantAlphaOp,   dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   wantAlphaOp));

        // Specular offset add (only when the poly requests it)
        DWORD wantSpecular = pp.pcw.Offset ? TRUE : FALSE;
        APPLY_STATE(c_specular, wantSpecular, dev->SetRenderState(D3DRS_SPECULARENABLE, wantSpecular));

        // Blend — enabled for ALL lists; opaque polys carry SRC=ONE/DST=ZERO (no-op)
        DWORD wantSrc = k_src[pp.tsp.SrcInstr & 7];
        DWORD wantDst = k_dst[pp.tsp.DstInstr & 7];
        APPLY_STATE(c_srcBlend, wantSrc, dev->SetRenderState(D3DRS_SRCBLEND,  wantSrc));
        APPLY_STATE(c_dstBlend, wantDst, dev->SetRenderState(D3DRS_DESTBLEND, wantDst));

        ++g_stat_polys;
#undef APPLY_STATE
    }

    void drawList(std::vector<PolyParam>& polys)
    {
        for (const PolyParam& pp : polys)
        {
            if (pp.count < 3) continue;
            if ((int)(pp.first + pp.count) > m_ni) continue;
            applyPoly(pp);
            dev->DrawIndexedPrimitive(D3DPT_TRIANGLESTRIP,
                                      0, m_nv, pp.first, pp.count - 2);
            ++g_stat_drawCalls;
        }
    }

    // Auto-sorted translucents: ta_util builds a back-to-front triangle list in
    // rc->idx and fills rc->sortedTriangles with per-run offsets into it.
    // global_param_tr holds the material state (polyIndex is the lookup key).
    void drawSortedTr()
    {
        // ta_util emits the back-to-front sorted triangles as one contiguous run
        // in rc->idx (first_{k+1} == first_k + count_k), one SortedTriangle per
        // triangle. Drawing them individually was ~850 DrawIndexedPrimitive calls
        // per frame, each re-binding full material state (xemu showed BEGIN_ENDS
        // /SHADER_BIND ~848). Coalesce adjacent triangles that share a material
        // AND are index-contiguous into a single draw; state is applied once per
        // run. Order is preserved because only adjacent entries merge.
        const std::vector<SortedTriangle>& tris = rc->sortedTriangles;
        const size_t n = tris.size();
        size_t i = 0;
        while (i < n)
        {
            const SortedTriangle& st = tris[i];
            if (st.count < 3
                || (int)(st.first + st.count) > m_ni
                || st.polyIndex >= rc->global_param_tr.size())
            {
                ++i;
                continue;
            }
            const u32 poly      = st.polyIndex;
            u32       runFirst  = st.first;
            u32       runCount  = st.count;
            size_t    j         = i + 1;
            // Extend the run while same material + contiguous + in-bounds.
            while (j < n)
            {
                const SortedTriangle& nx = tris[j];
                if (nx.polyIndex != poly)            break;
                if (nx.first != runFirst + runCount) break;
                if (nx.count < 3)                    break;
                if ((int)(nx.first + nx.count) > m_ni) break;
                runCount += nx.count;
                ++j;
            }
            applyPoly(rc->global_param_tr[poly]);
            dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
                                      0, m_nv, runFirst, runCount / 3);
            ++g_stat_drawCalls;
            i = j;
        }
    }

    bool Render() override
    {
        const long long _t0 = qpcNow();
        struct _Acc { long long t0; ~_Acc(){ g_renderUs += qpcToUs(qpcNow() - t0); } } _acc{_t0};
        if (!dev || !rc || rc->isRTT)
            return false;

        int nv = (int)rc->verts.size();
        int ni = (int)rc->idx.size();

        // With D3DSWAPEFFECT_COPY the backbuffer is preserved after Present(),
        // so returning true here (no draw) leaves the last good frame on screen.
        if (nv < 3 || ni < 3)
            return true;

        if (nv > MAX_V) nv = MAX_V;
        if (ni > MAX_I) ni = MAX_I;

        g_stat_verts = (unsigned)nv;
        g_stat_idx   = (unsigned)ni;

        // DC depth is a per-vertex 1/w (larger = nearer). XYZRHW uses the .z
        // field as the [0,1] z-buffer value (rhw stays = 1/w for perspective-
        // correct texturing). Normalize 1/w across the frame: nearest (max 1/w)
        // -> 0, farthest -> 1, paired with ZFUNC LESSEQUAL so near wins. Robust
        // by construction (always in range); a degenerate range (pure 2D, all
        // verts same 1/w) collapses to z=0 so painter's order via LESSEQUAL holds.
        float minW = 3.4e38f, maxW = -3.4e38f;
        for (int i = 0; i < nv; ++i)
        {
            float w = rc->verts[i].z;
            if (w <= 0.f) continue;
            if (w < minW) minW = w;
            if (w > maxW) maxW = w;
        }
        const float wRange = maxW - minW;
        const bool  flatZ  = !(wRange > 1e-9f);

        // Rotate to the geometry buffers the GPU finished with two frames ago
        // (see the GEO_BUFS comment at the members).
        geoBuf = (geoBuf + 1) % GEO_BUFS;
        IDirect3DVertexBuffer8* vb = vbs[geoBuf];
        IDirect3DIndexBuffer8*  ib = ibs[geoBuf];

        // Upload vertices (lock-stall time measured into g_stat_vbLockUs so the
        // triple-buffering fix is verifiable from the RENDER log line).
        const long long _lockT0 = qpcNow();
        TLVert* vp = nullptr;
        if (FAILED(vb->Lock(0, 0, (BYTE**)&vp, 0)) || !vp) return false;
        g_stat_vbLockUs += (unsigned)qpcToUs(qpcNow() - _lockT0);
        for (int i = 0; i < nv; ++i)
        {
            const Vertex& v = rc->verts[i];
            float w = v.z > 0.f ? v.z : 1e-10f;
            float z = flatZ ? 0.0f : (maxW - w) / wRange;   // near -> 0
            if (z < 0.f) z = 0.f; else if (z > 1.f) z = 1.f;
            vp[i].x        = v.x;
            vp[i].y        = v.y;
            vp[i].z        = z;
            vp[i].rhw      = w;
            vp[i].diffuse  = cvtCol(v.col);
            vp[i].specular = cvtCol(v.spc);
            vp[i].u        = v.u;
            vp[i].v        = v.v;
        }
        vb->Unlock();

        // Upload indices (u32 → u16, clamp out-of-range to 0)
        const long long _lockT1 = qpcNow();
        u16* ip = nullptr;
        if (FAILED(ib->Lock(0, 0, (BYTE**)&ip, 0)) || !ip) return false;
        g_stat_vbLockUs += (unsigned)qpcToUs(qpcNow() - _lockT1);
        for (int i = 0; i < ni; ++i)
        {
            u32 idx = rc->idx[i];
            ip[i] = (u16)(idx < (u32)nv ? idx : 0u);
        }
        ib->Unlock();

        m_nv = nv;
        m_ni = ni;

        // ---- Render --------------------------------------------------------
        dev->SetRenderTarget(bb, ds);
        dev->Clear(0, NULL,
                   D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                   D3DCOLOR_XRGB(0, 0, 0), 1.f, 0);

        dev->SetVertexShader(k_FVF);
        dev->SetStreamSource(0, vb, sizeof(TLVert));
        dev->SetIndices(ib, 0);

        dev->SetRenderState(D3DRS_ZENABLE,  TRUE);
        dev->SetRenderState(D3DRS_ZFUNC,    D3DCMP_LESSEQUAL);   // near (z=0) wins
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

        // Disable stage 1+ so they don't interfere
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        // 1) Opaque: write depth, no blend, no alpha test.
        dev->SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
        drawList(rc->global_param_op);

        // 2) Punch-through: write depth, alpha-test discards the transparent
        //    texels (1-bit alpha) so they don't occlude what's behind them.
        dev->SetRenderState(D3DRS_ALPHATESTENABLE,  TRUE);
        dev->SetRenderState(D3DRS_ALPHAREF,         0x80);
        dev->SetRenderState(D3DRS_ALPHAFUNC,        D3DCMP_GREATEREQUAL);
        drawList(rc->global_param_pt);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);

        // 3) Translucent: test depth against the opaque/PT pass but DON'T write
        //    it, blend, drawn back-to-front. When autosort is active ta_util
        //    builds the sorted triangle list in rc->sortedTriangles (and does
        //    NOT makeIndex global_param_tr, whose first/count are then raw vertex
        //    ranges) -- so use drawSortedTr() whenever that list is populated.
        dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        if (!rc->sortedTriangles.empty())
            drawSortedTr();
        else
            drawList(rc->global_param_tr);

        return true;
    }

    void RenderFramebuffer(const FramebufferInfo&) override {}

    bool Present() override
    {
        const long long _t0 = qpcNow();
        if (dev) dev->Present(NULL, NULL, NULL, NULL);
        g_renderUs += qpcToUs(qpcNow() - _t0);
        return true;
    }
};

Renderer* rend_DirectX9() { return new XboxD3D8Renderer(); }
