#include "gfx/Gfx.h"
#include "core/Log.h"
#include <d3dcompiler.h>
#include <cstring>
#include <cmath>

namespace ps {

static Gfx g_gfx;
Gfx& gfx() { return g_gfx; }

// ---------------------------------------------------------------------------
// HLSL — compilado em runtime (d3dcompiler_47.dll vem com o Windows 10/11)
// ---------------------------------------------------------------------------
static const char* kHlsl = R"HLSL(
cbuffer CB : register(b0) {
    float4 gPos[4];
    float4 gUv[4];
    float4 gColor;
    float4 gParams;   // x opacidade, y brilho, z aspect, w livre
    float4 gYuvR;
    float4 gYuvG;
    float4 gYuvB;
    float4 gHo0, gHo1, gHo2;   // inversa da homografia da saída (píxel → quadrado unitário)
    float4 gHi0, gHi1, gHi2;   // homografia da entrada (quadrado unitário → UV)
    float4 gAdjust;            // rgb = ganho (-1..1), w = contraste (-1..1)
};
Texture2D    t0 : register(t0);
Texture2D    t1 : register(t1);
SamplerState s0 : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 q : TEXCOORD1; };

VSOut VS(uint id : SV_VertexID) {
    VSOut o;
    o.pos = float4(gPos[id].xy, 0, 1);
    o.uv  = gUv[id].xy;
    o.q   = float2(id & 1, id >> 1); // coordenadas locais 0..1 do quad
    return o;
}

float4 finish(float3 rgb, float a) {
    rgb = saturate(rgb + gParams.y);
    return float4(rgb, a * gParams.x);
}

float4 PS_RGBA(VSOut i) : SV_Target {
    float4 c = t0.Sample(s0, i.uv);
    return finish(c.rgb, c.a);
}

float4 PS_YUV(VSOut i) : SV_Target {
    float  y  = t0.Sample(s0, i.uv).r;
    float2 uv = t1.Sample(s0, i.uv).rg;
    float3 yuv = float3(y, uv);
    float3 rgb = float3(dot(gYuvR.xyz, yuv) + gYuvR.w,
                        dot(gYuvG.xyz, yuv) + gYuvG.w,
                        dot(gYuvB.xyz, yuv) + gYuvB.w);
    return finish(rgb, 1);
}

float3 ycocg(float4 c) {
    float scale = (c.z * (255.0 / 8.0)) + 1.0;
    float Co = (c.x - (0.5 * 256.0 / 255.0)) / scale;
    float Cg = (c.y - (0.5 * 256.0 / 255.0)) / scale;
    float Y  = c.w;
    return float3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
}

float4 PS_YCoCg(VSOut i) : SV_Target {
    return finish(ycocg(t0.Sample(s0, i.uv)), 1);
}

float4 PS_YCoCgAlpha(VSOut i) : SV_Target {
    return finish(ycocg(t0.Sample(s0, i.uv)), t1.Sample(s0, i.uv).r);
}

// Test pattern: xadrez na cor da slice, círculo inscrito, diagonais e moldura
float4 PS_Test(VSOut i) : SV_Target {
    float2 q = i.q;
    float aspect = max(gParams.z, 0.0001);
    float2 p = float2(q.x * aspect, q.y);           // espaço com proporção real
    float cells = 6.0;
    float cell = 1.0 / cells;
    int cx = (int)floor(p.x / cell), cy = (int)floor(p.y / cell);
    float chk = ((cx + cy) & 1) ? 0.78 : 1.0;
    float3 col = gColor.rgb * chk;

    float2 fw = fwidth(p) * 1.5;
    // círculo
    float2 c = float2(aspect * 0.5, 0.5);
    float r = min(aspect, 1.0) * 0.48;
    float d = abs(length(p - c) - r);
    float line_ = 1.0 - smoothstep(0.0, max(fw.x, fw.y), d);
    // diagonais
    float d1 = abs(q.y - q.x) * 1.0;
    float d2 = abs(q.y - (1.0 - q.x));
    float2 fq = fwidth(q) * 1.5;
    line_ = max(line_, 1.0 - smoothstep(0.0, max(fq.x, fq.y), min(d1, d2)));
    // moldura
    float2 e = min(q, 1.0 - q) / max(fq, 1e-5);
    line_ = max(line_, 1.0 - smoothstep(1.0, 2.0, min(e.x, e.y)));
    col = lerp(col, float3(1, 1, 1), line_ * 0.85);
    return finish(col, 1);
}

