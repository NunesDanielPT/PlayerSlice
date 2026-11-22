#include "core/Timecode.h"
#include "core/Ltc.h"
#include "core/Log.h"
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <cmath>
#include <algorithm>
#include <cstdio>

using Microsoft::WRL::ComPtr;

// {a45c254e-df1c-4efd-8020-67d146a850e0}, 14 = PKEY_Device_FriendlyName
static const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

namespace ps {

static int nominal(float fps) { return std::max(1, (int)std::lround(fps)); }

std::string tcFormat(double sec, float fps, bool drop) {
    if (sec < 0) sec = 0;
    int nf = nominal(fps);
    long long tf = (long long)std::floor(sec * nf + 1e-6);
    int f = (int)(tf % nf); long long ts = tf / nf;
    int s = (int)(ts % 60), m = (int)((ts / 60) % 60), h = (int)((ts / 3600) % 24);
    char b[32];
    snprintf(b, sizeof(b), "%02d:%02d:%02d%c%02d", h, m, s, drop ? ';' : ':', f);
    return b;
}

bool tcParse(const std::string& str, float fps, double& out) {
    int v[4] = {0, 0, 0, 0}, n = 0, cur = -1;
    for (char c : str) {
        if (c >= '0' && c <= '9') { cur = (cur < 0 ? 0 : cur) * 10 + (c - '0'); if (cur > 9999) return false; }
        else if (c == ':' || c == ';' || c == '.' || c == ' ') { if (cur >= 0) { if (n >= 4) return false; v[n++] = cur; cur = -1; } }
        else return false;
    }
    if (cur >= 0) { if (n >= 4) return false; v[n++] = cur; }
    if (n == 0) return false;
    // aceita "SS", "MM:SS", "HH:MM:SS" e "HH:MM:SS:FF"
    int h = 0, m = 0, s = 0, f = 0;
    if (n == 1) s = v[0];
    else if (n == 2) { m = v[0]; s = v[1]; }
    else if (n == 3) { h = v[0]; m = v[1]; s = v[2]; }
    else { h = v[0]; m = v[1]; s = v[2]; f = v[3]; }
    int nf = nominal(fps);
    if (m > 59 || s > 59 || f >= nf || h > 23) return false;
    out = h * 3600.0 + m * 60.0 + s + (double)f / nf;
    return true;
}

Timecode& Timecode::get() { static Timecode t; return t; }

void Timecode::setDeviceName(const std::string& n) { std::lock_guard<std::mutex> lk(m_mtx); m_device = n; }
void Timecode::setError(const std::string& e) {
    { std::lock_guard<std::mutex> lk(m_mtx); m_error = e; }
    if (!e.empty()) PS_WARN("Timecode: %s", e.c_str());
}

void Timecode::feed(int h, int m, int s, int f, bool drop, float fpsGuess, double atClock, int kind) {
    const bool locate = kind == 2;
    std::lock_guard<std::mutex> lk(m_mtx);
    // frame rate
    float fps = m_cfg.fps;
    m_maxFrame = std::max(m_maxFrame, f);
    if (fps <= 0) {
        if (fpsGuess > 0) {
            if (fpsGuess < 24.5f) fps = 24; else if (fpsGuess < 27.5f) fps = 25; else fps = drop ? 29.97f : 30;
        } else fps = m_fps;
        if (m_maxFrame >= 25 && fps < 29) fps = drop ? 29.97f : 30;      // frames > 24 → família 30
        if (m_maxFrame == 24 && fps < 25) fps = 25;
    }
    int nf = nominal(fps);
    double pos = h * 3600.0 + m * 60.0 + s + (double)std::min(f, nf - 1) / nf;
    if (!locate) pos += (kind == 0 ? 1.0 : 2.0) / nf;   // LTC: o frame acabou de chegar; MTC: 8 quartos = 2 frames
    // está a andar? (posição nova ≈ prevista)
    double predicted = m_valid ? m_baseSec + (atClock - m_baseClock) : -1;
    bool moving = !locate && m_valid && m_lastFeed > 0 && atClock - m_lastFeed < 0.25 &&
                  std::fabs(pos - predicted) < 4.0 / nf && pos != m_baseSec;
    if (!locate && m_valid && m_moving && std::fabs(pos - predicted) < 0.5 / nf) {
        // pequenas variações: manter o relógio liso (só corrige devagar)
        m_baseSec = predicted + (pos - predicted) * 0.2;
    } else m_baseSec = pos;
    m_baseClock = atClock;
    m_lastFeed = atClock;
    m_moving = moving;
    m_valid = true;
    m_fps = fps;
    m_drop = drop;
}

TcStatus Timecode::status() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    TcStatus st;
    double now = nowSeconds();
    st.device = m_device;
    st.error = m_error;
    st.level = m_level.load();
    if (m_cfg.source == TcSource::Off) return st;
    if (m_cfg.source == TcSource::Internal) {
        st.valid = true; st.signal = true; st.running = m_genPlay;
        st.fps = m_cfg.fps > 0 ? m_cfg.fps : 25;
        st.seconds = m_genSec + (m_genPlay ? now - m_genClock : 0);
        st.seconds = std::max(0.0, st.seconds + m_cfg.offset);
        st.device = "Gerador interno";
        return st;
    }
    st.valid = m_valid;
    st.fps = m_fps;
    st.drop = m_drop;
    if (!m_valid) return st;
    double age = now - m_lastFeed;
    double frame = 1.0 / std::max(1.f, m_fps);
    st.signal = age < frame * 4;
    st.running = m_moving && age < frame * (m_cfg.freewheel + 1.5);
    st.seconds = m_baseSec + (st.running ? now - m_baseClock : 0);
    st.seconds = std::max(0.0, st.seconds + m_cfg.offset);
    return st;
}

