#include "render/NdiOutput.h"
#include "render/Compositor.h"
#include "gfx/Gfx.h"
#include "core/Log.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <set>

namespace ps {

// --- ABI mínima do NDI SDK (v5/v6), carregada dinamicamente -------------------------------
namespace {
struct NdiSendCreate {
    const char* p_ndi_name;
    const char* p_groups;
    bool clock_video;
    bool clock_audio;
};
struct NdiVideoFrameV2 {
    int xres, yres;
    uint32_t FourCC;
    int frame_rate_N, frame_rate_D;
    float picture_aspect_ratio;
    int frame_format_type;           // 1 = progressivo
    int64_t timecode;
    uint8_t* p_data;
    int line_stride_in_bytes;
    const char* p_metadata;
    int64_t timestamp;
};
constexpr uint32_t fourcc(char a, char b, char c, char d) { return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24); }
constexpr int64_t kSynthesizeTimecode = INT64_MAX;

using FnInit = bool (*)();
using FnDestroy = void (*)();
using FnSendCreate = void* (*)(const NdiSendCreate*);
using FnSendDestroy = void (*)(void*);
using FnSendVideoAsync = void (*)(void*, const NdiVideoFrameV2*);
FnInit pInit = nullptr;
FnDestroy pDestroy = nullptr;
FnSendCreate pSendCreate = nullptr;
FnSendDestroy pSendDestroy = nullptr;
FnSendVideoAsync pSendVideoAsync = nullptr;
} // namespace

NdiOutputs& NdiOutputs::get() { static NdiOutputs n; return n; }

bool NdiOutputs::load() {
    if (m_tried) return m_ok;
    m_tried = true;
    const wchar_t* envs[] = {L"NDI_RUNTIME_DIR_V6", L"NDI_RUNTIME_DIR_V5", L"NDI_RUNTIME_DIR_V4"};
    HMODULE h = nullptr;
    for (auto* e : envs) {
        wchar_t dir[MAX_PATH] = L"";
        DWORD n = GetEnvironmentVariableW(e, dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) continue;
        std::wstring path = std::wstring(dir) + L"\\Processing.NDI.Lib.x64.dll";
        h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (h) break;
    }
    if (!h) h = LoadLibraryW(L"Processing.NDI.Lib.x64.dll");
    if (!h) { m_err = "NDI Runtime não instalado"; PS_WARN("NDI: runtime não encontrado (instala o NDI Runtime ou o NDI Tools)"); return false; }
    pInit = (FnInit)GetProcAddress(h, "NDIlib_initialize");
    pDestroy = (FnDestroy)GetProcAddress(h, "NDIlib_destroy");
    pSendCreate = (FnSendCreate)GetProcAddress(h, "NDIlib_send_create");
    pSendDestroy = (FnSendDestroy)GetProcAddress(h, "NDIlib_send_destroy");
    pSendVideoAsync = (FnSendVideoAsync)GetProcAddress(h, "NDIlib_send_send_video_async_v2");
    if (!pInit || !pSendCreate || !pSendDestroy || !pSendVideoAsync) { m_err = "Versão do NDI Runtime não suportada"; FreeLibrary(h); return false; }
    if (!pInit()) { m_err = "O processador não é suportado pelo NDI"; FreeLibrary(h); return false; }
    m_dll = h;
    m_ok = true;
    PS_INFO("NDI: runtime carregado");
    return true;
}

bool NdiOutputs::runtimeAvailable() { return load(); }

void NdiOutputs::destroy(Sender& s) {
    if (s.inst) {
        pSendVideoAsync(s.inst, nullptr);   // liberta o último buffer
        pSendDestroy(s.inst);
        s.inst = nullptr;
    }
    for (auto& t : s.staging) t.Reset();
}

