#include "video/VideoPlayer.h"
#include "core/Log.h"
#include "core/ThreadPool.h"
#include "core/Settings.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#include <hap.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ps {

// ===========================================================================
// VideoSystem
// ===========================================================================
static AVBufferRef* g_hwDevice = nullptr;
static std::atomic<int> g_pendingDeletes{0};

static void ffLog(void* avcl, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_WARNING) return;
    char line[1024]; int prefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &prefix);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (level <= AV_LOG_ERROR) PS_ERROR("ffmpeg: %s", line);
    else                       PS_WARN("ffmpeg: %s", line);
}

static void lockCtx(void* p)   { static_cast<std::recursive_mutex*>(p)->lock(); }
static void unlockCtx(void* p) { static_cast<std::recursive_mutex*>(p)->unlock(); }

bool VideoSystem::init() {
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(ffLog);

    g_hwDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (g_hwDevice) {
        auto* dc = reinterpret_cast<AVHWDeviceContext*>(g_hwDevice->data);
        auto* d3d = static_cast<AVD3D11VADeviceContext*>(dc->hwctx);
        d3d->device = gfx().dev();
        d3d->device->AddRef();
        d3d->lock = lockCtx;
        d3d->unlock = unlockCtx;
        d3d->lock_ctx = &gfx().ctxMutex();
        if (av_hwdevice_ctx_init(g_hwDevice) < 0) {
            PS_WARN("D3D11VA indisponível — vídeos H.264/H.265 serão descodificados pela CPU");
            av_buffer_unref(&g_hwDevice);
        } else {
            PS_INFO("Descodificação por hardware D3D11VA ativa");
        }
    }
    return true;
}

void VideoSystem::shutdown() {
    // esperar (até 3 s) pelos players a serem destruídos em segundo plano
    for (int i = 0; i < 300 && g_pendingDeletes.load() > 0; ++i) Sleep(10);
    av_buffer_unref(&g_hwDevice);
}
AVBufferRef* VideoSystem::hwDevice() { return g_hwDevice; }

// ===========================================================================
// helpers
// ===========================================================================
static bool hapFormatToDxgi(unsigned hf, DXGI_FORMAT& fmt, unsigned& blockBytes) {
    switch (hf) {
    case HapTextureFormat_RGB_DXT1:        fmt = DXGI_FORMAT_BC1_UNORM; blockBytes = 8;  return true;
    case HapTextureFormat_RGBA_DXT5:       fmt = DXGI_FORMAT_BC3_UNORM; blockBytes = 16; return true;
    case HapTextureFormat_YCoCg_DXT5:      fmt = DXGI_FORMAT_BC3_UNORM; blockBytes = 16; return true;
    case HapTextureFormat_A_RGTC1:         fmt = DXGI_FORMAT_BC4_UNORM; blockBytes = 8;  return true;
    case HapTextureFormat_RGBA_BPTC_UNORM: fmt = DXGI_FORMAT_BC7_UNORM; blockBytes = 16; return true;
    case HapTextureFormat_RGB_BPTC_UNSIGNED_FLOAT: fmt = DXGI_FORMAT_BC6H_UF16; blockBytes = 16; return true;
    case HapTextureFormat_RGB_BPTC_SIGNED_FLOAT:   fmt = DXGI_FORMAT_BC6H_SF16; blockBytes = 16; return true;
    default: return false;
    }
}

static void hapParallel(HapDecodeWorkFunction function, void* p, unsigned int count, void*) {
    ThreadPool::get().parallelFor(count, [&](unsigned i) { function(p, i); });
}

static enum AVPixelFormat getHwFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_D3D11) return *p;
    PS_WARN("Descodificador sem suporte D3D11 para este ficheiro — a usar CPU");
    return avcodec_default_get_format(ctx, fmts);
}

static bool codecLikesHw(AVCodecID id) {
    switch (id) {
    case AV_CODEC_ID_H264: case AV_CODEC_ID_HEVC: case AV_CODEC_ID_VP9:
    case AV_CODEC_ID_AV1:  case AV_CODEC_ID_MPEG2VIDEO: case AV_CODEC_ID_VC1:
        return true;
    default: return false;
    }
}