void Timecode::genPlay(bool play) {
    std::lock_guard<std::mutex> lk(m_mtx);
    double now = nowSeconds();
    if (m_genPlay) m_genSec += now - m_genClock;
    m_genClock = now;
    m_genPlay = play;
}
void Timecode::genSet(double s) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_genSec = std::max(0.0, s);
    m_genClock = nowSeconds();
}
bool Timecode::genPlaying() const { std::lock_guard<std::mutex> lk(m_mtx); return m_genPlay; }

void Timecode::configure(const TcConfig& c) {
    TcConfig old;
    { std::lock_guard<std::mutex> lk(m_mtx); old = m_cfg; }
    bool reopen = old.source != c.source || (c.source == TcSource::LTC && (old.audioId != c.audioId || old.channel != c.channel)) ||
                  (c.source == TcSource::MTC && old.midiName != c.midiName) || !m_thread.joinable();
    if (c.source == TcSource::Off || c.source == TcSource::Internal) reopen = old.source != c.source;
    if (reopen) stopInput();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_cfg = c;
        if (reopen) { m_valid = false; m_moving = false; m_lastFeed = -1; m_maxFrame = 0; m_error.clear(); m_device.clear(); m_level = 0; }
    }
    if (!reopen) return;
    m_stop = false;
    if (c.source == TcSource::LTC) m_thread = std::thread(&Timecode::ltcThread, this, c.audioId, c.channel);
    else if (c.source == TcSource::MTC) m_thread = std::thread(&Timecode::mtcThread, this, c.midiName);
}

void Timecode::stopInput() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
}

void Timecode::shutdown() { stopInput(); }

// ---------------------------------------------------------------------------
// LTC: entrada de áudio (WASAPI partilhado)
// ---------------------------------------------------------------------------
std::vector<Timecode::Device> Timecode::audioInputs() {
    std::vector<Device> out;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return out;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(col->Item(i, &d))) continue;
        Device dv;
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id))) { dv.id = toUtf8(id); CoTaskMemFree(id); }
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) dv.name = toUtf8(v.pwszVal);
            PropVariantClear(&v);
        }
        out.push_back(dv);
    }
    return out;
}

