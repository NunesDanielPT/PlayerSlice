#include "render/OutputWindows.h"
#include "render/Compositor.h"
#include "core/Log.h"
#include <set>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ps {

static const wchar_t* kOutClass = L"PLAYERSLICE_OUTPUT";

LRESULT CALLBACK OutputWindows::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (GetWindowLongW(h, GWL_STYLE) & WS_POPUP)) { SetCursor(nullptr); return TRUE; }
        break;
    case WM_SIZE: {
        auto* flag = reinterpret_cast<bool*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (flag) *flag = true;
        return 0;
    }
    case WM_CLOSE:
        // fechar uma saída a meio do espetáculo só pela interface principal
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void registerClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = OutputWindows::wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kOutClass;
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), L"IDI_APP");
    RegisterClassExW(&wc);
    done = true;
}

bool OutputWindows::create(Win& w, const Screen& s, const DisplayInfo* d) {
    registerClass();
    std::wstring title = L"PLAYERSLICE — " + fromUtf8(s.name);
    DWORD style, ex;
    RECT r;
    if (s.device == ScreenDevice::Monitor && d) {
        style = WS_POPUP | WS_VISIBLE;
        ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
        r = d->rect;
    } else {
        style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        ex = WS_EX_NOACTIVATE;
        int ww = std::min(s.width, 1280), hh = std::max(90, ww * s.height / std::max(1, s.width));
        r = {80, 80, 80 + ww, 80 + hh};
        AdjustWindowRectEx(&r, style, FALSE, ex);
    }
    w.hwnd = CreateWindowExW(ex, kOutClass, title.c_str(), style, r.left, r.top, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!w.hwnd) { PS_ERROR("Falha a criar janela de saída"); return false; }
    SetWindowLongPtrW(w.hwnd, GWLP_USERDATA, (LONG_PTR)&w.needResize);

    RECT cr; GetClientRect(w.hwnd, &cr);
    w.w = std::max(1L, cr.right - cr.left);
    w.h = std::max(1L, cr.bottom - cr.top);

    Gfx& G = gfx();
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w.w; sd.Height = w.h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    HRESULT hr = G.factory()->CreateSwapChainForHwnd(G.dev(), w.hwnd, &sd, nullptr, nullptr, &w.swap);
    if (FAILED(hr)) { PS_ERROR("Falha a criar swapchain da saída (0x%08lX)", hr); DestroyWindow(w.hwnd); w.hwnd = nullptr; return false; }
    G.factory()->MakeWindowAssociation(w.hwnd, DXGI_MWA_NO_ALT_ENTER);
    ComPtr<ID3D11Texture2D> bb;
    w.swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    G.dev()->CreateRenderTargetView(bb.Get(), nullptr, &w.rtv);
    w.device = s.device;
    w.monitorKey = s.monitorKey;
    w.monitor = d ? d->monitor : nullptr;
    w.rect = d ? d->rect : RECT{};
    PS_INFO("Saída '%s' aberta %dx%d %s", s.name.c_str(), w.w, w.h, d ? d->key.c_str() : "(janela)");
    return true;
}

void OutputWindows::destroy(Win& w) {
    w.rtv.Reset();
    w.swap.Reset();
    if (w.hwnd) DestroyWindow(w.hwnd);
    w.hwnd = nullptr;
}

bool OutputWindows::resize(Win& w) {
    w.needResize = false;
    RECT cr; GetClientRect(w.hwnd, &cr);
    int nw = std::max(1L, cr.right - cr.left), nh = std::max(1L, cr.bottom - cr.top);
    if (nw == w.w && nh == w.h) return true;
    w.rtv.Reset();
    gfx().ctx()->OMSetRenderTargets(0, nullptr, nullptr);
    if (FAILED(w.swap->ResizeBuffers(0, nw, nh, DXGI_FORMAT_UNKNOWN, 0))) return false;
    ComPtr<ID3D11Texture2D> bb;
    w.swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    gfx().dev()->CreateRenderTargetView(bb.Get(), nullptr, &w.rtv);
    w.w = nw; w.h = nh;
    return true;
}