static std::string codecName(AVCodecID id, const char* fallback) {
    switch (id) {
    case AV_CODEC_ID_H264: return "H.264";
    case AV_CODEC_ID_HEVC: return "H.265";
    case AV_CODEC_ID_PRORES: return "ProRes";
    case AV_CODEC_ID_DNXHD: return "DNxHD/HR";
    default: break;
    }
    const AVCodecDescriptor* d = avcodec_descriptor_get(id);
    return d ? d->name : fallback;
}

// ===========================================================================
// VideoPlayer
// ===========================================================================
VideoPlayer::Frame::~Frame() { if (hw) av_frame_free(&hw); }


VideoPlayer::VideoPlayer(const std::string& pathUtf8, LoopMode loop, float speed)
    : m_path(pathUtf8), m_loop(loop), m_speed(speed) {
    m_maxQueue = (size_t)std::clamp(settings().decodeBuffer, 3, 16);
    m_thread = std::thread([this] { threadMain(); });
}

VideoPlayer::~VideoPlayer() {
    m_stop = true;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto& f : m_queue) if (f->hw) av_frame_free(&f->hw);
    m_queue.clear();
}

void VideoPlayer::destroyAsync(std::unique_ptr<VideoPlayer> p) {
    if (!p) return;
    p->m_stop = true;
    p->m_cv.notify_all();
    VideoPlayer* raw = p.release();
    g_pendingDeletes++;
    std::thread([raw] { delete raw; g_pendingDeletes--; }).detach();
}

bool VideoPlayer::finished() const {
    if (m_loop.load() == LoopMode::Loop) return false;
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_eof.load() && m_queue.empty() && m_started;
}

std::string VideoPlayer::error() const { std::lock_guard<std::mutex> lk(m_mtx); return m_error; }
MediaInfo VideoPlayer::info() const { std::lock_guard<std::mutex> lk(m_mtx); return m_info; }
int VideoPlayer::queued() const { std::lock_guard<std::mutex> lk(m_mtx); return (int)m_queue.size(); }

void VideoPlayer::fail(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_error = msg;
    }
    m_state = State::Error;
    PS_ERROR("%s — %s", msg.c_str(), m_path.c_str());
}

void VideoPlayer::restart() {
    m_gen++;
    m_restartReq = true;
    m_cv.notify_all();
    m_started = false;
}

void VideoPlayer::chase(double t) {
    m_chase = true;
    m_chaseT = t;
}

void VideoPlayer::setSpeed(float s) {
    // rebase do relógio para não saltar
    if (m_started) {
        double now = nowSeconds();
        m_clockT = m_clockT + (now - m_clockBase) * m_speed.load();
        m_clockBase = now;
    }
    m_speed = std::clamp(s, 0.0f, 8.0f);
}

std::unique_ptr<VideoPlayer::Frame> VideoPlayer::takeFree() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_free.empty()) {
        auto f = std::move(m_free.back());
        m_free.pop_back();
        return f;
    }
    return std::make_unique<Frame>();
}

void VideoPlayer::recycle(std::unique_ptr<Frame> f) {
    if (f->hw) av_frame_free(&f->hw);
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_free.size() < 4) m_free.push_back(std::move(f));
}

void VideoPlayer::pushFrame(std::unique_ptr<Frame> f) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_queue.push_back(std::move(f));
}

