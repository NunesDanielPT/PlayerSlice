#include "render/Compositor.h"
#include "core/Timecode.h"
#include "core/Log.h"
#include "core/Geometry.h"
#include "core/Effects.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <set>

namespace ps {

LayerRuntime* Compositor::runtime(uint32_t id) {
    auto it = m_layers.find(id);
    return it == m_layers.end() ? nullptr : &it->second;
}

VideoPlayer* Compositor::activePlayer(uint32_t id) {
    auto* r = runtime(id);
    return r ? r->cur.get() : nullptr;
}

const RenderTarget* Compositor::screenRT(uint32_t id) const {
    auto it = m_screenRT.find(id);
    return it == m_screenRT.end() || !it->second.valid() ? nullptr : &it->second;
}

const Clip* Compositor::activeClip(uint32_t id) const {
    auto it = m_layers.find(id);
    return it != m_layers.end() && it->second.cur ? &it->second.curClip : nullptr;
}

void Compositor::trigger(Project& p, int li, int column) {
    if (li < 0 || li >= (int)p.comp.layers.size()) return;
    Layer& l = p.comp.layers[li];
    if (column < 0 || column >= (int)l.clips.size()) return;
    const Clip& c = l.clips[column];
    if (c.empty()) return;
    LayerRuntime& rt = m_layers[l.id];

    // clicar no clip que já está a tocar → recomeça
    if (rt.cur && rt.column == column && rt.stack == p.comp.activeStack && rt.curClip.path == c.path) {
        rt.cur->restart();
        return;
    }
    if (rt.prev) VideoPlayer::destroyAsync(std::move(rt.prev));
    rt.prev = std::move(rt.cur);
    rt.prevClip = rt.curClip;
    rt.cur = std::make_unique<VideoPlayer>(c.path, c.loop, c.speed, c.trimIn, c.trimOut);
    rt.curClip = c;
    rt.column = column;
    rt.stack = p.comp.activeStack;
    rt.fadeDur = l.fadeTime > 0 ? l.fadeTime : (p.comp.transition == 1 ? p.comp.transitionTime : 0.f);
    rt.fadeStart = -1; // começa quando o novo clip tiver o 1º frame
    l.activeColumn = column;
}

void Compositor::previewClip(Project& p, int li, int column) {
    if (li < 0 || li >= (int)p.comp.layers.size()) return;
    const Layer& l = p.comp.layers[li];
    if (column < 0 || column >= (int)l.clips.size() || l.clips[column].empty()) return;
    const Clip& c = l.clips[column];
    if (m_prev && m_prevLayerId == l.id && m_prevCol == column && m_prevStack == p.comp.activeStack && m_prevClip.path == c.path) { m_prev->restart(); return; }
    VideoPlayer::destroyAsync(std::move(m_prev));
    m_prev = std::make_unique<VideoPlayer>(c.path, c.loop == LoopMode::PlayOnceClear ? LoopMode::PlayOnceHold : c.loop, c.speed, c.trimIn, c.trimOut);
    m_prevClip = c;
    m_prevLayerId = l.id;
    m_prevCol = column;
    m_prevStack = p.comp.activeStack;
}

void Compositor::clearPreview() {
    VideoPlayer::destroyAsync(std::move(m_prev));
    m_prevLayerId = 0;
    m_prevCol = -1;
}

void Compositor::takePreview(Project& p) {
    if (!m_prev) return;
    int li = p.layerIndex(m_prevLayerId);
    if (li < 0) { clearPreview(); return; }
    Layer& l = p.comp.layers[li];
    LayerRuntime& rt = m_layers[l.id];
    if (rt.prev) VideoPlayer::destroyAsync(std::move(rt.prev));
    rt.prev = std::move(rt.cur);
    rt.prevClip = rt.curClip;
    // o player do preview passa para o programa: continua do mesmo frame, sem recarregar
    m_prev->setLoop(m_prevClip.loop);
    rt.cur = std::move(m_prev);
    rt.curClip = m_prevClip;
    rt.column = m_prevCol;
    rt.stack = m_prevStack;
    rt.fadeDur = l.fadeTime > 0 ? l.fadeTime : (p.comp.transition == 1 ? p.comp.transitionTime : 0.f);
    rt.fadeStart = -1;
    l.activeColumn = m_prevStack == p.comp.activeStack ? m_prevCol : -1;
    m_prevLayerId = 0;
    m_prevCol = -1;
}

const RenderTarget* Compositor::previewScreenRT(uint32_t id) const {
    auto it = m_prevScreenRT.find(id);
    return it == m_prevScreenRT.end() || !it->second.valid() ? nullptr : &it->second;
}

void Compositor::triggerColumn(Project& p, int column) {
    m_lastColumn = column;
    for (int i = 0; i < (int)p.comp.layers.size(); ++i) {
        Layer& l = p.comp.layers[i];
        if (l.ignoreColumn) continue;
        if (column < (int)l.clips.size() && !l.clips[column].empty()) trigger(p, i, column);
        else {
            // célula vazia: com dissolve, a layer desvanece para nada em vez de cortar
            float fd = l.fadeTime > 0 ? l.fadeTime : (p.comp.transition == 1 ? p.comp.transitionTime : 0.f);
            auto it = m_layers.find(l.id);
            if (fd > 0 && it != m_layers.end() && it->second.cur) {
                LayerRuntime& rt = it->second;
                if (rt.prev) VideoPlayer::destroyAsync(std::move(rt.prev));
                rt.prev = std::move(rt.cur);
                rt.prevClip = rt.curClip;
                rt.column = -1;
                rt.fadeDur = fd;
                rt.fadeStart = nowSeconds();
                l.activeColumn = -1; l.playing = false;
            } else stopLayer(p, i);
        }
    }
}

void Compositor::stopLayer(Project& p, int li) {
    if (li < 0 || li >= (int)p.comp.layers.size()) return;
    Layer& l = p.comp.layers[li];
    auto it = m_layers.find(l.id);
    if (it != m_layers.end()) {
        VideoPlayer::destroyAsync(std::move(it->second.cur));
        VideoPlayer::destroyAsync(std::move(it->second.prev));
        m_layers.erase(it);
    }
    l.activeColumn = -1; l.playing = false;
}

void Compositor::stopAll(Project& p) {
    m_lastColumn = -1;
    for (int i = 0; i < (int)p.comp.layers.size(); ++i) stopLayer(p, i);
}

void Compositor::restartLayer(int li, Project& p) {
    if (li < 0 || li >= (int)p.comp.layers.size()) return;
    if (auto* r = runtime(p.comp.layers[li].id)) if (r->cur) r->cur->restart();
}

void Compositor::remapColumns(Project& p, const std::function<int(int)>& map) {
    for (auto& l : p.comp.layers) {
        auto it = m_layers.find(l.id);
        if (it == m_layers.end()) continue;
        LayerRuntime& rt = it->second;
        if (rt.column < 0 || rt.stack != p.comp.activeStack) continue;   // só a stack visível muda
        int nc = map(rt.column);
        if (nc < 0) {
            VideoPlayer::destroyAsync(std::move(rt.cur));
            VideoPlayer::destroyAsync(std::move(rt.prev));
            m_layers.erase(it);
            l.activeColumn = -1;
        } else {
            rt.column = nc;
            l.activeColumn = nc;
        }
    }
    if (m_prev && m_prevCol >= 0 && m_prevStack == p.comp.activeStack) {
        int nc = map(m_prevCol);
        if (nc < 0) clearPreview(); else m_prevCol = nc;
    }
}

void Compositor::update(Project& p, double now) {
    m_now = now;
    // limpar runtimes de layers apagadas
    std::set<uint32_t> ids;
    for (auto& l : p.comp.layers) ids.insert(l.id);
    for (auto it = m_layers.begin(); it != m_layers.end();) {
        if (!ids.count(it->first)) {
            VideoPlayer::destroyAsync(std::move(it->second.cur));
            VideoPlayer::destroyAsync(std::move(it->second.prev));
            it = m_layers.erase(it);
        } else ++it;
    }

    if (m_prev) {
        if (!ids.count(m_prevLayerId)) clearPreview();
        else {
            int li = p.layerIndex(m_prevLayerId);
            if (li >= 0 && m_prevStack == p.comp.activeStack && m_prevCol >= 0 && m_prevCol < (int)p.comp.layers[li].clips.size() &&
                p.comp.layers[li].clips[m_prevCol].path == m_prevClip.path) {
                const Clip& c = p.comp.layers[li].clips[m_prevCol];
                if (c.speed != m_prevClip.speed) m_prev->setSpeed(c.speed);
                if (c.trimIn != m_prevClip.trimIn || c.trimOut != m_prevClip.trimOut) m_prev->setTrim(c.trimIn, c.trimOut);
                m_prevClip = c;
            }
            if (tcFollows(m_prevClip.tcMode)) { TcStatus ts = Timecode::get().status(); if (ts.valid) m_prev->chase((ts.seconds - m_prevClip.tcStart) * m_prevClip.speed); }
            m_prev->update(now);
        }
    }

    for (auto& l : p.comp.layers) {
        auto it = m_layers.find(l.id);
        if (it == m_layers.end()) { l.activeColumn = -1; l.playing = false; continue; }
        LayerRuntime& rt = it->second;
        // propriedades editadas em direto
        if (rt.stack == p.comp.activeStack && rt.column >= 0 && rt.column < (int)l.clips.size() && l.clips[rt.column].path == rt.curClip.path) {
            const Clip& c = l.clips[rt.column];
            if (c.speed != rt.curClip.speed && rt.cur) rt.cur->setSpeed(c.speed);
            if (c.loop != rt.curClip.loop && rt.cur) rt.cur->setLoop(c.loop);
            if ((c.trimIn != rt.curClip.trimIn || c.trimOut != rt.curClip.trimOut) && rt.cur) rt.cur->setTrim(c.trimIn, c.trimOut);
            rt.curClip = c;
        }
        if (rt.cur && tcFollows(rt.curClip.tcMode)) {
            TcStatus ts = Timecode::get().status();
            if (ts.valid) rt.cur->chase((ts.seconds - rt.curClip.tcStart) * rt.curClip.speed);
        }
        if (rt.cur) rt.cur->update(now);
        if (rt.prev) rt.prev->update(now);

        if (rt.cur && rt.fadeStart < 0 && (rt.cur->hasFrame() || rt.cur->state() == VideoPlayer::State::Error))
            rt.fadeStart = now;
        // fim do crossfade (ou corte seco assim que o novo tem imagem)
        if (rt.prev && rt.fadeStart >= 0 && now - rt.fadeStart >= rt.fadeDur)
            VideoPlayer::destroyAsync(std::move(rt.prev));

        if (rt.cur && rt.cur->finished() && rt.curClip.loop == LoopMode::PlayOnceClear) {
            VideoPlayer::destroyAsync(std::move(rt.cur));
            rt.column = -1;
        }
        l.playing = rt.cur != nullptr;
        l.activeColumn = rt.cur && rt.stack == p.comp.activeStack ? rt.column : -1;
    }
}

void Compositor::clipCorners(const Project& p, const Layer& l, const Clip& c, float vw, float vh, float px[4], float py[4]) {
    const int cw = p.comp.width, ch = p.comp.height;
    Rect R = l.useRegion ? l.region : Rect{0, 0, (float)cw, (float)ch};
    vw = std::max(1.f, vw); vh = std::max(1.f, vh);
    float dw = R.w, dh = R.h;
    switch (c.fit) {
    case FitMode::Stretch: break;
    case FitMode::Fit:  { float s = std::min(R.w / vw, R.h / vh); dw = vw * s; dh = vh * s; } break;
    case FitMode::Fill: { float s = std::max(R.w / vw, R.h / vh); dw = vw * s; dh = vh * s; } break;
    case FitMode::Native: dw = vw; dh = vh; break;
    }
    dw *= c.scale * c.scaleX; dh *= c.scale * c.scaleY;
    float rcx = R.x + R.w * 0.5f, rcy = R.y + R.h * 0.5f;
    float cx = rcx + c.offsetX, cy = rcy + c.offsetY;
    const float hw = dw * 0.5f, hh = dh * 0.5f;
    const float lx[4] = {-hw, hw, -hw, hw}, ly[4] = {-hh, -hh, hh, hh};
    const float ca = std::cos(c.rotation * 3.14159265f / 180.f), sa = std::sin(c.rotation * 3.14159265f / 180.f);
    const float la = std::cos(l.rotation * 3.14159265f / 180.f), lsn = std::sin(l.rotation * 3.14159265f / 180.f);
    for (int i = 0; i < 4; ++i) {
        float x = cx + lx[i] * ca - ly[i] * sa, y = cy + lx[i] * sa + ly[i] * ca;
        float ox = (x - rcx) * l.scale, oy = (y - rcy) * l.scale;
        px[i] = rcx + ox * la - oy * lsn + l.posX;
        py[i] = rcy + ox * lsn + oy * la + l.posY;
    }
}

void Compositor::drawClip(const Project& p, const Layer& l, const Clip& c, VideoPlayer& v, float opacity) {
    if (!v.hasFrame() || opacity <= 0.001f) return;
    const VideoTexture& t = v.texture();
    if (!t.valid()) return;
    const int cw = p.comp.width, ch = p.comp.height;
    Rect R = l.useRegion ? l.region : Rect{0, 0, (float)cw, (float)ch};
    if (R.w <= 0 || R.h <= 0) return;
    float vw = (float)std::max(1, v.width()), vh = (float)std::max(1, v.height());

    QuadParams q;
    float px[4], py[4];
    clipCorners(p, l, c, vw, vh, px, py);
    for (int i = 0; i < 4; ++i) {
        q.pos[i][0] = px[i] / cw * 2.f - 1.f;
        q.pos[i][1] = 1.f - py[i] / ch * 2.f;
    }
    float u0 = 0, u1 = v.uvMaxX(), v0 = 0, v1 = v.uvMaxY();
    if (c.flipH) std::swap(u0, u1);
    if (c.flipV) std::swap(v0, v1);
    q.uv[0][0] = u0; q.uv[0][1] = v0;
    q.uv[1][0] = u1; q.uv[1][1] = v0;
    q.uv[2][0] = u0; q.uv[2][1] = v1;
    q.uv[3][0] = u1; q.uv[3][1] = v1;
    q.srv0 = t.srv[0].Get();
    q.srv1 = t.srv[1].Get();
    q.ps = PSKind::Video;
    switch (v.psKind()) {
    case PSKind::YUV: q.kind = 1; break;
    case PSKind::YCoCg: q.kind = 2; break;
    case PSKind::YCoCgAlpha: q.kind = 3; break;
    default: q.kind = 0; break;
    }
    q.yuv = v.yuv();
    q.vid[0] = v.uvMaxX(); q.vid[1] = v.uvMaxY(); q.vid[2] = vw; q.vid[3] = vh;
    FxParams fa = aggregateFx(c.effects, m_now), fb = aggregateFx(l.effects, m_now);
    memcpy(q.fxA, fa.v, sizeof(q.fxA));
    memcpy(q.fxB, fb.v, sizeof(q.fxB));
    q.opacity = opacity * fa.opacityMul * fb.opacityMul;
    if (q.opacity <= 0.001f) return;
    switch (l.blend) {
    case BlendMode::Normal: q.blend = Blend::Alpha; break;
    case BlendMode::Add: q.blend = Blend::Add; break;
    case BlendMode::Multiply: q.blend = Blend::Multiply; break;
    case BlendMode::Screen: q.blend = Blend::Screen; break;
    }
    // vídeo do tamanho exato, sem rotação nem efeitos de UV → amostragem por pontos (píxel perfeito no LED)
    bool noRot = std::fabs(c.rotation) < 0.001f && std::fabs(l.rotation) < 0.001f;
    float sw = px[1] - px[0], sh = py[2] - py[0];
    q.pointSample = noRot && std::fabs(std::fabs(sw) - vw) < 0.01f && std::fabs(std::fabs(sh) - vh) < 0.01f &&
                    std::fabs(px[0] - std::round(px[0])) < 0.01f && std::fabs(py[0] - std::round(py[0])) < 0.01f &&
                    fa.v[14] <= 1 && fb.v[14] <= 1;

    Gfx& G = gfx();
    if (l.useRegion) G.setScissor((int)std::floor(R.x), (int)std::floor(R.y), (int)std::ceil(R.w), (int)std::ceil(R.h));
    G.drawQuad(q);
    G.clearScissor();
}

void Compositor::drawLayer(const Project& p, const Layer& l, LayerRuntime& rt, double now) {
    float k = 1.f;
    if (rt.prev && rt.fadeStart >= 0 && rt.fadeDur > 0) k = std::clamp(float((now - rt.fadeStart) / rt.fadeDur), 0.f, 1.f);
    else if (rt.prev && rt.fadeStart < 0) k = 0.f;
    // sem clip novo (desvanecer para nada): o anterior vai desaparecendo
    float kp = (!rt.cur && rt.prev) ? 1.f - k : 1.f;
    if (rt.prev && kp > 0) drawClip(p, l, rt.prevClip, *rt.prev, l.opacity * rt.prevClip.opacity * kp);
    if (rt.cur && k > 0) drawClip(p, l, rt.curClip, *rt.cur, l.opacity * rt.curClip.opacity * k);
}

ID3D11ShaderResourceView* Compositor::sourceFor(uint32_t layerId) const {
    if (layerId) {
        auto it = m_layerRT.find(layerId);
        if (it != m_layerRT.end() && it->second.valid()) return it->second.srv.Get();
    }
    return m_comp.srv.Get();
}

void Compositor::render(Project& p) {
    Gfx& G = gfx();
    ++m_frameNo;
    const int cw = p.comp.width, ch = p.comp.height;
    const DXGI_FORMAT fmt = p.comp.colorDepth == 16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!m_comp.valid() || m_comp.w != cw || m_comp.h != ch || m_compFmt != fmt) {
        if (!G.createRT(m_comp, cw, ch, fmt)) return;
        m_compRaw = {};
        m_compFmt = fmt;
        PS_INFO("Composição %dx%d %dbpc", cw, ch, p.comp.colorDepth);
    }
    // com efeitos na composição: desenhar primeiro em m_compRaw e aplicar os efeitos em m_comp
    FxParams compFx = aggregateFx(p.comp.effects, m_now);
    const bool useFx = !compFx.identity;
    if (useFx && (!m_compRaw.valid() || m_compRaw.w != cw || m_compRaw.h != ch)) G.createRT(m_compRaw, cw, ch, fmt);
    RenderTarget& target = (useFx && m_compRaw.valid()) ? m_compRaw : m_comp;

