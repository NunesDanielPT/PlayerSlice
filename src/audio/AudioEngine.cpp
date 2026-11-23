#include "audio/AudioEngine.h"
#include "core/Log.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ps {

// ---------------------------------------------------------------------------
// AudioSource
// ---------------------------------------------------------------------------
void AudioSource::push(double t0, const float* stereo, int frames) {
    if (frames <= 0) return;
    Chunk c;
    c.t0 = t0;
    c.s.assign(stereo, stereo + frames * 2);
    std::lock_guard<std::mutex> lk(m_mtx);
    m_chunks.push_back(std::move(c));
    // limite de segurança (~10 s)
    size_t total = 0;
    for (auto& k : m_chunks) total += k.s.size() / 2;
    while (total > (size_t)m_rate * 10 && !m_chunks.empty()) { total -= m_chunks.front().s.size() / 2; m_chunks.pop_front(); }
}

void AudioSource::clear() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_chunks.clear();
}

void AudioSource::setClock(double ph, double at, double speed, bool running) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_ph = ph; m_phAt = at; m_speed = speed; m_running = running;
}

void AudioSource::setMix(float gain, float pan, uint32_t layerId, bool audible) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_gain = gain; m_pan = pan; m_layer = layerId; m_audible = audible;
}

int AudioSource::render(float* out, int frames, int rate, double nowSec) {
    // estado lido sob o mutex; a cópia dos blocos também (os blocos são pequenos)
    std::lock_guard<std::mutex> lk(m_mtx);
    float peakL = 0, peakR = 0;
    if (!m_running || !m_audible || std::fabs(m_speed - 1.0) > 0.01) {
        m_hasRead = false;
        m_vuL = m_vuL.load() * 0.85f; m_vuR = m_vuR.load() * 0.85f;
        return 0;
    }
    // tempo do clip do 1.º sample deste bloco, segundo o relógio do vídeo
    double target = m_ph + (nowSec - m_phAt) * m_speed;
    double t;
    if (!m_hasRead || std::fabs(m_readT - target) > 0.04) t = target;   // saltou → acompanha o vídeo
    else t = m_readT + (target - m_readT) * 0.002;                     // corrige devagar (sem cliques)
    // descartar blocos já passados
    while (!m_chunks.empty() && m_chunks.front().t0 + m_chunks.front().s.size() / 2.0 / rate < t - 0.25) m_chunks.pop_front();

    float gl = m_gain * std::min(1.f, 1.f - m_pan), gr = m_gain * std::min(1.f, 1.f + m_pan);
    float pl = m_hasRead ? m_lastGL : gl, pr = m_hasRead ? m_lastGR : gr;
    size_t ci = 0;
    long long k = -1;
    for (int i = 0; i < frames; ++i) {
        double ts = t + (double)i / rate;
        // encontrar o bloco / amostra para ts
        if (k < 0 || ci >= m_chunks.size() || k >= (long long)(m_chunks[ci].s.size() / 2)) {
            k = -1;
            while (ci < m_chunks.size()) {
                const Chunk& c = m_chunks[ci];
                long long idx = (long long)std::floor((ts - c.t0) * rate + 0.5);
                long long n = (long long)(c.s.size() / 2);
                if (idx < n) { if (idx >= 0) k = idx; break; }
                ++ci;
            }
            if (k < 0) continue;   // ainda não há som para este instante (silêncio)
        }
        float a = (float)i / frames;
        float g0 = pl + (gl - pl) * a, g1 = pr + (gr - pr) * a;
        float L = m_chunks[ci].s[k * 2] * g0, R = m_chunks[ci].s[k * 2 + 1] * g1;
        out[i * 2] += L; out[i * 2 + 1] += R;
        peakL = std::max(peakL, std::fabs(L)); peakR = std::max(peakR, std::fabs(R));
        ++k;
    }
    m_lastGL = gl; m_lastGR = gr;
    m_readT = t + (double)frames / rate;
    m_hasRead = true;
    m_vuL = std::max(peakL, m_vuL.load() * 0.85f);
    m_vuR = std::max(peakR, m_vuR.load() * 0.85f);
    return frames;
}

// ---------------------------------------------------------------------------
// AudioEngine
// ---------------------------------------------------------------------------
AudioEngine& AudioEngine::get() { static AudioEngine e; return e; }

std::shared_ptr<AudioSource> AudioEngine::createSource() {
    auto s = std::make_shared<AudioSource>();
    s->m_rate = m_rate.load();
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sources.push_back(s);
    return s;
}

VuLevel AudioEngine::layerVu(uint32_t id) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_layerVu.find(id);
    return it == m_layerVu.end() ? VuLevel{} : it->second;
}

std::string AudioEngine::deviceName() const { std::lock_guard<std::mutex> lk(m_mtx); return m_name; }
std::string AudioEngine::error() const { std::lock_guard<std::mutex> lk(m_mtx); return m_err; }

void AudioEngine::start(const std::string& id) {
    shutdown();
    m_stop = false;
    m_thread = std::thread(&AudioEngine::thread, this, id);
    // esperar um pouco para saber a taxa da placa (os players novos usam-na)
    for (int i = 0; i < 50 && !m_ok.load(); ++i) Sleep(10);
}

void AudioEngine::shutdown() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    m_ok = false;
}

