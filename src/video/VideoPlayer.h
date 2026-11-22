#pragma once
#include "gfx/Gfx.h"
#include "core/Model.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVFrame;
struct AVBufferRef;

namespace ps {

// Inicialização global do FFmpeg + dispositivo D3D11VA partilhado com o renderer
namespace VideoSystem {
    bool init();
    void shutdown();
    AVBufferRef* hwDevice();   // pode ser nullptr (sem descodificação por hardware)
}

enum class DecodePath { None, Hap, Hardware, Software };

struct MediaInfo {
    int width = 0, height = 0;
    double fps = 0, duration = 0;
    std::string codec;          // ex. "HAP Q", "H.264 (GPU)", "ProRes (CPU)"
};

// Um clip em reprodução. Thread de descodificação própria + fila de frames.
// Toda a interação com a GPU (upload) acontece na thread de render em update().
class VideoPlayer {
public:
    enum class State { Opening, Playing, Ended, Error };

    VideoPlayer(const std::string& pathUtf8, LoopMode loop, float speed);
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // thread de render: avança o relógio e envia para a GPU o frame devido.
    void update(double now);
    void restart();                       // volta ao início
    void setSpeed(float s);
    void setLoop(LoopMode m) { m_loop.store(m); }

    State state() const { return m_state.load(); }
    bool  hasFrame() const { return m_hasFrame; }
    bool  finished() const;               // PlayOnce chegou ao fim
    std::string error() const;
    MediaInfo info() const;
    double position() const { return m_playhead.load(); }
    const std::string& path() const { return m_path; }

    // desenho
    const VideoTexture& texture() const { return m_tex; }
    PSKind psKind() const { return m_psKind; }
    const YuvMatrix* yuv() const { return m_path_ == DecodePath::Hardware ? &m_yuv : nullptr; }
    float uvMaxX() const { return m_uvMax[0]; }
    float uvMaxY() const { return m_uvMax[1]; }
    int   width() const { return m_info.width; }
    int   height() const { return m_info.height; }

    // destrói sem bloquear a thread de render (join noutra thread)
    static void destroyAsync(std::unique_ptr<VideoPlayer> p);

    // estatísticas
    int queued() const;
    int dropped() const { return m_dropped.load(); }

private:
    struct Frame {
        ~Frame();
        int gen = 0;
        double t = 0;                 // tempo na timeline do clip (s), inclui loops
        AVFrame* hw = nullptr;        // frame D3D11 (hardware)
        std::vector<uint8_t> buf[2];  // HAP: blocos BC; SW: BGRA
        int w = 0, h = 0;
        uint32_t pitch[2] = {0, 0};
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN, fmt2 = DXGI_FORMAT_UNKNOWN;
        PSKind ps = PSKind::RGBA;
    };

    void threadMain();
    bool openAndRun();
    void pushFrame(std::unique_ptr<Frame> f);
    std::unique_ptr<Frame> takeFree();
    void recycle(std::unique_ptr<Frame> f);
    void upload(Frame& f);
    void fail(const std::string& msg);

    std::string m_path;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_restartReq{false};
    std::atomic<int> m_gen{0};
    std::atomic<State> m_state{State::Opening};
    std::atomic<LoopMode> m_loop;
    std::atomic<float> m_speed;
    std::atomic<double> m_playhead{0};
    std::atomic<int> m_dropped{0};
    std::atomic<bool> m_eof{false};

    mutable std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<std::unique_ptr<Frame>> m_queue;
    std::vector<std::unique_ptr<Frame>> m_free;
    std::string m_error;
    MediaInfo m_info;
    size_t m_maxQueue = 6;

    // render thread
    bool   m_started = false;
    double m_clockBase = 0;      // instante (now) correspondente ao tempo m_clockT
    double m_clockT = 0;
    bool   m_hasFrame = false;
    double m_shownT = -1;
    VideoTexture m_tex;
    PSKind m_psKind = PSKind::RGBA;
    YuvMatrix m_yuv{};
    float m_uvMax[2] = {1, 1};
    DecodePath m_path_ = DecodePath::None;
};

} // namespace ps