    bool anySolo = std::any_of(p.comp.layers.begin(), p.comp.layers.end(), [](const Layer& l) { return l.solo; });
    auto visible = [&](const Layer& l) { return !l.bypass && (!anySolo || l.solo); };

    // 1) composição: da última layer (fundo) para a primeira (topo)
    G.setTarget(target.rtv.Get(), cw, ch);
    G.clear(target.rtv.Get(), 0, 0, 0, 1);
    for (int i = (int)p.comp.layers.size() - 1; i >= 0; --i) {
        const Layer& l = p.comp.layers[i];
        if (!visible(l)) continue;
        auto it = m_layers.find(l.id);
        if (it != m_layers.end()) drawLayer(p, l, it->second, m_now);
    }
    if (&target == &m_compRaw) {
        G.setTarget(m_comp.rtv.Get(), cw, ch);
        G.clear(m_comp.rtv.Get(), 0, 0, 0, 1);
        QuadParams q;
        Gfx::rectToNdc(0, 0, (float)cw, (float)ch, cw, ch, q.pos);
        q.srv0 = m_compRaw.srv.Get();
        q.ps = PSKind::Fx;
        q.blend = Blend::Opaque;
        memcpy(q.fxB, compFx.v, sizeof(q.fxB));
        q.vid[2] = (float)cw; q.vid[3] = (float)ch;
        q.opacity = compFx.opacityMul;
        G.drawQuad(q);
    }

