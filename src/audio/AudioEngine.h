#pragma once
// Áudio: mistura o som dos clips no ar e envia para a placa de som escolhida (WASAPI partilhado).
// Cada VideoPlayer com som tem uma AudioSource: o descodificador mete lá blocos com o tempo do clip,
// e o misturador lê-os à posição da cabeça de leitura do vídeo (fica sincronizado, também com
// timecode, saltos e loops).
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps {

struct VuLevel { float l = 0, r = 0; };   // pico (0..1+) com descida suave

class AudioSource {
public:
    // descodificador: bloco estéreo intercalado a partir do tempo t0 (s, tempo do clip, com loops)
    void push(double t0, const float* stereo, int frames);
    void clear();                          // depois de um salto
    // render thread: posição do vídeo
    void setClock(double playhead, double atTime, double speed, bool running);
    // compositor: ganho final, pan (-1..1), a que layer pertence
    void setMix(float gain, float pan, uint32_t layerId, bool audible);
    VuLevel vu() const { return {m_vuL.load(), m_vuR.load()}; }

private:
    friend class AudioEngine;
    int  render(float* out, int frames, int rate, double nowSec);   // soma em out; devolve frames escritos
    struct Chunk { double t0; std::vector<float> s; };
    std::mutex m_mtx;
    std::deque<Chunk> m_chunks;
    double m_ph = 0, m_phAt = 0, m_speed = 1;
    bool   m_running = false;
    float  m_gain = 1, m_pan = 0;
    uint32_t m_layer = 0;
    bool   m_audible = false;
    std::atomic<float> m_vuL{0}, m_vuR{0};
    int    m_rate = 48000;                 // taxa dos blocos (= taxa da placa)
    // só a thread de áudio
    double m_readT = 0;
    bool   m_hasRead = false;
    float  m_lastGL = 1, m_lastGR = 1;
    friend class VideoPlayer;
};

class AudioEngine {
public:
    static AudioEngine& get();
    void start(const std::string& deviceId);   // (re)abre a placa
    void shutdown();
    int  sampleRate() const { return m_rate.load(); }
    bool running() const { return m_ok.load(); }
    std::string deviceName() const;
    std::string error() const;

    std::shared_ptr<AudioSource> createSource();
    void setMaster(float gain, bool mute) { m_master = mute ? 0.f : gain; }

    VuLevel masterVu() const { return {m_mL.load(), m_mR.load()}; }
    VuLevel layerVu(uint32_t layerId) const;

private:
    void thread(std::string id);
    std::thread m_thread;
    std::atomic<bool> m_stop{false}, m_ok{false};
    std::atomic<int> m_rate{48000};
    std::atomic<float> m_master{1.f};
    std::atomic<float> m_mL{0}, m_mR{0};
    mutable std::mutex m_mtx;
    std::vector<std::weak_ptr<AudioSource>> m_sources;
    std::map<uint32_t, VuLevel> m_layerVu;
    std::string m_name, m_err;
};

// dB para mostrar (0 → "-inf")
inline float gainToDb(float g) { return g <= 1e-5f ? -100.f : 20.f * std::log10(g); }

} // namespace ps