void OutputWindows::sync(const Project& p, const std::vector<DisplayInfo>& displays, const LiveFn& live, bool offBlack) {
    std::set<uint32_t> keep;
    for (const Screen& s : p.screens) {
        if (s.device != ScreenDevice::Monitor && s.device != ScreenDevice::Window) continue;   // NDI e virtual: sem janela
        if (!live(s.id) && !offBlack) continue;
        const DisplayInfo* d = nullptr;
        if (s.device == ScreenDevice::Monitor) {
            for (auto& di : displays) if (di.key == s.monitorKey) { d = &di; break; }
            if (!d) continue; // monitor desligado → não abrir (fica "Sem sinal" na interface)
        }
        keep.insert(s.id);
        auto it = m_wins.find(s.id);
        if (it != m_wins.end()) {
            Win& w = it->second;
            bool changed = w.device != s.device || w.monitorKey != s.monitorKey ||
                           (d && (memcmp(&w.rect, &d->rect, sizeof(RECT)) != 0));
            if (!changed) continue;
            destroy(w);
            m_wins.erase(it);
        }
        Win& w = m_wins[s.id];
        if (!create(w, s, d)) m_wins.erase(s.id);
    }
    for (auto it = m_wins.begin(); it != m_wins.end();) {
        if (!keep.count(it->first)) { destroy(it->second); it = m_wins.erase(it); }
        else ++it;
    }
}

std::vector<RECT> OutputWindows::monitorRects() const {
    std::vector<RECT> r;
    for (auto& [id, w] : m_wins) if (w.device == ScreenDevice::Monitor && w.hwnd) r.push_back(w.rect);
    return r;
}

bool OutputWindows::coversMonitor(HMONITOR m) const {
    for (auto& [id, w] : m_wins) if (w.device == ScreenDevice::Monitor && w.monitor == m) return true;
    return false;
}

void OutputWindows::present(const Compositor& comp, const LiveFn& live, bool& didVsync) {
    Gfx& G = gfx();
    bool first = true;
    // garantir que nada fica à frente das saídas (notificações, outras janelas "sempre visíveis")
    static DWORD lastTop = 0;
    DWORD nowTick = GetTickCount();
    bool reassert = nowTick - lastTop > 1000;
    if (reassert) lastTop = nowTick;
    if (m_wins.empty()) m_outFps = 0, m_lastPresent = 0;
    else {
        LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
        double t = (double)c.QuadPart / f.QuadPart;
        if (m_lastPresent > 0) {
            double dt = t - m_lastPresent;
            if (dt > 0 && dt < 0.5) m_outFps = m_outFps <= 0 ? 1.0 / dt : m_outFps * 0.95 + (1.0 / dt) * 0.05;
        }
        m_lastPresent = t;
    }
    for (auto& [id, w] : m_wins) {
        if (reassert && w.device == ScreenDevice::Monitor && w.hwnd)
            SetWindowPos(w.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (w.needResize) resize(w);
        if (!w.rtv) continue;
        const RenderTarget* src = live(id) ? comp.screenRT(id) : nullptr; // desligada → preto
        G.setTarget(w.rtv.Get(), w.w, w.h);
        G.clear(w.rtv.Get(), 0, 0, 0, 1);
        if (src) {
            // ajustar mantendo proporção (em modo monitor normalmente é 1:1)
            float s = std::min((float)w.w / src->w, (float)w.h / src->h);
            float dw = src->w * s, dh = src->h * s;
            QuadParams q;
            Gfx::rectToNdc((w.w - dw) * 0.5f, (w.h - dh) * 0.5f, dw, dh, w.w, w.h, q.pos);
            q.srv0 = src->srv.Get();
            q.blend = Blend::Opaque;
            q.pointSample = std::fabs(s - 1.f) < 0.0001f;
            G.drawQuad(q);
        }
        // a primeira saída em monitor marca o ritmo (vsync); as outras não bloqueiam
        UINT sync = (first && w.device == ScreenDevice::Monitor) ? 1 : 0;
        HRESULT hr = w.swap->Present(sync, 0);
        if (sync) { didVsync = true; first = false; }
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            PS_ERROR("GPU removida/reiniciada (0x%08lX)", (unsigned long)G.dev()->GetDeviceRemovedReason());
    }
}

void OutputWindows::closeAll() {
    for (auto& [id, w] : m_wins) destroy(w);
    m_wins.clear();
}

} // namespace ps