void Timecode::ltcThread(std::string audioId, int channel) {
    
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto run = [&]() -> bool {
        ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) { setError("Áudio indisponível"); return false; }
        ComPtr<IMMDevice> dev;
        if (!audioId.empty()) en->GetDevice(fromUtf8(audioId).c_str(), &dev);
        if (!dev && FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev))) { setError("Sem entrada de áudio"); return false; }
        {
            ComPtr<IPropertyStore> ps;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) setDeviceName(toUtf8(v.pwszVal));
                PropVariantClear(&v);
            }
        }
        ComPtr<IAudioClient> ac;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)ac.GetAddressOf()))) { setError("Não foi possível abrir a entrada de áudio"); return false; }
        WAVEFORMATEX* wf = nullptr;
        if (FAILED(ac->GetMixFormat(&wf))) { setError("Formato de áudio desconhecido"); return false; }
        const int chans = wf->nChannels, sr = (int)wf->nSamplesPerSec, bits = wf->wBitsPerSample;
        bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf);
            isFloat = ext->SubFormat.Data1 == 3;   // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
        }
        HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000 /*20 ms*/, 0, wf, nullptr);
        CoTaskMemFree(wf);
        if (FAILED(hr)) { setError("Não foi possível iniciar a captura de áudio"); return false; }
        ac->SetEventHandle(ev);
        ComPtr<IAudioCaptureClient> cap;
        if (FAILED(ac->GetService(IID_PPV_ARGS(&cap)))) { setError("Captura de áudio indisponível"); return false; }
        if (FAILED(ac->Start())) { setError("Não foi possível iniciar a captura de áudio"); return false; }
        PS_INFO("Timecode LTC: %d Hz, %d canais, canal %d", sr, chans, channel + 1);
        setError("");
        const int ch = std::clamp(channel, 0, std::max(0, chans - 1));
        LtcDecoder dec(sr);
        std::vector<float> mono;
        int64_t samplePos = 0;
        while (!m_stop.load()) {
            WaitForSingleObject(ev, 100);
            UINT32 pk = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&pk)) && pk > 0) {
                BYTE* data = nullptr; UINT32 nfr = 0; DWORD flags = 0; UINT64 devPos = 0, qpc = 0;
                if (FAILED(cap->GetBuffer(&data, &nfr, &flags, &devPos, &qpc))) break;
                mono.resize(nfr);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(mono.begin(), mono.end(), 0.f);
                else if (isFloat && bits == 32) { const float* f = (const float*)data; for (UINT32 i = 0; i < nfr; ++i) mono[i] = f[i * chans + ch]; }
                else if (bits == 16) { const int16_t* s = (const int16_t*)data; for (UINT32 i = 0; i < nfr; ++i) mono[i] = s[i * chans + ch] / 32768.f; }
                else if (bits == 32) { const int32_t* s = (const int32_t*)data; for (UINT32 i = 0; i < nfr; ++i) mono[i] = s[i * chans + ch] / 2147483648.f; }
                else if (bits == 24) { const uint8_t* b = data; for (UINT32 i = 0; i < nfr; ++i) { const uint8_t* p = b + (i * chans + ch) * 3; int32_t v = (p[0] << 8 | p[1] << 16 | p[2] << 24); mono[i] = v / 2147483648.f; } }
                double t0 = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || qpc == 0 ? nowSeconds() - (double)nfr / sr : qpc * 1e-7;
                const int64_t first = samplePos;
                dec.process(mono.data(), (int)nfr, [&](const LtcFrame& F) {
                    double at = t0 + (F.sampleEnd - (double)first) / sr;
                    feed(F.h, F.m, F.s, F.f, F.drop, F.fpsEst, at, 0);
                });
                samplePos += nfr;
                cap->ReleaseBuffer(nfr);
                m_level.store(dec.peak());
            }
        }
        ac->Stop();
        return true;
    };
    run();
    CloseHandle(ev);
    if (SUCCEEDED(hrCo)) CoUninitialize();
}

