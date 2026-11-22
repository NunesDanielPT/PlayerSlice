#pragma once
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <mutex>
#include <string>

namespace ps {

using Microsoft::WRL::ComPtr;

struct RenderTarget {
    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11RenderTargetView>   rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
    bool valid() const { return tex != nullptr; }
};

// Textura de vídeo: até 2 planos (Y + UV no NV12/P010, cor + alfa no HAP Q Alpha)
struct VideoTexture {
    ComPtr<ID3D11Texture2D>          tex[2];
    ComPtr<ID3D11ShaderResourceView> srv[2];
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT fmt2 = DXGI_FORMAT_UNKNOWN;
    int w = 0, h = 0;       // tamanho alocado
    bool valid() const { return tex[0] != nullptr; }
};

enum class PSKind : int {
    RGBA = 0,    // BGRA/RGBA normal, BC1/BC3/BC7 (HAP, HAP Alpha, HAP R)
    YUV,         // NV12 / P010 (descodificação por hardware)
    YCoCg,       // HAP Q
    YCoCgAlpha,  // HAP Q Alpha (2ª textura BC4 = alfa)
    Test,        // test pattern
    Solid,       // cor lisa
    Warp,        // slice com perspetiva (homografias em hOutInv / hIn)
    WarpTest,    // test pattern com perspetiva
    Count
};

enum class Blend : int { Alpha = 0, Add, Multiply, Screen, Opaque, Count };

struct YuvMatrix {
    float r[4], g[4], b[4]; // rgb = dot(row.xyz, yuv) + row.w
};
YuvMatrix makeYuvMatrix(bool bt709, bool fullRange, bool tenBitInHigh = false);

struct QuadParams {
    float pos[4][2];     // NDC: TL, TR, BL, BR
    float uv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    ID3D11ShaderResourceView* srv0 = nullptr;
    ID3D11ShaderResourceView* srv1 = nullptr;
    PSKind ps = PSKind::RGBA;
    Blend  blend = Blend::Alpha;
    float opacity = 1.f;
    float brightness = 0.f;
    float color[4] = {1, 1, 1, 1};
    float aspect = 1.f;  // largura/altura do quad (test pattern)
    const YuvMatrix* yuv = nullptr;
    bool pointSample = false;
    // warp: píxel do alvo → (s,t) [hOutInv]; (s,t) → UV da textura [hIn]
    float hOutInv[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float hIn[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float adjust[4] = {0, 0, 0, 0};   // ganho r,g,b e contraste
};

class Gfx {
public:
    // preferredGpu: descrição da placa (vazio = a mais potente)
    bool init(const std::string& preferredGpu, std::string* err);
    void shutdown();

    ID3D11Device*        dev() const { return m_dev.Get(); }
    ID3D11DeviceContext* ctx() const { return m_ctx.Get(); }
    IDXGIFactory2*       factory() const { return m_factory.Get(); }
    IDXGIAdapter1*       adapter() const { return m_adapter.Get(); }
    const std::wstring&  adapterName() const { return m_adapterName; }
    size_t               vramMB() const { return m_vramMB; }
    bool                 tearingSupported() const { return m_tearing; }

    // Mutex do contexto imediato: partilhado com o descodificador por hardware (FFmpeg)
    std::recursive_mutex& ctxMutex() { return m_ctxMutex; }

    bool createRT(RenderTarget& rt, int w, int h, DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM);
    bool ensureVideoTexture(VideoTexture& vt, int w, int h, DXGI_FORMAT fmt, DXGI_FORMAT fmt2 = DXGI_FORMAT_UNKNOWN, bool dynamic = false);

    void setTarget(ID3D11RenderTargetView* rtv, int w, int h);
    void clear(ID3D11RenderTargetView* rtv, float r, float g, float b, float a);
    void setScissor(int x, int y, int w, int h);
    void clearScissor();
    void drawQuad(const QuadParams& q);

    // helper: retângulo em píxeis do alvo → NDC
    static void rectToNdc(float x, float y, float w, float h, int tw, int th, float out[4][2]);

private:
    bool compileShaders(std::string* err);

    ComPtr<ID3D11Device>        m_dev;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<IDXGIFactory2>       m_factory;
    ComPtr<IDXGIAdapter1>       m_adapter;
    ComPtr<ID3D11VertexShader>  m_vs;
    ComPtr<ID3D11PixelShader>   m_ps[(int)PSKind::Count];
    ComPtr<ID3D11Buffer>        m_cb;
    ComPtr<ID3D11SamplerState>  m_sampLinear, m_sampPoint;
    ComPtr<ID3D11BlendState>    m_blend[(int)Blend::Count];
    ComPtr<ID3D11RasterizerState> m_rsNormal, m_rsScissor;
    std::wstring m_adapterName;
    size_t m_vramMB = 0;
    bool m_tearing = false;
    bool m_scissor = false;
    std::recursive_mutex m_ctxMutex;
};

Gfx& gfx();

} // namespace ps