// coordenadas (s,t) dentro do quadrilátero de saída, com perspetiva correta
float2 warpST(float2 px) {
    float3 p = float3(px, 1);
    float3 st = float3(dot(gHo0.xyz, p), dot(gHo1.xyz, p), dot(gHo2.xyz, p));
    return st.xy / st.z;
}

float4 PS_Warp(VSOut i) : SV_Target {
    float2 st = warpST(i.pos.xy);
    float3 s3 = float3(st, 1);
    float3 uv = float3(dot(gHi0.xyz, s3), dot(gHi1.xyz, s3), dot(gHi2.xyz, s3));
    float4 c = t0.Sample(s0, uv.xy / uv.z);
    float3 rgb = (c.rgb - 0.5) * (1 + gAdjust.w) + 0.5;
    rgb *= (1 + gAdjust.rgb);
    return finish(rgb, c.a);
}

float4 PS_WarpTest(VSOut i) : SV_Target {
    i.q = saturate(warpST(i.pos.xy));
    return PS_Test(i);
}

float4 PS_Solid(VSOut i) : SV_Target {
    return finish(gColor.rgb, gColor.a);
}
)HLSL";

struct CBData {
    float pos[4][4];
    float uv[4][4];
    float color[4];
    float params[4];
    float yr[4], yg[4], yb[4];
    float ho[3][4];
    float hi[3][4];
    float adjust[4];
};

YuvMatrix makeYuvMatrix(bool bt709, bool fullRange, bool) {
    // coeficientes R = Y + a*Cr ; G = Y - b*Cb - c*Cr ; B = Y + d*Cb
    float a, b, c, d;
    if (bt709) { a = 1.5748f; b = 0.1873f; c = 0.4681f; d = 1.8556f; }
    else       { a = 1.4020f; b = 0.3441f; c = 0.7141f; d = 1.7720f; }
    float ys, yo, cs;
    if (fullRange) { ys = 1.f; yo = 0.f; cs = 1.f; }
    else           { ys = 255.f / 219.f; yo = -16.f / 255.f * ys; cs = 255.f / 224.f; }
    const float co = -128.f / 255.f; // offset do croma (antes da escala)
    YuvMatrix m;
    // R = ys*Y + yo + a*cs*(V+co)
    m.r[0] = ys; m.r[1] = 0;          m.r[2] = a * cs;  m.r[3] = yo + a * cs * co;
    m.g[0] = ys; m.g[1] = -b * cs;    m.g[2] = -c * cs; m.g[3] = yo - b * cs * co - c * cs * co;
    m.b[0] = ys; m.b[1] = d * cs;     m.b[2] = 0;       m.b[3] = yo + d * cs * co;
    return m;
}

static ComPtr<ID3DBlob> compile(const char* entry, const char* target, std::string* err) {
    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3DCompile(kHlsl, strlen(kHlsl), "playerslice.hlsl", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr)) {
        if (err) *err = std::string("Shader ") + entry + ": " + (errors ? (const char*)errors->GetBufferPointer() : "erro");
        return nullptr;
    }
    return blob;
}

bool Gfx::compileShaders(std::string* err) {
    auto vs = compile("VS", "vs_5_0", err);
    if (!vs) return false;
    if (FAILED(m_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs))) return false;
    const char* names[] = {"PS_RGBA", "PS_YUV", "PS_YCoCg", "PS_YCoCgAlpha", "PS_Test", "PS_Solid", "PS_Warp", "PS_WarpTest"};
    for (int i = 0; i < (int)PSKind::Count; ++i) {
        auto ps = compile(names[i], "ps_5_0", err);
        if (!ps) return false;
        if (FAILED(m_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps[i]))) return false;
    }
    return true;
}