// ---------------------------------------------------------------------------
// MTC: entrada MIDI (quarter frames + full frame SysEx)
// ---------------------------------------------------------------------------
std::vector<std::string> Timecode::midiInputs() {
    std::vector<std::string> out;
    UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) out.push_back(toUtf8(caps.szPname));
    }
    return out;
}

void Timecode::mtcThread(std::string midiName) {
    
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // cria a fila de mensagens
    UINT n = midiInGetNumDevs(), dev = UINT_MAX;
    std::string name;
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        std::string nm = toUtf8(caps.szPname);
        if (midiName.empty() || nm == midiName) { dev = i; name = nm; break; }
    }
    if (dev == UINT_MAX) { setError(n ? "Entrada MIDI não encontrada" : "Não há entradas MIDI"); return; }
    HMIDIIN h = nullptr;
    if (midiInOpen(&h, dev, (DWORD_PTR)GetCurrentThreadId(), 0, CALLBACK_THREAD) != MMSYSERR_NOERROR) {
        setError("Não foi possível abrir a entrada MIDI (está a ser usada por outro programa?)");
        return;
    }
    setDeviceName(name);
    setError("");
    PS_INFO("Timecode MTC: %s", name.c_str());
    static const int kBufs = 4;
    MIDIHDR hdr[kBufs]{};
    char buf[kBufs][256];
    for (int i = 0; i < kBufs; ++i) {
        hdr[i].lpData = buf[i]; hdr[i].dwBufferLength = sizeof(buf[i]);
        midiInPrepareHeader(h, &hdr[i], sizeof(MIDIHDR));
        midiInAddBuffer(h, &hdr[i], sizeof(MIDIHDR));
    }
    midiInStart(h);
    int qf[8] = {0}; int seen = 0;
    while (!m_stop.load()) {
        MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (!PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) continue;
        if (msg.message == WM_QUIT) break;
        if (msg.message == MM_MIM_DATA) {
            DWORD d = (DWORD)msg.lParam;
            if ((d & 0xFF) != 0xF1) continue;
            int b = (d >> 8) & 0x7F, piece = b >> 4;
            qf[piece] = b & 0x0F;
            seen = piece == 0 ? 1 : (seen | (1 << piece));
            if (piece == 7 && seen == 0xFF) {
                int fr = qf[0] | (qf[1] << 4), s = qf[2] | (qf[3] << 4), m = qf[4] | (qf[5] << 4);
                int hh = qf[6] | ((qf[7] & 1) << 4), rate = (qf[7] >> 1) & 3;
                static const float rates[4] = {24, 25, 29.97f, 30};
                feed(hh, m, s, fr, rate == 2, rates[rate], nowSeconds(), 1);
                seen = 0;
            }
        } else if (msg.message == MM_MIM_LONGDATA) {
            MIDIHDR* hd = (MIDIHDR*)msg.lParam;
            const uint8_t* p = (const uint8_t*)hd->lpData;
            if (hd->dwBytesRecorded >= 10 && p[0] == 0xF0 && p[1] == 0x7F && p[3] == 0x01 && p[4] == 0x01) {
                int rate = (p[5] >> 5) & 3;
                static const float rates[4] = {24, 25, 29.97f, 30};
                feed(p[5] & 0x1F, p[6], p[7], p[8], rate == 2, rates[rate], nowSeconds(), 2);
            }
            if (!m_stop.load()) midiInAddBuffer(h, hd, sizeof(MIDIHDR));
        }
    }
    midiInStop(h);
    midiInReset(h);
    for (int i = 0; i < kBufs; ++i) midiInUnprepareHeader(h, &hdr[i], sizeof(MIDIHDR));
    midiInClose(h);
}

} // namespace ps