    // 2) layers usadas diretamente como fonte de slices
    std::set<uint32_t> needed;
    for (auto& s : p.screens)
        for (auto& sl : s.slices)
            if (sl.enabled && sl.sourceLayer && p.layerIndex(sl.sourceLayer) >= 0)
                needed.insert(sl.sourceLayer);
    for (auto it = m_layerRT.begin(); it != m_layerRT.end();)
        it = needed.count(it->first) ? std::next(it) : m_layerRT.erase(it);
    for (int i = 0; i < (int)p.comp.layers.size(); ++i) {
        const Layer& l = p.comp.layers[i];
        if (!needed.count(l.id)) continue;
        RenderTarget& rt = m_layerRT[l.id];
        if (!rt.valid() || rt.w != cw || rt.h != ch) if (!G.createRT(rt, cw, ch, fmt)) continue;
        G.setTarget(rt.rtv.Get(), cw, ch);
        G.clear(rt.rtv.Get(), 0, 0, 0, 0);
        auto it = m_layers.find(l.id);
        if (it != m_layers.end() && visible(l)) drawLayer(p, l, it->second, m_now);
    }

    // 2b) preview: só o clip em preview, na região da sua layer
    if (!m_prevRT.valid() || m_prevRT.w != cw || m_prevRT.h != ch) G.createRT(m_prevRT, cw, ch, fmt);
    if (m_prevRT.valid()) {
        G.setTarget(m_prevRT.rtv.Get(), cw, ch);
        G.clear(m_prevRT.rtv.Get(), 0, 0, 0, 1);
        int li = m_prev ? p.layerIndex(m_prevLayerId) : -1;
        if (li >= 0) {
            Layer l = p.comp.layers[li];
            l.blend = BlendMode::Normal;
            drawClip(p, l, m_prevClip, *m_prev, 1.f);
        }
    }

