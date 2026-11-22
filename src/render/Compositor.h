#pragma once
#include "core/Model.h"
#include "gfx/Gfx.h"
#include "video/VideoPlayer.h"
#include <map>
#include <memory>
#include <set>
#include <functional>

namespace ps {

// Estado em execução de cada layer (players ativos e crossfade)
struct LayerRuntime {
    std::unique_ptr<VideoPlayer> cur, prev;
    Clip   curClip, prevClip;
    int    column = -1;
    int    stack = 0;           // stack de onde veio o clip (continua a tocar ao mudar de stack)
    double fadeStart = 0, fadeDur = 0;
};

class Compositor {
public:
    void trigger(Project& p, int layerIndex, int column);
    void triggerColumn(Project& p, int column);
    void stopLayer(Project& p, int layerIndex);
    void stopAll(Project& p);
    void restartLayer(int layerIndex, Project& p);
    // colunas movidas/apagadas/inseridas: map(antiga) = nova, ou -1 se foi apagada
    void remapColumns(Project& p, const std::function<int(int)>& map);

    // PREVIEW: um clip à parte, fora das saídas, para ver antes de pôr no ar
    void previewClip(Project& p, int layerIndex, int column);
    void clearPreview();
    void takePreview(Project& p);              // passa o preview para o programa (sem recomeçar)
    bool hasPreview() const { return m_prev != nullptr; }
    uint32_t previewLayerId() const { return m_prevLayerId; }
    int previewColumn() const { return m_prevCol; }
    int previewStack() const { return m_prevStack; }
    const Clip* activeClip(uint32_t layerId) const;   // clip no programa (de qualquer stack)
    const Clip& previewClipInfo() const { return m_prevClip; }
    VideoPlayer* previewPlayer() const { return m_prev.get(); }
    const RenderTarget& previewRT() const { return m_prevRT; }
    // ecrãs vistos a partir do preview (renderizados a pedido da interface)
    void requestPreviewScreen(uint32_t screenId) { m_prevScreenReq.insert(screenId); }
    const RenderTarget* previewScreenRT(uint32_t screenId) const;
    ID3D11ShaderResourceView* programSource(uint32_t layerId) const { return sourceFor(layerId); }

    // thread de render, uma vez por frame
    void update(Project& p, double now);
    void render(Project& p);

    const RenderTarget& compRT() const { return m_comp; }
    const RenderTarget* screenRT(uint32_t screenId) const;
    LayerRuntime* runtime(uint32_t layerId);
    VideoPlayer* activePlayer(uint32_t layerId);

    // desenha um ecrã (todas as slices) num alvo qualquer
    void drawScreen(const Project& p, const Screen& s, ID3D11RenderTargetView* rtv, int w, int h, bool fromPreview = false);

    void shutdown();

    // cantos do clip na composição (TL, TR, BL, BR) — usado no desenho e na moldura de transformação
    static void clipCorners(const Project& p, const Layer& l, const Clip& c, float vw, float vh, float px[4], float py[4]);

private:
    void drawLayer(const Project& p, const Layer& l, LayerRuntime& rt, double now);
    void drawClip(const Project& p, const Layer& l, const Clip& c, VideoPlayer& v, float opacity);
    ID3D11ShaderResourceView* sourceFor(uint32_t layerId) const;

    RenderTarget m_comp, m_compRaw;
    DXGI_FORMAT  m_compFmt = DXGI_FORMAT_UNKNOWN;
    std::map<uint32_t, RenderTarget> m_layerRT;     // layers usadas como fonte de slices
    std::map<uint32_t, RenderTarget> m_screenRT;
    std::map<uint32_t, LayerRuntime> m_layers;
    double m_now = 0;
    uint64_t m_frameNo = 0;

    std::unique_ptr<VideoPlayer> m_prev;
    Clip     m_prevClip;
    uint32_t m_prevLayerId = 0;
    int      m_prevCol = -1;
    int      m_prevStack = 0;
    RenderTarget m_prevRT;
    std::map<uint32_t, RenderTarget> m_prevScreenRT;
    std::set<uint32_t> m_prevScreenReq, m_prevScreenActive;
};

} // namespace ps