bool Gfx::init(const std::string& preferredGpu, std::string* err) {
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
    if (FAILED(hr)) { if (err) *err = "CreateDXGIFactory2 falhou"; return false; }

    // 1) placa escolhida nas Definições
    if (!preferredGpu.empty()) {
        ComPtr<IDXGIFactory1> f1; m_factory.As(&f1);
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; f1 && f1->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
            if (toUtf8(d.Description) == preferredGpu) { m_adapter = a; break; }
        }
        if (!m_adapter) PS_WARN("Placa '%s' não encontrada — a usar a mais potente", preferredGpu.c_str());
    }
    // 2) senão, a gráfica de alto desempenho (a NVIDIA em vez da integrada)
    ComPtr<IDXGIFactory6> f6;
    if (!m_adapter && SUCCEEDED(m_factory.As(&f6))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&m_adapter));
    }
    if (!m_adapter) {
        ComPtr<IDXGIFactory1> f1; m_factory.As(&f1);
        ComPtr<IDXGIAdapter1> a; size_t best = 0;
        for (UINT i = 0; f1->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (d.DedicatedVideoMemory >= best) { best = d.DedicatedVideoMemory; m_adapter = a; }
        }
    }
    if (m_adapter) {
        DXGI_ADAPTER_DESC1 d; m_adapter->GetDesc1(&d);
        m_adapterName = d.Description;
        m_vramMB = d.DedicatedVideoMemory / (1024 * 1024);
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef PS_D3D_DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got;
    hr = D3D11CreateDevice(m_adapter.Get(), m_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           flags, levels, 2, D3D11_SDK_VERSION, &m_dev, &got, &m_ctx);
    if (FAILED(hr)) {
        // sem suporte de vídeo (ex. drivers antigos): tentar sem a flag
        flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        hr = D3D11CreateDevice(m_adapter.Get(), m_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               flags, levels, 2, D3D11_SDK_VERSION, &m_dev, &got, &m_ctx);
    }
    if (FAILED(hr)) { if (err) *err = "Não foi possível criar o dispositivo Direct3D 11"; return false; }

    // O descodificador por hardware usa o mesmo dispositivo noutra thread
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(m_factory.As(&f5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) m_tearing = allow;
    }

    if (!compileShaders(err)) return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CBData);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_dev->CreateBuffer(&bd, nullptr, &m_cb);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    m_dev->CreateSamplerState(&sd, &m_sampLinear);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    m_dev->CreateSamplerState(&sd, &m_sampPoint);

    auto mkBlend = [&](Blend b, D3D11_BLEND src, D3D11_BLEND dst, bool enable) {
        D3D11_BLEND_DESC d{};
        auto& rt = d.RenderTarget[0];
        rt.BlendEnable = enable;
        rt.SrcBlend = src; rt.DestBlend = dst; rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        m_dev->CreateBlendState(&d, &m_blend[(int)b]);
    };
    mkBlend(Blend::Alpha,    D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, true);
    mkBlend(Blend::Add,      D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, true);
    mkBlend(Blend::Multiply, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_SRC_ALPHA, true);
    mkBlend(Blend::Screen,   D3D11_BLEND_INV_DEST_COLOR, D3D11_BLEND_ONE, true);
    mkBlend(Blend::Opaque,   D3D11_BLEND_ONE, D3D11_BLEND_ZERO, false);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    m_dev->CreateRasterizerState(&rd, &m_rsNormal);
    rd.ScissorEnable = TRUE;
    m_dev->CreateRasterizerState(&rd, &m_rsScissor);

    PS_INFO("GPU: %s (%zu MB VRAM), feature level %x, tearing=%d", toUtf8(m_adapterName).c_str(), m_vramMB, (unsigned)got, (int)m_tearing);
    return true;
}

void Gfx::shutdown() {
    if (m_ctx) { m_ctx->ClearState(); m_ctx->Flush(); }
    for (auto& p : m_ps) p.Reset();
    for (auto& b : m_blend) b.Reset();
    m_vs.Reset(); m_cb.Reset(); m_sampLinear.Reset(); m_sampPoint.Reset();
    m_rsNormal.Reset(); m_rsScissor.Reset();
    m_ctx.Reset(); m_dev.Reset(); m_adapter.Reset(); m_factory.Reset();
}

bool Gfx::createRT(RenderTarget& rt, int w, int h, DXGI_FORMAT fmt) {
    rt = {};
    if (w <= 0 || h <= 0) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &rt.tex))) { PS_ERROR("Falha a criar render target %dx%d", w, h); return false; }
    m_dev->CreateRenderTargetView(rt.tex.Get(), nullptr, &rt.rtv);
    m_dev->CreateShaderResourceView(rt.tex.Get(), nullptr, &rt.srv);
    rt.w = w; rt.h = h;
    return true;
}

static DXGI_FORMAT planeFormat(DXGI_FORMAT fmt, int plane) {
    if (fmt == DXGI_FORMAT_NV12) return plane == 0 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8_UNORM;
    if (fmt == DXGI_FORMAT_P010 || fmt == DXGI_FORMAT_P016) return plane == 0 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R16G16_UNORM;
    return fmt;
}