void NdiOutputs::present(const Project& p, const Compositor& comp, const LiveFn& live, float fps) {
    std::set<uint32_t> want;
    for (auto& s : p.screens) if (s.device == ScreenDevice::NDI) want.insert(s.id);
    // ecrãs que deixaram de ser NDI
    for (auto it = m_send.begin(); it != m_send.end();) {
        if (!want.count(it->first)) { destroy(it->second); it = m_send.erase(it); }
        else ++it;
    }
    if (want.empty() || !load()) return;

    Gfx& G = gfx();
    if (fps <= 0) fps = 50;
    for (auto& s : p.screens) {
        if (s.device != ScreenDevice::NDI) continue;
        const RenderTarget* rt = comp.screenRT(s.id);
        if (!rt || !rt->valid()) continue;
        Sender& snd = m_send[s.id];
        std::string name = s.ndiName.empty() ? s.name : s.ndiName;
        if (snd.inst && (snd.name != name || snd.w != rt->w || snd.h != rt->h)) destroy(snd);
        if (!snd.inst) {
            NdiSendCreate c{name.c_str(), nullptr, false, false};
            snd.inst = pSendCreate(&c);
            if (!snd.inst) { PS_ERROR("NDI: não foi possível criar a fonte '%s'", name.c_str()); continue; }
            snd.name = name; snd.w = rt->w; snd.h = rt->h; snd.frame = 0;
            D3D11_TEXTURE2D_DESC td{};
            rt->tex->GetDesc(&td);
            td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
            bool ok = true;
            for (auto& t : snd.staging) if (FAILED(G.dev()->CreateTexture2D(&td, nullptr, &t))) ok = false;
            if (!ok) { destroy(snd); PS_ERROR("NDI: sem memória para '%s'", name.c_str()); continue; }
            for (auto& b : snd.buf) b.assign((size_t)snd.w * snd.h * 4, 0);
            PS_INFO("NDI: fonte '%s' %dx%d", name.c_str(), snd.w, snd.h);
        }
        std::vector<uint8_t>& out = snd.buf[snd.bufIdx];
        bool have = false;
        if (live(s.id)) {
            // cópia para staging agora; leitura do frame de há 2 (sem parar a GPU)
            std::lock_guard<std::recursive_mutex> lk(G.ctxMutex());
            G.ctx()->CopyResource(snd.staging[snd.frame % 3].Get(), rt->tex.Get());
            if (snd.frame >= 2) {
                ID3D11Texture2D* rd = snd.staging[(snd.frame - 2) % 3].Get();
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(G.ctx()->Map(rd, 0, D3D11_MAP_READ, 0, &m))) {
                    const size_t row = (size_t)snd.w * 4;
                    for (int y = 0; y < snd.h; ++y) memcpy(out.data() + y * row, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, row);
                    G.ctx()->Unmap(rd, 0);
                    have = true;
                }
            }
            snd.frame++;
        } else {
            std::fill(out.begin(), out.end(), 0);   // saída desligada → preto
            snd.frame = 0;
            have = true;
        }
        if (!have) continue;
        NdiVideoFrameV2 f{};
        f.xres = snd.w; f.yres = snd.h;
        f.FourCC = fourcc('R', 'G', 'B', 'X');
        f.frame_rate_N = (int)std::lround(fps * 1000); f.frame_rate_D = 1000;
        f.picture_aspect_ratio = (float)snd.w / (float)snd.h;
        f.frame_format_type = 1;
        f.timecode = kSynthesizeTimecode;
        f.p_data = out.data();
        f.line_stride_in_bytes = snd.w * 4;
        pSendVideoAsync(snd.inst, &f);
        snd.bufIdx ^= 1;   // o NDI usa este buffer até ao próximo envio
    }
}

void NdiOutputs::shutdown() {
    for (auto& [id, s] : m_send) destroy(s);
    m_send.clear();
    if (m_ok && pDestroy) pDestroy();
    if (m_dll) FreeLibrary((HMODULE)m_dll);
    m_dll = nullptr; m_ok = false; m_tried = false;
}

} // namespace ps