void VideoPlayer::threadMain() {
    using SetDescFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static auto setDesc = (SetDescFn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
    if (setDesc) setDesc(GetCurrentThread(), L"PLAYERSLICE decoder");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    try {
        openAndRun();
    } catch (const std::exception& e) {
        fail(std::string("Exceção no descodificador: ") + e.what());
    } catch (...) {
        fail("Exceção desconhecida no descodificador");
    }
}

namespace {
struct FFGuard {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* fr = nullptr;
    SwsContext* sws = nullptr;
    ~FFGuard() {
        if (sws) sws_free_context(&sws);
        if (fr) av_frame_free(&fr);
        if (pkt) av_packet_free(&pkt);
        if (dec) avcodec_free_context(&dec);
        if (fmt) avformat_close_input(&fmt);
    }
};
}

bool VideoPlayer::openAndRun() {
    FFGuard g;
    char errbuf[256];

    int r = avformat_open_input(&g.fmt, m_path.c_str(), nullptr, nullptr);
    if (r < 0) { av_strerror(r, errbuf, sizeof(errbuf)); fail(std::string("Não foi possível abrir: ") + errbuf); return false; }
    avformat_find_stream_info(g.fmt, nullptr);

    int vs = av_find_best_stream(g.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { fail("O ficheiro não tem vídeo"); return false; }
    for (unsigned i = 0; i < g.fmt->nb_streams; ++i)
        if ((int)i != vs) g.fmt->streams[i]->discard = AVDISCARD_ALL;

    AVStream* st = g.fmt->streams[vs];
    AVCodecParameters* par = st->codecpar;
    const AVRational tb = st->time_base;
    const int64_t startPts = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    double fps = av_q2d(st->avg_frame_rate);
    if (fps <= 0 || fps > 1000) fps = av_q2d(st->r_frame_rate);
    if (fps <= 0 || fps > 1000) fps = 25.0;
    const double frameDur = 1.0 / fps;
    double duration = st->duration != AV_NOPTS_VALUE ? st->duration * av_q2d(tb)
                    : (g.fmt->duration > 0 ? g.fmt->duration / (double)AV_TIME_BASE : 0);

    const bool isHap = par->codec_id == AV_CODEC_ID_HAP;
    DecodePath path = isHap ? DecodePath::Hap : DecodePath::Software;
    std::string codecLabel;

    if (!isHap) {
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec) { fail("Codec não suportado"); return false; }
        g.dec = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(g.dec, par);
        g.dec->pkt_timebase = tb;

        bool hw = false;
        if (settings().hwDecode && VideoSystem::hwDevice() && codecLikesHw(par->codec_id)) {
            for (int i = 0;; ++i) {
                const AVCodecHWConfig* c = avcodec_get_hw_config(codec, i);
                if (!c) break;
                if (c->device_type == AV_HWDEVICE_TYPE_D3D11VA && (c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) { hw = true; break; }
            }
        }
        if (hw) {
            g.dec->hw_device_ctx = av_buffer_ref(VideoSystem::hwDevice());
            g.dec->get_format = getHwFormat;
            g.dec->extra_hw_frames = (int)m_maxQueue + 4;
            g.dec->thread_count = 1;
            path = DecodePath::Hardware;
        } else {
            g.dec->thread_count = 0;  // automático
            g.dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        r = avcodec_open2(g.dec, codec, nullptr);
        if (r < 0) { av_strerror(r, errbuf, sizeof(errbuf)); fail(std::string("Erro a abrir o descodificador: ") + errbuf); return false; }
        codecLabel = codecName(par->codec_id, codec->name);
    } else {
        codecLabel = "HAP";
        if (par->codec_tag == MKTAG('H', 'a', 'p', '5')) codecLabel = "HAP Alpha";
        else if (par->codec_tag == MKTAG('H', 'a', 'p', 'Y')) codecLabel = "HAP Q";
        else if (par->codec_tag == MKTAG('H', 'a', 'p', 'M')) codecLabel = "HAP Q Alpha";
        else if (par->codec_tag == MKTAG('H', 'a', 'p', '7')) codecLabel = "HAP R";
    }

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_info.width = par->width;
        m_info.height = par->height;
        m_info.fps = fps;
        m_info.duration = duration;
        m_info.codec = codecLabel + (path == DecodePath::Hardware ? " (GPU)" : path == DecodePath::Software ? " (CPU)" : "");
    }
    PS_INFO("Abrir %s — %dx%d %.2ffps %s", m_path.c_str(), par->width, par->height, fps, m_info.codec.c_str());

    g.pkt = av_packet_alloc();
    g.fr = av_frame_alloc();

    // relógio e loops
    double loopOffset = 0;
    double loopLen = duration > 0 ? duration : 0;
    double maxRelT = 0;
    double lastT = -frameDur;
    int consecutiveErrors = 0;
    bool firstPushed = false;
    double seekTarget = -1;      // depois de um salto: ignorar frames antes deste tempo
    int gen = m_gen.load();
    int swW = 0, swH = 0, swFmt = -1;
    const YuvMatrix yuv709 = makeYuvMatrix(true, false), yuv601 = makeYuvMatrix(false, false);
    const YuvMatrix yuv709f = makeYuvMatrix(true, true), yuv601f = makeYuvMatrix(false, true);

    auto relTime = [&](int64_t pts) -> double {
        if (pts == AV_NOPTS_VALUE) return lastT + frameDur - loopOffset;
        return (pts - startPts) * av_q2d(tb);
    };

    auto clearQueue = [&] {
        std::deque<std::unique_ptr<Frame>> old;
        { std::lock_guard<std::mutex> lk(m_mtx); old.swap(m_queue); }
        for (auto& f : old) recycle(std::move(f));
    };

    auto seekStart = [&] {
        av_seek_frame(g.fmt, vs, startPts, AVSEEK_FLAG_BACKWARD);
        if (g.dec) avcodec_flush_buffers(g.dec);
    };

    auto emitDecoded = [&](AVFrame* fr) {
        double rel = relTime(fr->best_effort_timestamp);
        double t = rel + loopOffset;
        maxRelT = std::max(maxRelT, rel);
        lastT = t;
        if (seekTarget >= 0) { if (t < seekTarget - frameDur * 0.5) return; seekTarget = -1; }
        // descartar frames muito atrasados (exceto o primeiro)
        if (firstPushed && t < m_playhead.load() - frameDur * 1.5) { m_dropped++; return; }

        auto f = takeFree();
        f->t = t;
        f->gen = gen;
        f->w = fr->width; f->h = fr->height;
        bool full = fr->color_range == AVCOL_RANGE_JPEG;
        bool bt709 = fr->colorspace == AVCOL_SPC_BT709 || (fr->colorspace == AVCOL_SPC_UNSPECIFIED && fr->height >= 720);
        if (fr->format == AV_PIX_FMT_D3D11) {
            f->hw = av_frame_clone(fr);
            auto* tex = reinterpret_cast<ID3D11Texture2D*>(fr->data[0]);
            D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
            f->fmt = td.Format;
            f->ps = PSKind::YUV;
            f->buf[0].clear();
            // matriz guardada no próprio frame (pitch[0] usado como flag)
            f->pitch[0] = (bt709 ? 1u : 0u) | (full ? 2u : 0u);
        } else {
            if (fr->width != swW || fr->height != swH || fr->format != swFmt || !g.sws) {
                if (g.sws) sws_free_context(&g.sws);
                g.sws = sws_alloc_context();
                av_opt_set_int(g.sws, "srcw", fr->width, 0);
                av_opt_set_int(g.sws, "srch", fr->height, 0);
                av_opt_set_int(g.sws, "src_format", fr->format, 0);
                av_opt_set_int(g.sws, "dstw", fr->width, 0);
                av_opt_set_int(g.sws, "dsth", fr->height, 0);
                av_opt_set_int(g.sws, "dst_format", AV_PIX_FMT_BGRA, 0);
                av_opt_set_int(g.sws, "sws_flags", SWS_BILINEAR, 0);
                av_opt_set_int(g.sws, "threads", 0, 0);
                if (sws_init_context(g.sws, nullptr, nullptr) < 0) { sws_free_context(&g.sws); fail("Formato de píxeis não suportado"); return; }
                const int* coefs = sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
                const int* dstc = sws_getCoefficients(SWS_CS_DEFAULT);
                sws_setColorspaceDetails(g.sws, coefs, full ? 1 : 0, dstc, 1, 0, 1 << 16, 1 << 16);
                swW = fr->width; swH = fr->height; swFmt = fr->format;
            }
            f->pitch[0] = (uint32_t)fr->width * 4;
            f->buf[0].resize((size_t)f->pitch[0] * fr->height);
            uint8_t* dst[4] = {f->buf[0].data(), nullptr, nullptr, nullptr};
            int dstStride[4] = {(int)f->pitch[0], 0, 0, 0};
            sws_scale(g.sws, fr->data, fr->linesize, 0, fr->height, dst, dstStride);
            f->fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
            f->ps = PSKind::RGBA;
        }
        (void)yuv709; (void)yuv601; (void)yuv709f; (void)yuv601f;
        pushFrame(std::move(f));
        firstPushed = true;
        m_state = State::Playing;
    };

    auto drainDecoder = [&] {
        while (true) {
            int rr = avcodec_receive_frame(g.dec, g.fr);
            if (rr < 0) break;
            emitDecoded(g.fr);
            av_frame_unref(g.fr);
        }
    };

    while (!m_stop.load()) {
        if (m_restartReq.exchange(false)) {
            gen = m_gen.load();
            seekStart();
            clearQueue();
            loopOffset = 0; lastT = -frameDur;
            m_eof = false; firstPushed = false;
            m_playhead = 0;
            seekTarget = -1;
        }
        if (m_seekReq.exchange(false)) {
            gen = m_gen.load();
            double t = std::max(0.0, m_seekT.load());
            double base = 0;
            if (loopLen > 0 && m_loop.load() == LoopMode::Loop) base = std::floor(t / loopLen) * loopLen;
            else if (loopLen > 0) t = std::min(t, std::max(0.0, loopLen - frameDur));
            int64_t ts = startPts + (int64_t)((t - base) / av_q2d(tb));
            av_seek_frame(g.fmt, vs, ts, AVSEEK_FLAG_BACKWARD);
            if (g.dec) avcodec_flush_buffers(g.dec);
            clearQueue();
            loopOffset = base; lastT = t - frameDur;
            m_eof = false; firstPushed = false;
            seekTarget = t;
            m_playhead = t;
        }

        // fila cheia → esperar (sem ocupar CPU)
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_queue.size() >= m_maxQueue || m_eof.load()) {
                m_cv.wait_for(lk, std::chrono::milliseconds(4));
                continue;
            }
        }

        r = av_read_frame(g.fmt, g.pkt);
        if (r == AVERROR_EOF || (r < 0 && avio_feof(g.fmt->pb))) {
            if (g.dec) {
                avcodec_send_packet(g.dec, nullptr);
                drainDecoder();
                avcodec_flush_buffers(g.dec);
            }
            if (m_loop.load() == LoopMode::Loop) {
                (void)loopLen;
                loopOffset += std::max(maxRelT + frameDur, frameDur);
                seekStart();
            } else {
                m_eof = true;
            }
            continue;
        }
        if (r < 0) {
            if (++consecutiveErrors > 50) { fail("Erros de leitura repetidos"); return false; }
            Sleep(2);
            continue;
        }
        if (g.pkt->stream_index != vs) { av_packet_unref(g.pkt); continue; }

        if (isHap) {
            double rel = relTime(g.pkt->pts != AV_NOPTS_VALUE ? g.pkt->pts : g.pkt->dts);
            double t = rel + loopOffset;
            maxRelT = std::max(maxRelT, rel);
            lastT = t;
            if (seekTarget >= 0) { if (t < seekTarget - frameDur * 0.5) { av_packet_unref(g.pkt); continue; } seekTarget = -1; }
            if (firstPushed && t < m_playhead.load() - frameDur * 1.5) { m_dropped++; av_packet_unref(g.pkt); continue; }

            unsigned texCount = 0;
            if (HapGetFrameTextureCount(g.pkt->data, g.pkt->size, &texCount) != HapResult_No_Error || texCount < 1 || texCount > 2) {
                if (++consecutiveErrors > 50) { fail("Frames HAP inválidos"); return false; }
                av_packet_unref(g.pkt);
                continue;
            }
            auto f = takeFree();
            f->t = t;
            f->gen = gen;
            f->w = par->width; f->h = par->height;
            const int w4 = (par->width + 3) & ~3, h4 = (par->height + 3) & ~3;
            bool ok = true;
            unsigned fmt0 = 0;
            for (unsigned ti = 0; ti < texCount && ok; ++ti) {
                f->buf[ti].resize((size_t)w4 * h4);
                unsigned long used = 0; unsigned hf = 0;
                unsigned res = HapDecode(g.pkt->data, g.pkt->size, ti, hapParallel, nullptr,
                                         f->buf[ti].data(), (unsigned long)f->buf[ti].size(), &used, &hf);
                DXGI_FORMAT df; unsigned bb;
                if (res != HapResult_No_Error || !hapFormatToDxgi(hf, df, bb)) { ok = false; break; }
                if (ti == 0) { f->fmt = df; fmt0 = hf; } else f->fmt2 = df;
                f->pitch[ti] = (uint32_t)(w4 / 4) * bb;
            }
            av_packet_unref(g.pkt);
            if (!ok) { recycle(std::move(f)); if (++consecutiveErrors > 50) { fail("Erro a descodificar HAP"); return false; } continue; }
            if (texCount == 1) f->fmt2 = DXGI_FORMAT_UNKNOWN;
            if (fmt0 == HapTextureFormat_YCoCg_DXT5) f->ps = texCount == 2 ? PSKind::YCoCgAlpha : PSKind::YCoCg;
            else f->ps = PSKind::RGBA;
            consecutiveErrors = 0;
            pushFrame(std::move(f));
            firstPushed = true;
            m_state = State::Playing;
            continue;
        }

        r = avcodec_send_packet(g.dec, g.pkt);
        av_packet_unref(g.pkt);
        if (r < 0 && r != AVERROR(EAGAIN)) {
            if (++consecutiveErrors > 100) { fail("Erros de descodificação repetidos"); return false; }
            continue;
        }
        consecutiveErrors = 0;
        drainDecoder();
        if (m_state.load() == State::Error) return false;
    }
    clearQueue();
    return true;
}

