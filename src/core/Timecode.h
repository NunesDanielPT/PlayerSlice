#pragma once
// Entrada de timecode SMPTE: LTC (áudio), MTC (MIDI) ou gerador interno.
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

namespace ps {

enum class TcSource : int { Off = 0, LTC = 1, MTC = 2, Internal = 3 };

struct TcConfig {
    TcSource    source = TcSource::Off;
    std::string audioId;        // dispositivo de entrada (WASAPI); "" = predefinido
    int         channel = 0;    // 0 = esquerdo, 1 = direito
    std::string midiName;       // entrada MIDI (nome); "" = primeira
    float       fps = 0;        // 0 = automático (detetado); senão 24, 25, 29.97, 30
    double      offset = 0;     // segundos somados ao timecode recebido
    int         freewheel = 10; // frames que continua a contar se o sinal falhar
    bool operator==(const TcConfig&) const = default;
};

struct TcStatus {
    bool   valid = false;       // já recebeu timecode (ou gerador ativo)
    bool   running = false;     // a avançar agora
    bool   signal = false;      // sinal a chegar neste momento (sem freewheel)
    double seconds = 0;         // posição (com offset), em segundos
    float  fps = 25;
    bool   drop = false;
    float  level = 0;           // nível de entrada LTC (0..1)
    std::string device;         // nome do dispositivo em uso
    std::string error;
};

// "01:02:03:04" ↔ segundos
std::string tcFormat(double seconds, float fps, bool drop = false);
bool        tcParse(const std::string& s, float fps, double& seconds);

class Timecode {
public:
    static Timecode& get();
    void configure(const TcConfig& c);    // (re)abre o dispositivo se mudou
    void shutdown();
    TcStatus status() const;              // posição interpolada no instante atual

    // gerador interno
    void genPlay(bool play);
    void genSet(double seconds);
    bool genPlaying() const;

    struct Device { std::string id, name; };
    static std::vector<Device> audioInputs();
    static std::vector<std::string> midiInputs();

    // chamado pelas threads de entrada (frame completo recebido em tSeconds = relógio local)
    void feed(int h, int m, int s, int f, bool drop, float fpsGuess, double atClock, int kind); // kind: 0 LTC, 1 MTC, 2 locate
    void setLevel(float l) { m_level.store(l); }
    void setDeviceName(const std::string& n);
    void setError(const std::string& e);

private:
    Timecode() = default;
    void stopInput();
    void ltcThread(std::string audioId, int channel);
    void mtcThread(std::string midiName);

    TcConfig m_cfg;
    mutable std::mutex m_mtx;
    // último timecode recebido
    bool   m_valid = false;
    double m_baseSec = 0;       // posição do frame recebido (sem offset)
    double m_baseClock = 0;     // instante local em que foi recebido
    double m_lastFeed = -1;
    bool   m_moving = false;    // os frames recebidos estão a avançar
    float  m_fps = 25;
    bool   m_drop = false;
    int    m_maxFrame = 0;
    std::string m_device, m_error;
    std::atomic<float> m_level{0};
    // gerador
    bool   m_genPlay = false;
    double m_genSec = 0, m_genClock = 0;

    std::thread m_thread;
    std::atomic<bool> m_stop{false};
};

} // namespace ps
