#include "video/Thumbnail.h"
#include "core/ThreadPool.h"
#include "core/Log.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace ps {

ThumbnailCache& ThumbnailCache::get() { static ThumbnailCache c; return c; }

static void generate(const std::string& path, ThumbnailCache::Entry& e) {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* fr = nullptr;
    SwsContext* sws = nullptr;
    bool ok = false;
    do {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) break;
        avformat_find_stream_info(fmt, nullptr);
        int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vs < 0) break;
        AVStream* st = fmt->streams[vs];
        e.info.width = st->codecpar->width;
        e.info.height = st->codecpar->height;
        double fps = av_q2d(st->avg_frame_rate);
        if (fps <= 0) fps = av_q2d(st->r_frame_rate);
        e.info.fps = fps;
        e.info.duration = st->duration != AV_NOPTS_VALUE ? st->duration * av_q2d(st->time_base)
                        : (fmt->duration > 0 ? fmt->duration / (double)AV_TIME_BASE : 0);
        const AVCodecDescriptor* d = avcodec_descriptor_get(st->codecpar->codec_id);
        e.info.codec = d ? d->name : "?";
        if (st->codecpar->codec_id == AV_CODEC_ID_HAP) e.info.codec = "hap";
        {
            int as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vs, nullptr, 0);
            if (as >= 0) {
                e.info.audioChannels = fmt->streams[as]->codecpar->ch_layout.nb_channels;
                e.info.audioRate = fmt->streams[as]->codecpar->sample_rate;
                const AVCodecDescriptor* ad = avcodec_descriptor_get(fmt->streams[as]->codecpar->codec_id);
                e.info.audioCodec = ad ? ad->name : "?";
            }
        }

        const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
        if (!c) break;
        dec = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(dec, st->codecpar);
        dec->thread_count = 2;
        if (avcodec_open2(dec, c, nullptr) < 0) break;
        pkt = av_packet_alloc();
        fr = av_frame_alloc();
        // o 1º frame costuma ser preto (fade in): saltar para ~10% do vídeo, no máximo 3 s
        if (e.info.duration > 0.5) {
            double t = std::min(3.0, e.info.duration * 0.1);
            int64_t ts = (int64_t)(t / av_q2d(st->time_base)) + (st->start_time != AV_NOPTS_VALUE ? st->start_time : 0);
            if (av_seek_frame(fmt, vs, ts, AVSEEK_FLAG_BACKWARD) >= 0) avcodec_flush_buffers(dec);
        }
        bool got = false;
        for (int tries = 0; tries < 300 && !got; ++tries) {
            int r = av_read_frame(fmt, pkt);
            if (r < 0) { avcodec_send_packet(dec, nullptr); }
            else if (pkt->stream_index != vs) { av_packet_unref(pkt); continue; }
            else { avcodec_send_packet(dec, pkt); av_packet_unref(pkt); }
            if (avcodec_receive_frame(dec, fr) == 0) got = true;
            if (r < 0 && !got) break;
        }
        if (!got) break;
        int tw = 192;
        int th = fr->width > 0 ? std::max(1, tw * fr->height / fr->width) : 108;
        if (th > 192) { th = 192; tw = std::max(1, th * fr->width / fr->height); }
        sws = sws_getContext(fr->width, fr->height, (AVPixelFormat)fr->format, tw, th, AV_PIX_FMT_RGBA, SWS_AREA, nullptr, nullptr, nullptr);
        if (!sws) break;
        e.rgba.resize((size_t)tw * th * 4);
        uint8_t* dst[4] = {e.rgba.data(), nullptr, nullptr, nullptr};
        int ds[4] = {tw * 4, 0, 0, 0};
        sws_scale(sws, fr->data, fr->linesize, 0, fr->height, dst, ds);
        e.w = tw; e.h = th;
        ok = true;
    } while (false);
    if (sws) sws_freeContext(sws);
    if (fr) av_frame_free(&fr);
    if (pkt) av_packet_free(&pkt);
    if (dec) avcodec_free_context(&dec);
    if (fmt) avformat_close_input(&fmt);
    if (!ok) PS_WARN("Miniatura indisponível: %s", path.c_str());
}

std::shared_ptr<ThumbnailCache::Entry> ThumbnailCache::request(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_map.find(path);
    if (it != m_map.end()) return it->second;
    auto e = std::make_shared<Entry>();
    m_map[path] = e;
    ThreadPool::get().submit([path, e] {
        auto tmp = std::make_shared<Entry>();
        try { generate(path, *tmp); } catch (...) {}
        // publicar o resultado de uma vez (a entrada é lida na thread de render)
        std::lock_guard<std::mutex> lk(ThumbnailCache::get().m_mtx);
        e->rgba = std::move(tmp->rgba);
        e->w = tmp->w; e->h = tmp->h;
        e->info = tmp->info;
        e->state = e->rgba.empty() ? Entry::State::Failed : Entry::State::Ready;
    });
    return e;
}

void ThumbnailCache::poll() {
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto& [path, e] : m_map) {
        if (e->state != Entry::State::Ready || e->uploaded) continue;
        e->uploaded = true;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = e->w; td.Height = e->h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{e->rgba.data(), (UINT)e->w * 4, 0};
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(gfx().dev()->CreateTexture2D(&td, &sd, &tex)))
            gfx().dev()->CreateShaderResourceView(tex.Get(), nullptr, &e->srv);
        e->rgba.clear();
        e->rgba.shrink_to_fit();
    }
}

void ThumbnailCache::clear() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_map.clear();
}

} // namespace ps