bool Gfx::ensureVideoTexture(VideoTexture& vt, int w, int h, DXGI_FORMAT fmt, DXGI_FORMAT fmt2, bool dynamic) {
    if (vt.valid() && vt.w == w && vt.h == h && vt.fmt == fmt && vt.fmt2 == fmt2) return true;
    vt = {};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    td.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &vt.tex[0]))) {
        PS_ERROR("Falha a criar textura de vídeo %dx%d fmt=%d", w, h, (int)fmt);
        return false;
    }
    bool biplanar = fmt == DXGI_FORMAT_NV12 || fmt == DXGI_FORMAT_P010 || fmt == DXGI_FORMAT_P016;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    sd.Format = planeFormat(fmt, 0);
    if (FAILED(m_dev->CreateShaderResourceView(vt.tex[0].Get(), &sd, &vt.srv[0]))) { vt = {}; return false; }
    if (biplanar) {
        sd.Format = planeFormat(fmt, 1);
        if (FAILED(m_dev->CreateShaderResourceView(vt.tex[0].Get(), &sd, &vt.srv[1]))) { vt = {}; return false; }
    } else if (fmt2 != DXGI_FORMAT_UNKNOWN) {
        td.Format = fmt2;
        if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &vt.tex[1]))) { vt = {}; return false; }
        m_dev->CreateShaderResourceView(vt.tex[1].Get(), nullptr, &vt.srv[1]);
    }
    vt.fmt = fmt; vt.fmt2 = fmt2; vt.w = w; vt.h = h;
    return true;
}

void Gfx::setTarget(ID3D11RenderTargetView* rtv, int w, int h) {
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
    m_ctx->RSSetViewports(1, &vp);
    clearScissor();
}

void Gfx::clear(ID3D11RenderTargetView* rtv, float r, float g, float b, float a) {
    float c[4] = {r, g, b, a};
    m_ctx->ClearRenderTargetView(rtv, c);
}

void Gfx::setScissor(int x, int y, int w, int h) {
    D3D11_RECT r{x, y, x + w, y + h};
    m_ctx->RSSetScissorRects(1, &r);
    m_scissor = true;
}

void Gfx::clearScissor() { m_scissor = false; }

void Gfx::rectToNdc(float x, float y, float w, float h, int tw, int th, float out[4][2]) {
    float x0 = x / tw * 2.f - 1.f, x1 = (x + w) / tw * 2.f - 1.f;
    float y0 = 1.f - y / th * 2.f, y1 = 1.f - (y + h) / th * 2.f;
    out[0][0] = x0; out[0][1] = y0;
    out[1][0] = x1; out[1][1] = y0;
    out[2][0] = x0; out[2][1] = y1;
    out[3][0] = x1; out[3][1] = y1;
}

void Gfx::drawQuad(const QuadParams& q) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(m_ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    CBData* cb = (CBData*)m.pData;
    memset(cb, 0, sizeof(CBData));
    for (int i = 0; i < 4; ++i) {
        cb->pos[i][0] = q.pos[i][0]; cb->pos[i][1] = q.pos[i][1];
        cb->uv[i][0] = q.uv[i][0];   cb->uv[i][1] = q.uv[i][1];
    }
    memcpy(cb->color, q.color, sizeof(cb->color));
    cb->params[0] = q.opacity; cb->params[1] = q.brightness; cb->params[2] = q.aspect;
    if (q.yuv) { memcpy(cb->yr, q.yuv->r, 16); memcpy(cb->yg, q.yuv->g, 16); memcpy(cb->yb, q.yuv->b, 16); }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) { cb->ho[r][c] = q.hOutInv[r * 3 + c]; cb->hi[r][c] = q.hIn[r * 3 + c]; }
    memcpy(cb->adjust, q.adjust, sizeof(cb->adjust));
    m_ctx->Unmap(m_cb.Get(), 0);

    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->VSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
    m_ctx->PSSetShader(m_ps[(int)q.ps].Get(), nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
    ID3D11ShaderResourceView* srvs[2] = {q.srv0, q.srv1};
    m_ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* s = q.pointSample ? m_sampPoint.Get() : m_sampLinear.Get();
    m_ctx->PSSetSamplers(0, 1, &s);
    float bf[4] = {0, 0, 0, 0};
    m_ctx->OMSetBlendState(m_blend[(int)q.blend].Get(), bf, 0xFFFFFFFF);
    m_ctx->RSSetState(m_scissor ? m_rsScissor.Get() : m_rsNormal.Get());
    m_ctx->Draw(4, 0);
    ID3D11ShaderResourceView* nulls[2] = {nullptr, nullptr};
    m_ctx->PSSetShaderResources(0, 2, nulls);
}

} // namespace ps