void AudioEngine::thread(std::string devId) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // {a45c254e-df1c-4efd-8020-67d146a850e0}, 14 = PKEY_Device_FriendlyName
    static const PROPERTYKEY kFriendly = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto setErr = [&](const char* e) { { std::lock_guard<std::mutex> lk(m_mtx); m_err = e; } PS_WARN("Áudio: %s", e); };
    while (!m_stop.load()) {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioClient> ac;
        ComPtr<IAudioRenderClient> rc;
        WAVEFORMATEX* wf = nullptr;
        bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)));
        if (ok && !devId.empty()) en->GetDevice(fromUtf8(devId).c_str(), &dev);
        if (ok && !dev) ok = SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev));
        if (ok) {
            ComPtr<IPropertyStore> ps;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(kFriendly, &v)) && v.vt == VT_LPWSTR) { std::lock_guard<std::mutex> lk(m_mtx); m_name = toUtf8(v.pwszVal); }
                PropVariantClear(&v);
            }
            ok = SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)ac.GetAddressOf()));
        }
        if (ok) ok = SUCCEEDED(ac->GetMixFormat(&wf));
        if (ok) ok = SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000 /*20 ms*/, 0, wf, nullptr));
        if (ok) ok = SUCCEEDED(ac->SetEventHandle(ev)) && SUCCEEDED(ac->GetService(IID_PPV_ARGS(&rc)));
        UINT32 bufFrames = 0;
        if (ok) ok = SUCCEEDED(ac->GetBufferSize(&bufFrames)) && SUCCEEDED(ac->Start());
        if (!ok) {
            if (wf) CoTaskMemFree(wf);
            setErr("não foi possível abrir a placa de som (a tentar de novo)");
            for (int i = 0; i < 20 && !m_stop.load(); ++i) Sleep(100);
            continue;
        }
        const int rate = (int)wf->nSamplesPerSec, ch = wf->nChannels, bits = wf->wBitsPerSample, blockAlign = wf->nBlockAlign;
        bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                       (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf)->SubFormat.Data1 == 3);
        CoTaskMemFree(wf);
        m_rate = rate;
        { std::lock_guard<std::mutex> lk(m_mtx); m_err.clear(); }
        m_ok = true;
        PS_INFO("Áudio: %s, %d Hz, %d canais", deviceName().c_str(), rate, ch);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        std::vector<float> mix;
        bool lost = false;
        while (!m_stop.load()) {
            WaitForSingleObject(ev, 100);
            UINT32 pad = 0;
            HRESULT hr = ac->GetCurrentPadding(&pad);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || FAILED(hr)) { lost = true; break; }
            UINT32 n = bufFrames - pad;
            if (n == 0) continue;
            mix.assign((size_t)n * 2, 0.f);
            // instante em que o 1.º sample deste bloco vai ser ouvido
            double when = nowSeconds() + (double)pad / rate;
            std::vector<std::shared_ptr<AudioSource>> live;
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                for (auto it = m_sources.begin(); it != m_sources.end();) {
                    if (auto s = it->lock()) { live.push_back(s); ++it; }
                    else it = m_sources.erase(it);
                }
            }
            std::map<uint32_t, VuLevel> layers;
            for (auto& s : live) {
                if (s->m_rate != rate) { s->clear(); s->m_rate = rate; }
                s->render(mix.data(), (int)n, rate, when);
                VuLevel v = s->vu();
                VuLevel& lv = layers[s->m_layer];
                lv.l = std::max(lv.l, v.l); lv.r = std::max(lv.r, v.r);
            }
            const float m = m_master.load();
            float pl = 0, pr = 0;
            for (UINT32 i = 0; i < n; ++i) {
                float L = mix[i * 2] * m, R = mix[i * 2 + 1] * m;
                pl = std::max(pl, std::fabs(L)); pr = std::max(pr, std::fabs(R));
                mix[i * 2] = std::clamp(L, -1.f, 1.f); mix[i * 2 + 1] = std::clamp(R, -1.f, 1.f);
            }
            m_mL = std::max(pl, m_mL.load() * 0.85f);
            m_mR = std::max(pr, m_mR.load() * 0.85f);
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                for (auto& [id, v] : m_layerVu) { v.l *= 0.85f; v.r *= 0.85f; }
                for (auto& [id, v] : layers) { VuLevel& d = m_layerVu[id]; d.l = std::max(d.l, v.l); d.r = std::max(d.r, v.r); }
            }
            BYTE* data = nullptr;
            if (FAILED(rc->GetBuffer(n, &data))) { lost = true; break; }
            if (isFloat && bits == 32) {
                float* o = (float*)data;
                for (UINT32 i = 0; i < n; ++i)
                    for (int c = 0; c < ch; ++c) o[i * ch + c] = ch == 1 ? (mix[i * 2] + mix[i * 2 + 1]) * 0.5f : c < 2 ? mix[i * 2 + c] : 0.f;
            } else if (bits == 16) {
                int16_t* o = (int16_t*)data;
                for (UINT32 i = 0; i < n; ++i)
                    for (int c = 0; c < ch; ++c) {
                        float v = ch == 1 ? (mix[i * 2] + mix[i * 2 + 1]) * 0.5f : c < 2 ? mix[i * 2 + c] : 0.f;
                        o[i * ch + c] = (int16_t)std::lround(v * 32767.f);
                    }
            } else memset(data, 0, (size_t)n * blockAlign);
            rc->ReleaseBuffer(n, 0);
        }
        ac->Stop();
        m_ok = false;
        if (lost && !m_stop.load()) { setErr("placa de som desligada (a tentar de novo)"); Sleep(1000); }
    }
    CloseHandle(ev);
    if (SUCCEEDED(hrCo)) CoUninitialize();
}

} // namespace ps