    // 3) cada ecrã num render target próprio (usado nas saídas e no editor)
    std::set<uint32_t> screens;
    for (auto& s : p.screens) {
        screens.insert(s.id);
        int w = std::clamp(s.width, 16, 16384), h = std::clamp(s.height, 16, 16384);
        RenderTarget& rt = m_screenRT[s.id];
        if (!rt.valid() || rt.w != w || rt.h != h) if (!G.createRT(rt, w, h)) continue;
        drawScreen(p, s, rt.rtv.Get(), w, h);
    }
    for (auto it = m_screenRT.begin(); it != m_screenRT.end();)
        it = screens.count(it->first) ? std::next(it) : m_screenRT.erase(it);

    // 4) ecrãs vistos a partir do preview (só os pedidos pela interface no frame anterior)
    m_prevScreenActive = std::move(m_prevScreenReq);
    m_prevScreenReq.clear();
    for (auto it = m_prevScreenRT.begin(); it != m_prevScreenRT.end();)
        it = m_prevScreenActive.count(it->first) ? std::next(it) : m_prevScreenRT.erase(it);
    for (uint32_t id : m_prevScreenActive) {
        const Screen* s = nullptr;
        for (auto& x : p.screens) if (x.id == id) s = &x;
        if (!s) continue;
        int w = std::clamp(s->width, 16, 16384), h = std::clamp(s->height, 16, 16384);
        RenderTarget& rt = m_prevScreenRT[id];
        if (!rt.valid() || rt.w != w || rt.h != h) if (!G.createRT(rt, w, h)) continue;
        drawScreen(p, *s, rt.rtv.Get(), w, h, true);
    }
}

