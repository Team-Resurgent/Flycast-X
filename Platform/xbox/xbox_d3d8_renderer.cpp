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
class XboxTex final : public BaseTextureCacheData
{
public:
    // For non-paletted textures this is an A8R8G8B8 texture; for gpuPalette
    // (_8) textures it is a native P8 index texture (1 byte/pixel) whose colours
    // come from a hardware palette bound per-draw -- see GetXboxPalette / applyPoly.
    IDirect3DTexture8* d3dtex = nullptr;

    XboxTex(TSP tsp={}, TCW tcw={}, int area=0) : BaseTextureCacheData(tsp, tcw, area) {}
    XboxTex(XboxTex&& o) : BaseTextureCacheData(std::move(o))
    {
        std::swap(d3dtex, o.d3dtex);
    }

    std::string GetId() override { return std::to_string((uintptr_t)d3dtex); }

    // CreateTexture (lazily) + swizzle a finished A8R8G8B8 buffer into it.
    void commitPx(const u32* px, int w, int h)
    {
        if (!d3dtex
            && FAILED(g_xbox_d3d_dev->CreateTexture(w, h, 1, 0,
                          D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &d3dtex)))
            return;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(d3dtex->LockRect(0, &lr, NULL, 0)))
        {
            XGSwizzleRect((void*)px, w * 4, NULL, lr.pBits, w, h, NULL, 4);
            d3dtex->UnlockRect(0);
        }
    }

    void UploadToGPU(int w, int h, const u8* src, bool, bool) override
    {
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
            if (!d3dtex
                && FAILED(g_xbox_d3d_dev->CreateTexture(w, h, 1, 0,
                              D3DFMT_P8, D3DPOOL_MANAGED, &d3dtex)))
                return;
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(d3dtex->LockRect(0, &lr, NULL, 0)))
            {
                XGSwizzleRect((void*)src, w, NULL, lr.pBits, w, h, NULL, 1); // 1 byte/px
                d3dtex->UnlockRect(0);
            }
            return;
        }

        u32* px = (u32*)malloc(n * 4);
        if (!px) return;

        const u16* s = (const u16*)src;
        switch (tex_type)
        {
        case TextureType::_565:
            for (int i = 0; i < n; ++i)
            {
                u32 t = s[i];
                px[i] = 0xff000000u
                      | (((t >> 11) & 31) * 255 / 31 << 16)
                      | (((t >>  5) & 63) * 255 / 63 <<  8)
                      | (( t        & 31) * 255 / 31);
            }
            break;

        case TextureType::_5551:
            for (int i = 0; i < n; ++i)
            {
                u32 t = s[i];
                px[i] = ((t >> 15 & 1) ? 0xff000000u : 0u)
                      | ((t >> 10 & 31) * 255 / 31 << 16)
                      | ((t >>  5 & 31) * 255 / 31 <<  8)
                      | (( t      & 31) * 255 / 31);
            }
            break;

        case TextureType::_4444:
            for (int i = 0; i < n; ++i)
            {
                u32 t = s[i];
                px[i] = ((t >> 12 & 15) * 17u << 24)
                      | ((t >>  8 & 15) * 17u << 16)
                      | ((t >>  4 & 15) * 17u <<  8)
                      | (( t      & 15) * 17u);
            }
            break;

        case TextureType::_8888:
            memcpy(px, src, n * 4);
            break;

        default:
            free(px);
            return;
        }

        commitPx(px, w, h);
        free(px);
    }

    bool Delete() override
    {
        if (!BaseTextureCacheData::Delete()) return false;
        if (d3dtex) { d3dtex->Release(); d3dtex = nullptr; }
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
    IDirect3DVertexBuffer8* vb   = nullptr;
    IDirect3DIndexBuffer8*  ib   = nullptr;
    IDirect3DSurface8*      bb   = nullptr;   // backbuffer (D3DSWAPEFFECT_COPY keeps it alive)
    IDirect3DSurface8*      ds   = nullptr;   // depth+stencil
    XboxTexCache            tc;
    rend_context*           rc      = nullptr;
    int                     m_nv   = 0;
    int                     m_ni   = 0;
    int                     s_frame = 0;
    int                     s_fault = 0;

    bool Init() override
    {
        dev = g_xbox_d3d_dev;
        if (!dev) return false;

        if (FAILED(dev->CreateVertexBuffer(MAX_V * sizeof(TLVert), 0, 0,
                       D3DPOOL_DEFAULT, &vb))) return false;

        if (FAILED(dev->CreateIndexBuffer(MAX_I * 2, 0, D3DFMT_INDEX16,
                       D3DPOOL_DEFAULT, &ib))) return false;

        dev->GetBackBuffer(0, (D3DBACKBUFFER_TYPE)0, &bb);
        dev->GetDepthStencilSurface(&ds);

        RDBG("FLYCAST: XboxD3D8Renderer::Init OK\n");
        return bb != nullptr;
    }

    void Term() override
    {
        if (vb) { vb->Release(); vb = nullptr; }
        if (ib) { ib->Release(); ib = nullptr; }
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
                if (pal) dev->SetPalette(0, pal);
            }

            dev->SetTexture(0, t->d3dtex);

            dev->SetTextureStageState(0, D3DTSS_ADDRESSU,
                pp.tsp.ClampU ? D3DTADDRESS_CLAMP :
                pp.tsp.FlipU  ? D3DTADDRESS_MIRROR :
                                D3DTADDRESS_WRAP);
            dev->SetTextureStageState(0, D3DTSS_ADDRESSV,
                pp.tsp.ClampV ? D3DTADDRESS_CLAMP :
                pp.tsp.FlipV  ? D3DTADDRESS_MIRROR :
                                D3DTADDRESS_WRAP);

            DWORD filt = pp.tsp.FilterMode ? D3DTEXF_LINEAR : D3DTEXF_POINT;
            dev->SetTextureStageState(0, D3DTSS_MINFILTER, filt);
            dev->SetTextureStageState(0, D3DTSS_MAGFILTER, filt);
            dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);

            // Colour combine — matches the PC dx11 pixel shader exactly:
            //   ShadInstr 0  decal:   out.rgb = tex.rgb
            //   ShadInstr 1  modulate: out.rgb = tex.rgb * diff.rgb
            //   ShadInstr 2  decal-a:  out.rgb = lerp(diff.rgb, tex.rgb, tex.a)
            //   ShadInstr 3  mod-a:    out.rgb = tex.rgb * diff.rgb
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            switch (pp.tsp.ShadInstr)
            {
            case 0:
                dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                break;
            case 2:
                dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);
                break;
            default:
                dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                break;
            }

            // Alpha combine
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            if (pp.tsp.ShadInstr == 2)
            {
                dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            }
            else if (pp.tsp.ShadInstr == 3)
            {
                dev->SetTextureStageState(0, D3DTSS_ALPHAOP,
                    pp.tsp.IgnoreTexA ? D3DTOP_SELECTARG2 : D3DTOP_MODULATE);
            }
            else
            {
                dev->SetTextureStageState(0, D3DTSS_ALPHAOP,
                    pp.tsp.IgnoreTexA ? D3DTOP_SELECTARG2 : D3DTOP_SELECTARG1);
            }
        }
        else
        {
            dev->SetTexture(0, NULL);
            dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }

        // Specular offset add (only when the poly requests it)
        dev->SetRenderState(D3DRS_SPECULARENABLE, pp.pcw.Offset ? TRUE : FALSE);

        // Blend — enabled for ALL lists; opaque polys carry SRC=ONE/DST=ZERO (no-op)
        dev->SetRenderState(D3DRS_SRCBLEND,  k_src[pp.tsp.SrcInstr & 7]);
        dev->SetRenderState(D3DRS_DESTBLEND, k_dst[pp.tsp.DstInstr & 7]);
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

        // Upload vertices
        TLVert* vp = nullptr;
        if (FAILED(vb->Lock(0, 0, (BYTE**)&vp, 0)) || !vp) return false;
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
        u16* ip = nullptr;
        if (FAILED(ib->Lock(0, 0, (BYTE**)&ip, 0)) || !ip) return false;
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
