#pragma once
#include "core/Model.h"
#include "gfx/Gfx.h"
#include "video/VideoPlayer.h"
#include <map>
#include <memory>

namespace ps {

// Estado em execução de cada layer (players ativos e crossfade)
struct LayerRuntime {
    std::unique_ptr<VideoPlayer> cur, prev;
    Clip   curClip, prevClip;
    int    column = -1;
    double fadeStart = 0, fadeDur = 0;
};

class Compositor {
public:
    void trigger(Project& p, int layerIndex, int column);
    void triggerColumn(Project& p, int column);
    void stopLayer(Project& p, int layerIndex);
    void stopAll(Project& p);
    void restartLayer(int layerIndex, Project& p);

    // thread de render, uma vez por frame
    void update(Project& p, double now);
    void render(Project& p);

    const RenderTarget& compRT() const { return m_comp; }
    const RenderTarget* screenRT(uint32_t screenId) const;
    LayerRuntime* runtime(uint32_t layerId);
    VideoPlayer* activePlayer(uint32_t layerId);

    // desenha um ecrã (todas as slices) num alvo qualquer
    void drawScreen(const Project& p, const Screen& s, ID3D11RenderTargetView* rtv, int w, int h);

    void shutdown();

private:
    void drawLayer(const Project& p, const Layer& l, LayerRuntime& rt, double now);
    void drawClip(const Project& p, const Layer& l, const Clip& c, VideoPlayer& v, float opacity);
    ID3D11ShaderResourceView* sourceFor(uint32_t layerId) const;

    RenderTarget m_comp;
    std::map<uint32_t, RenderTarget> m_layerRT;     // layers usadas como fonte de slices
    std::map<uint32_t, RenderTarget> m_screenRT;
    std::map<uint32_t, LayerRuntime> m_layers;
    double m_now = 0;
};

} // namespace ps