static void unpackColor(uint32_t c, float out[4]) {
    out[0] = (c & 0xFF) / 255.f;
    out[1] = ((c >> 8) & 0xFF) / 255.f;
    out[2] = ((c >> 16) & 0xFF) / 255.f;
    out[3] = 1.f;
}

void Compositor::drawScreen(const Project& p, const Screen& s, ID3D11RenderTargetView* rtv, int w, int h, bool fromPreview) {
    Gfx& G = gfx();
    G.setTarget(rtv, w, h);
    G.clear(rtv, 0, 0, 0, 1);
    const float sx = (float)w / std::max(1, s.width), sy = (float)h / std::max(1, s.height);
    const float cw = (float)p.comp.width, ch = (float)p.comp.height;

    for (const Slice& sl : s.slices) {
        if (!sl.enabled) continue;
        float out[4][2], in[4][2];
        sl.outputQuad(out);
        sl.inputQuad(in);
        for (auto& pt : out) { pt[0] *= sx; pt[1] *= sy; }
        if (!quadIsConvex(out)) continue;   // quadrilátero torcido: não desenhar

        QuadParams q;
        // vértices pela ordem do triangle strip: TL, TR, BL, BR
        const int order[4] = {0, 1, 3, 2};
        for (int i = 0; i < 4; ++i) {
            q.pos[i][0] = out[order[i]][0] / w * 2.f - 1.f;
            q.pos[i][1] = 1.f - out[order[i]][1] / h * 2.f;
        }
        // píxel do alvo → (s,t) e (s,t) → UV na composição
        Mat3 ho = inverse(squareToQuad(out));
        Mat3 hi = squareToQuad(in);
        for (int k = 0; k < 3; ++k) { hi.m[k] /= cw; hi.m[3 + k] /= ch; }
        for (int k = 0; k < 9; ++k) { q.hOutInv[k] = (float)ho.m[k]; q.hIn[k] = (float)hi.m[k]; }

        q.opacity = sl.opacity * s.opacity * p.comp.master;
        q.brightness = sl.brightness + s.brightness;
        q.adjust[0] = s.red; q.adjust[1] = s.green; q.adjust[2] = s.blue; q.adjust[3] = s.contrast;
        q.blend = Blend::Alpha;
        if (s.testPattern) {
            q.ps = PSKind::WarpTest;
            unpackColor(sl.color, q.color);
            q.vid[0] = (float)std::fmod(m_now, 3600.0);   // animação do test pattern
            q.vid[1] = (float)(m_frameNo % 100000);
            float ew = std::hypot(out[1][0] - out[0][0], out[1][1] - out[0][1]);
            float eh = std::hypot(out[3][0] - out[0][0], out[3][1] - out[0][1]);
            q.aspect = eh > 0 ? ew / eh : 1.f;
        } else {
            q.ps = PSKind::Warp;
            q.srv0 = fromPreview ? m_prevRT.srv.Get() : sourceFor(sl.sourceLayer);
            // recorte com o mesmo tamanho, sem rotação nem pontos → píxel perfeito
            q.pointSample = !sl.inWarp && !sl.outWarp && std::fabs(sl.input.w - sl.outW) < 0.01f &&
                            std::fabs(sl.input.h - sl.outH) < 0.01f && std::fmod(std::fabs(sl.rotation), 360.f) < 0.01f &&
                            sx == 1.f && sy == 1.f;
        }
        G.drawQuad(q);
    }
}

void Compositor::shutdown() {
    m_prev.reset();
    m_prevRT = {};
    m_prevScreenRT.clear();
    for (auto& [id, rt] : m_layers) {
        rt.cur.reset();
        rt.prev.reset();
    }
    m_layers.clear();
    m_layerRT.clear();
    m_screenRT.clear();
    m_comp = {};
}

} // namespace ps