// ---------------------------------------------------------------------------
// render thread
// ---------------------------------------------------------------------------
void VideoPlayer::update(double now) {
    if (m_state.load() == State::Error) return;

    std::unique_ptr<Frame> show;
    std::vector<std::unique_ptr<Frame>> dropped;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // descartar frames de antes de um restart
        const int gen = m_gen.load();
        while (!m_queue.empty() && m_queue.front()->gen != gen) {
            dropped.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        double fps = m_info.fps > 0 ? m_info.fps : 50;
        double fd = 1.0 / fps;
        if (m_chase) {
            // modo escravo: o playhead é o tempo pedido (timecode)
            double target = m_chaseT;
            const double dur = m_info.duration;
            if (target < 0) target = 0;
            if (dur > 0 && m_loop.load() != LoopMode::Loop) target = std::min(target, std::max(0.0, dur - fd));
            double lo = m_hasFrame ? m_shownT : -1, hi = lo;
            if (!m_queue.empty()) { lo = m_hasFrame ? std::min(lo, m_queue.front()->t) : m_queue.front()->t; hi = std::max(hi, m_queue.back()->t); }
            bool waiting = m_seekSentT >= 0 && now - m_seekSentAt < 1.0 && std::fabs(target - m_seekSentT) < 0.5;
            bool behind = lo >= 0 && target < lo - fd * 2.5;            // recuou
            bool ahead = hi >= 0 ? target > hi + std::max(0.4, fd * 8) : target > 0.4;   // saltou para a frente
            if (!waiting && (behind || ahead)) {
                m_seekT = target;
                m_gen++;
                m_seekReq = true;
                m_seekSentT = target; m_seekSentAt = now;
            } else if (waiting && !m_queue.empty() && m_queue.front()->gen == m_gen.load()) m_seekSentT = -1;
            const int g2 = m_gen.load();
            while (!m_queue.empty() && m_queue.front()->gen != g2) { dropped.push_back(std::move(m_queue.front())); m_queue.pop_front(); }
            while (!m_queue.empty() && m_queue.front()->t <= target + fd * 0.5) {
                if (show) dropped.push_back(std::move(show));
                show = std::move(m_queue.front());
                m_queue.pop_front();
            }
            if (!show && !m_hasFrame && !m_queue.empty()) { show = std::move(m_queue.front()); m_queue.pop_front(); }
            m_playhead = target;
            m_started = true;
        } else {
        if (!m_started) {
            if (m_queue.empty()) return;
            // o relógio arranca quando o primeiro frame está pronto → sem saltos no início
            m_started = true;
            m_clockBase = now;
            m_clockT = m_queue.front()->t;
        }
        double speed = m_speed.load();
        double playhead = m_clockT + (now - m_clockBase) * speed;
        // tolerância de meio frame para evitar "jitter" com relógios quase iguais
        double tol = 0.5 * fd;
        while (!m_queue.empty() && m_queue.front()->t <= playhead + tol) {
            if (show) dropped.push_back(std::move(show));
            show = std::move(m_queue.front());
            m_queue.pop_front();
        }
        // se o descodificador ficar para trás, mostrar o primeiro disponível
        if (!show && !m_hasFrame && !m_queue.empty()) {
            show = std::move(m_queue.front());
            m_queue.pop_front();
        }
        m_playhead = playhead;
        }
    }
    m_cv.notify_all();

        for (auto& d : dropped) recycle(std::move(d));
    if (show) {
        upload(*show);
        m_shownT = show->t;
        recycle(std::move(show));
    }
}

void VideoPlayer::upload(Frame& f) {
    Gfx& G = gfx();
    ID3D11DeviceContext* ctx = G.ctx();

    if (f.hw) {
        auto* src = reinterpret_cast<ID3D11Texture2D*>(f.hw->data[0]);
        UINT index = (UINT)(intptr_t)f.hw->data[1];
        int w = f.w & ~1, h = f.h & ~1;
        if (!G.ensureVideoTexture(m_tex, w, h, f.fmt)) return;
        D3D11_BOX box{0, 0, 0, (UINT)w, (UINT)h, 1};
        {
            std::lock_guard<std::recursive_mutex> lk(G.ctxMutex());
            ctx->CopySubresourceRegion(m_tex.tex[0].Get(), 0, 0, 0, 0, src, index, &box);
        }
        bool bt709 = f.pitch[0] & 1u, full = f.pitch[0] & 2u;
        m_yuv = makeYuvMatrix(bt709, full);
        m_psKind = PSKind::YUV;
        m_path_ = DecodePath::Hardware;
        m_uvMax[0] = 1; m_uvMax[1] = 1;
        m_hasFrame = true;
        return;
    }

    if (f.fmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
        if (!G.ensureVideoTexture(m_tex, f.w, f.h, f.fmt, DXGI_FORMAT_UNKNOWN, true)) return;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(m_tex.tex[0].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            const uint8_t* s = f.buf[0].data();
            uint8_t* d = (uint8_t*)m.pData;
            if (m.RowPitch == f.pitch[0]) memcpy(d, s, (size_t)f.pitch[0] * f.h);
            else for (int y = 0; y < f.h; ++y) memcpy(d + (size_t)y * m.RowPitch, s + (size_t)y * f.pitch[0], f.pitch[0]);
            ctx->Unmap(m_tex.tex[0].Get(), 0);
        }
        m_psKind = PSKind::RGBA;
        m_path_ = DecodePath::Software;
        m_uvMax[0] = 1; m_uvMax[1] = 1;
        m_hasFrame = true;
        return;
    }

    // HAP: blocos BC enviados diretamente (sem descompressão na GPU)
    const int w4 = (f.w + 3) & ~3, h4 = (f.h + 3) & ~3;
    if (!G.ensureVideoTexture(m_tex, w4, h4, f.fmt, f.fmt2)) return;
    ctx->UpdateSubresource(m_tex.tex[0].Get(), 0, nullptr, f.buf[0].data(), f.pitch[0], 0);
    if (f.fmt2 != DXGI_FORMAT_UNKNOWN && m_tex.tex[1])
        ctx->UpdateSubresource(m_tex.tex[1].Get(), 0, nullptr, f.buf[1].data(), f.pitch[1], 0);
    m_psKind = f.ps;
    m_path_ = DecodePath::Hap;
    m_uvMax[0] = (float)f.w / w4;
    m_uvMax[1] = (float)f.h / h4;
    m_hasFrame = true;
}

} // namespace ps
