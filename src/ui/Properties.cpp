// Painéis de propriedades (como no Resolume): Composição · Layer · Coluna · Clip
// e o painel Efeitos (arrastar um efeito para um clip, layer ou composição).
#include "ui/UI.h"
#include "core/I18n.h"
#include "app/App.h"
#include "video/Thumbnail.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ps {

void colorSwatches(App& app, uint32_t& target);   // ClipGrid.cpp

// valor com - / +: arrastar, duplo clique escreve, botão direito repõe (estilo Resolume)
static bool param(App& app, const char* label, float* v, float lo, float hi, float def, const char* fmt) {
    ImGui::PushID(label);
    bool ch = false;
    float w = ImGui::CalcItemWidth();
    ImGui::SetNextItemWidth(std::max(60.f, w - 52));
    ch |= valueSlider(app, "##v", v, lo, hi, def, fmt);
    float step = (hi - lo) / 100.f;
    ImGui::SameLine(0, 2);
    if (ImGui::SmallButton("-")) { app.checkpoint(); *v = std::clamp(*v - step, lo, hi); ch = true; }
    ImGui::SameLine(0, 2);
    if (ImGui::SmallButton("+")) { app.checkpoint(); *v = std::clamp(*v + step, lo, hi); ch = true; }
    ImGui::SameLine(0, 6);
    ImGui::TextUnformatted(label);
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { app.checkpoint(); *v = def; ch = true; app.markDirty(); }
    ImGui::PopID();
    return ch;
}

// lista de efeitos editável (usada no clip, na layer e na composição)
void effectsEditor(App& app, std::vector<Effect>& fx, const char* id) {
    ImGui::PushID(id);
    int remove = -1, up = -1;
    for (int i = 0; i < (int)fx.size(); ++i) {
        Effect& e = fx[i];
        const EffectDef* d = findEffect(e.type);
        if (!d) continue;
        ImGui::PushID(i);
        bool open = ImGui::CollapsingHeader(T(d->name), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 92);
        if (ImGui::Checkbox("##on", &e.enabled)) app.checkpoint();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Ligar/desligar efeito"));
        ImGui::SameLine(0, 2);
        if (ImGui::SmallButton("^") && i > 0) up = i;
        ImGui::SameLine(0, 2);
        if (ImGui::SmallButton("x")) remove = i;
        if (open) {
            ImGui::Indent(6);
            ImGui::TextDisabled("%s", T(d->help));
            for (int k = 0; k < (int)d->params.size() && k < (int)e.p.size(); ++k) {
                const ParamDef& pd = d->params[k];
                if (pd.color && k + 2 < (int)e.p.size()) {
                    if (ImGui::ColorEdit3(T(pd.name), &e.p[k], ImGuiColorEditFlags_NoInputs)) {}
                    undoOnActivate(app);
                    k += 2;
                } else if (pd.toggle) {
                    bool b = e.p[k] > 0.5f;
                    if (ImGui::Checkbox(T(pd.name), &b)) { app.checkpoint(); e.p[k] = b ? 1.f : 0.f; }
                } else {
                    param(app, T(pd.name), &e.p[k], pd.min, pd.max, pd.def, "%.2f");
                }
            }
            ImGui::Unindent(6);
        }
        ImGui::PopID();
    }
    if (remove >= 0) { app.checkpoint(); fx.erase(fx.begin() + remove); }
    if (up > 0) { app.checkpoint(); std::swap(fx[up], fx[up - 1]); }

    // adicionar: lista ou arrastar do painel Efeitos para esta zona
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##add", T("+ Adicionar efeito"))) {
        for (auto& d : effectCatalog())
            if (ImGui::Selectable(T(d.name))) { app.checkpoint(); fx.push_back(makeEffect(d.id)); }
        ImGui::EndCombo();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PS_FX")) {
            int idx = *(const int*)pl->Data;
            if (idx >= 0 && idx < (int)effectCatalog().size()) { app.checkpoint(); fx.push_back(makeEffect(effectCatalog()[idx].id)); }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
}

// cada painel tem a sua cor (faixa no topo + cabeçalhos das secções) para não se confundirem:
// Composição roxo, Layer azul, Coluna âmbar, Clip verde-água
static bool beginPanel(const char* pt, const char* id, bool* open, ImU32 accent) {
    if (!*open) return false;
    if (!ImGui::Begin(winTitle(pt, id).c_str(), open)) { ImGui::End(); return false; }
    ImVec4 a = ImGui::ColorConvertU32ToFloat4(accent);
    auto mul = [&](float k) { return ImVec4(std::min(1.f, a.x * k), std::min(1.f, a.y * k), std::min(1.f, a.z * k), 1); };
    ImGui::PushStyleColor(ImGuiCol_Header, mul(1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, mul(1.18f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, mul(1.35f));
    // faixa com o nome do painel
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight() + 2;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32((int)(a.x * 255 * 0.55f), (int)(a.y * 255 * 0.55f), (int)(a.z * 255 * 0.55f), 255), 3);
    dl->AddRectFilled(p0, ImVec2(p0.x + 4, p0.y + h), accent, 2);
    std::string title = T(pt);
    for (size_t i = 0; i < title.size(); ++i) {
        unsigned char ch = (unsigned char)title[i];
        if (ch >= 'a' && ch <= 'z') title[i] = (char)(ch - 32);
        else if (ch == 0xC3 && i + 1 < title.size()) {   // à-þ → À-Þ (UTF-8)
            unsigned char n = (unsigned char)title[i + 1];
            if (n >= 0xA0 && n <= 0xBE && n != 0xB7) title[i + 1] = (char)(n - 0x20);
            ++i;
        }
    }
    dl->AddText(ImVec2(p0.x + 12, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(255, 255, 255, 235), title.c_str());
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PushItemWidth(-110);
    return true;
}
static void endPanel() { ImGui::PopItemWidth(); ImGui::PopStyleColor(3); ImGui::End(); }

// ---------------------------------------------------------------------------
static void compositionPanel(App& app) {
    if (!beginPanel("Composição", "props_comp", &app.ui.panel.propComp, IM_COL32(124, 78, 170, 255))) return;
    Composition& c = app.project.comp;
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.75f, 1), "%s (%dx%d)", c.name.c_str(), c.width, c.height);
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Definições..."))) app.ui.showCompSettings = true;
    if (ImGui::CollapsingHeader(T("Composição##sec"), ImGuiTreeNodeFlags_DefaultOpen))
        param(app, T("Master"), &c.master, 0, 1, 1, "%.2f");
    if (ImGui::CollapsingHeader(T("Transição entre clips"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* tr[] = {T("Corte"), T("Dissolve")};
        if (ImGui::Combo(T("Tipo##trans"), &c.transition, tr, 2)) { app.checkpoint(); app.markDirty(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Dissolve: ao mudar de coluna ou de clip, a imagem nova aparece por cima da anterior durante este tempo.\nUma layer com Fade próprio (> 0) usa o seu."));
        if (c.transition == 1) param(app, T("Duração (s)"), &c.transitionTime, 0, 10, 0.5f, "%.2f s");
    }
    if (ImGui::CollapsingHeader(T("Efeitos##comp"), ImGuiTreeNodeFlags_DefaultOpen))
        effectsEditor(app, c.effects, "compfx");
    endPanel();
}

static void layerPanel(App& app) {
    if (!beginPanel("Layer", "props_layer", &app.ui.panel.propLayer, IM_COL32(52, 100, 178, 255))) return;
    Project& p = app.project;
    int li = app.ui.selLayer;
    if (li < 0 || li >= (int)p.comp.layers.size()) { ImGui::TextDisabled("%s", T("Seleciona uma layer.")); endPanel(); return; }
    Layer& l = p.comp.layers[li];
    inputTextStd(T("Nome##layer"), l.name);
    undoOnActivate(app);
    ImGui::TextUnformatted(T("Cor"));
    ImGui::SameLine();
    colorSwatches(app, l.color);

    if (ImGui::CollapsingHeader(T("Vídeo##layer"), ImGuiTreeNodeFlags_DefaultOpen)) {
        param(app, T("Opacidade"), &l.opacity, 0, 1, 1, "%.2f");
        const char* blends[] = {"Normal", "Add", "Multiply", "Screen"};
        int bm = (int)l.blend;
        if (ImGui::Combo(T("Mistura"), &bm, blends, 4)) { app.checkpoint(); l.blend = (BlendMode)bm; }
        param(app, T("Fade (s)"), &l.fadeTime, 0, 10, 0, "%.2f");
        if (ImGui::Checkbox(T("Ignorar disparo de coluna"), &l.ignoreColumn)) app.checkpoint();
        if (ImGui::Checkbox(T("Bloquear conteúdo"), &l.locked)) app.checkpoint();
    }
    if (ImGui::CollapsingHeader(T("Transformar##layer"), ImGuiTreeNodeFlags_DefaultOpen)) {
        param(app, T("Posição X"), &l.posX, -(float)p.comp.width, (float)p.comp.width, 0, "%.0f");
        param(app, T("Posição Y"), &l.posY, -(float)p.comp.height, (float)p.comp.height, 0, "%.0f");
        param(app, T("Escala"), &l.scale, 0.01f, 4, 1, "%.3f");
        param(app, T("Rotação"), &l.rotation, -180, 180, 0, "%.1f°");
    }
    if (ImGui::CollapsingHeader(T("Região na composição"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox(T("Usar região"), &l.useRegion)) app.checkpoint();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Os vídeos desta layer encaixam neste retângulo da composição\n(ex.: a zona de um ledwall). Desligado = composição inteira."));
        if (l.useRegion) {
            rectEditor(app, "region", l.region, (float)p.comp.width, (float)p.comp.height);
            if (ImGui::BeginCombo(T("Copiar de"), T("slice..."))) {
                for (auto& s : p.screens)
                    for (auto& sl : s.slices) {
                        std::string n = s.name + " / " + sl.name + "##" + std::to_string(sl.id);
                        if (ImGui::Selectable(n.c_str())) { app.checkpoint(); l.region = sl.input; }
                    }
                ImGui::EndCombo();
            }
        }
    }
    if (ImGui::CollapsingHeader(T("Efeitos##layer"), ImGuiTreeNodeFlags_DefaultOpen))
        effectsEditor(app, l.effects, "layerfx");
    endPanel();
}

static void columnPanel(App& app) {
    if (!beginPanel("Coluna", "props_col", &app.ui.panel.propColumn, IM_COL32(170, 118, 34, 255))) return;
    Project& p = app.project;
    int c = app.ui.selColumnProp;
    if (c < 0 || c >= (int)p.comp.columns.size()) { ImGui::TextDisabled("%s", T("Clica no cabeçalho de uma coluna.")); endPanel(); return; }
    Column& col = p.comp.columns[c];
    inputTextStd(T("Nome##col"), col.name);
    undoOnActivate(app);
    ImGui::TextUnformatted(T("Cor"));
    ImGui::SameLine();
    colorSwatches(app, col.color);
    ImGui::Spacing();
    if (ImGui::Button(T("Pôr a coluna no ar"), ImVec2(-1, 0))) app.comp.triggerColumn(p, c);
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(T("Timecode##col"), col.tcTrigger ? ImGuiTreeNodeFlags_DefaultOpen : 0)) columnTimecodeControls(app, col);
    ImGui::Spacing();
    ImGui::SeparatorText(T("Clips nesta coluna"));
    int n = 0;
    for (auto& l : p.comp.layers) {
        if (c >= (int)l.clips.size() || l.clips[c].empty()) continue;
        ++n;
        bool live = l.activeColumn == c;
        ImGui::TextColored(live ? ImVec4(0.4f, 0.9f, 0.5f, 1) : ImVec4(0.75f, 0.75f, 0.78f, 1), "%s%s:  %s", live ? "● " : "", l.name.c_str(), l.clips[c].name.c_str());
    }
    if (!n) ImGui::TextDisabled("%s", T("(vazia)"));
    endPanel();
}

void clipTransformControls(App& app, Clip& c);

static std::string fmtSec(double s) {
    if (s < 0) s = 0;
    char b[32];
    int m = (int)(s / 60);
    snprintf(b, sizeof(b), "%02d:%05.2f", m, s - m * 60);
    return b;
}

// Transporte (como o Resolume): timeline com cabeça de leitura, Trim In / Trim Out, pausa e saltar
static void clipTransport(App& app, Layer& l, int ci, Clip& c, double fileDur) {
    Project& p = app.project;
    VideoPlayer* v = nullptr;
    if (l.activeColumn == ci) v = app.comp.activePlayer(l.id);
    else if (app.comp.previewLayerId() == l.id && app.comp.previewColumn() == ci && app.comp.previewStack() == p.comp.activeStack) v = app.comp.previewPlayer();
    if (v && v->info().duration > 0) fileDur = v->info().duration;
    if (fileDur <= 0) { ImGui::TextDisabled("%s", T("A ler a duração...")); return; }
    const double dur = fileDur;
    double in = std::clamp(c.trimIn, 0.0, dur), out = c.trimOut > 0 ? std::clamp(c.trimOut, in + 0.04, dur) : dur;
    double len = std::max(0.04, out - in);
    double pos = -1;
    if (v) {
        double ph = std::max(0.0, v->position());
        pos = in + (c.loop == LoopMode::Loop ? std::fmod(ph, len) : std::min(ph, len));
    }
    // textos
    ImGui::TextDisabled("In %s", fmtSec(in).c_str());
    std::string mid = pos >= 0 ? fmtSec(pos - in) + " / " + fmtSec(len) : fmtSec(len);
    float aw = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(std::max(80.f, aw * 0.5f - ImGui::CalcTextSize(mid.c_str()).x * 0.5f));
    ImGui::TextColored(pos >= 0 ? ImVec4(0.25f, 0.88f, 0.75f, 1) : ImVec4(0.75f, 0.76f, 0.8f, 1), "%s", mid.c_str());
    std::string outS = "Out " + fmtSec(out);
    ImGui::SameLine(std::max(160.f, aw - ImGui::CalcTextSize(outS.c_str()).x));
    ImGui::TextDisabled("%s", outS.c_str());

    // barra
    const float w = std::max(60.f, aw), h = 30, pad = 7;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##timeline", ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto X = [&](double t) { return p0.x + pad + (w - 2 * pad) * (float)(t / dur); };
    auto Tm = [&](float x) { return std::clamp((double)(x - p0.x - pad) / (w - 2 * pad) * dur, 0.0, dur); };
    float ty0 = p0.y + 12, ty1 = p0.y + 20;
    dl->AddRectFilled(ImVec2(p0.x + pad, ty0), ImVec2(p0.x + w - pad, ty1), IM_COL32(30, 31, 35, 255), 3);
    dl->AddRectFilled(ImVec2(X(in), ty0), ImVec2(X(out), ty1), IM_COL32(40, 110, 96, 255), 3);
    if (pos >= 0) dl->AddRectFilled(ImVec2(X(in), ty0), ImVec2(X(pos), ty1), IM_COL32(64, 200, 170, 255), 3);
    // pegas Trim In / Trim Out
    auto handle = [&](double t, bool isIn) {
        float x = X(t);
        ImU32 hc = IM_COL32(64, 224, 192, 255);
        dl->AddRectFilled(ImVec2(x - 2, ty0 - 6), ImVec2(x + 2, ty1 + 6), hc);
        if (isIn) dl->AddTriangleFilled(ImVec2(x, ty1 + 6), ImVec2(x + 7, ty1 + 6), ImVec2(x, ty1 + 1), hc);
        else dl->AddTriangleFilled(ImVec2(x, ty1 + 6), ImVec2(x - 7, ty1 + 6), ImVec2(x, ty1 + 1), hc);
    };
    handle(in, true); handle(out, false);
    if (pos >= 0) {
        float x = X(pos);
        dl->AddTriangleFilled(ImVec2(x - 6, p0.y + 1), ImVec2(x + 6, p0.y + 1), ImVec2(x, p0.y + 10), IM_COL32(240, 240, 245, 255));
        dl->AddLine(ImVec2(x, p0.y + 8), ImVec2(x, ty1), IM_COL32(240, 240, 245, 255), 2);
    }
    // interação
    static int drag = 0;   // 1 In, 2 Out, 3 cabeça de leitura
    static double lastSeek = -1, lastSeekAt = 0;
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated()) {
        float mx = io.MousePos.x;
        if (std::fabs(mx - X(in)) < 9) drag = 1;
        else if (std::fabs(mx - X(out)) < 9) drag = 2;
        else drag = 3;
        if (drag != 3) app.checkpoint();
        lastSeek = -1;
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
        float mx = io.MousePos.x;
        if (std::fabs(mx - X(in)) < 9 || std::fabs(mx - X(out)) < 9) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::SetTooltip("%s", T("Arrasta as pegas para o Trim In / Trim Out; clica ou arrasta na barra para saltar (clip no ar ou em preview)."));
    }
    if (ImGui::IsItemActive()) {
        double t = Tm(io.MousePos.x);
        if (drag == 1) { c.trimIn = std::clamp(t, 0.0, out - 0.1); app.markDirty(); }
        else if (drag == 2) { double o = std::clamp(t, in + 0.1, dur); c.trimOut = o >= dur - 0.001 ? 0.0 : o; app.markDirty(); }
        else if (drag == 3 && v) {
            double target = std::clamp(t, in, out) - in;
            double now = ImGui::GetTime();
            if (std::fabs(target - lastSeek) > 0.02 && now - lastSeekAt > 0.04) { v->seek(target); lastSeek = target; lastSeekAt = now; }
        }
    }
    if (ImGui::IsItemDeactivated()) drag = 0;

    // botões
    ImGui::BeginDisabled(!v);
    if (ImGui::Button(T("Recomeçar"))) v->seek(0);
    ImGui::SameLine();
    bool paused = v && v->paused();
    if (ImGui::Button(paused ? T("Play") : T("Pausa"), ImVec2(70, 0))) v->setPaused(!paused);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Repor trim"))) { app.checkpoint(); c.trimIn = 0; c.trimOut = 0; app.markDirty(); }
    ImGui::SameLine();
    ImGui::TextDisabled(T("Duração %s"), fmtSec(len / std::max(0.01f, c.speed)).c_str());
    float fin = (float)in, fout = (float)out;
    if (param(app, T("Trim In (s)"), &fin, 0, (float)dur, 0, "%.2f s")) c.trimIn = std::clamp((double)fin, 0.0, out - 0.1);
    if (param(app, T("Trim Out (s)"), &fout, 0, (float)dur, (float)dur, "%.2f s")) { double o = std::clamp((double)fout, c.trimIn + 0.1, dur); c.trimOut = o >= dur - 0.001 ? 0.0 : o; }
}

static void clipPanel(App& app) {
    if (!beginPanel("Clip", "props_clip", &app.ui.panel.propClip, IM_COL32(40, 128, 110, 255))) return;
    Project& p = app.project;
    int li = app.ui.selLayer, ci = app.ui.selColumn;
    if (li < 0 || li >= (int)p.comp.layers.size() || ci < 0 || ci >= (int)p.comp.layers[li].clips.size() || p.comp.layers[li].clips[ci].empty()) {
        ImGui::TextDisabled("%s", T("Seleciona um clip na grelha."));
        endPanel();
        return;
    }
    Layer& l = p.comp.layers[li];
    Clip& c = l.clips[ci];
    inputTextStd(T("Nome##clip"), c.name);
    undoOnActivate(app);
    ImGui::TextWrapped("%s", c.path.c_str());
    auto th = ThumbnailCache::get().request(c.path);
    if (th->state == ThumbnailCache::Entry::State::Ready) {
        const MediaInfo& mi = th->info;
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.8f, 1), "%dx%d  ·  %.2f fps  ·  %.1f s  ·  %s", mi.width, mi.height, mi.fps, mi.duration, mi.codec.c_str());
        if (l.useRegion && (std::abs(l.region.w - mi.width) > 0.5f || std::abs(l.region.h - mi.height) > 0.5f))
            ImGui::TextColored(ImVec4(1, 0.7f, 0.25f, 1), T("Atenção: o vídeo não tem o tamanho da região (%.0fx%.0f)"), l.region.w, l.region.h);
        if (mi.codec != "hap" && (mi.width * mi.height >= 3840 * 2160 || mi.width >= 7680))
            ImGui::TextColored(ImVec4(1, 0.7f, 0.25f, 1), "%s", T("Dica: para vídeos muito grandes usa HAP / HAP Q."));
    }
    if (ImGui::SmallButton(T("Copiar definições"))) copyClipSettings(app, c);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Repetição, velocidade, opacidade, encaixe, transformar, efeitos e modo de timecode (Ctrl+Shift+C)."));
    ImGui::SameLine();
    ImGui::BeginDisabled(!app.ui.hasClipSettings);
    if (ImGui::SmallButton(T("Colar definições"))) pasteClipSettings(app);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", T("Cola nos clips selecionados (Ctrl/Shift+clique para escolher vários) — Ctrl+Shift+V."));
    if (ImGui::CollapsingHeader(T("Transporte##clip"), ImGuiTreeNodeFlags_DefaultOpen))
        clipTransport(app, l, ci, c, th->state == ThumbnailCache::Entry::State::Ready ? th->info.duration : 0);
    if (VideoPlayer* v = app.comp.activePlayer(l.id); v && l.activeColumn == ci) {
        MediaInfo mi = v->info();
        ImGui::TextDisabled(T("A tocar: %s  ·  fila %d  ·  descartados %d"), mi.codec.c_str(), v->queued(), v->dropped());
        if (v->state() == VideoPlayer::State::Error) ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s", v->error().c_str());
    }
    if (ImGui::CollapsingHeader(T("Reprodução##clip"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* loops[] = {T("Loop"), T("Tocar uma vez e parar no fim"), T("Tocar uma vez e limpar")};
        int lm = (int)c.loop;
        if (ImGui::Combo(T("Repetição"), &lm, loops, 3)) { app.checkpoint(); c.loop = (LoopMode)lm; }
        param(app, T("Velocidade"), &c.speed, 0, 4, 1, "%.2fx");
        param(app, T("Opacidade"), &c.opacity, 0, 1, 1, "%.2f");
    }
    if (ImGui::CollapsingHeader(T("Transformar##clip"), ImGuiTreeNodeFlags_DefaultOpen)) clipTransformControls(app, c);
    if (ImGui::CollapsingHeader(T("Timecode##clip"), c.tcMode != TcMode::Off ? ImGuiTreeNodeFlags_DefaultOpen : 0)) clipTimecodeControls(app, c);
    if (ImGui::CollapsingHeader(T("Efeitos##clip"), ImGuiTreeNodeFlags_DefaultOpen))
        effectsEditor(app, c.effects, "clipfx");
    endPanel();
}

void clipTransformControls(App& app, Clip& c) {
    Project& p = app.project;
    {
        const char* fits[] = {T("Esticar"), T("Caber (com barras)"), T("Preencher (cortar)"), T("Tamanho nativo")};
        int fm = (int)c.fit;
        if (ImGui::Combo(T("Encaixe"), &fm, fits, 4)) { app.checkpoint(); c.fit = (FitMode)fm; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Esticar, Caber e Preencher só ficam diferentes quando a proporção do vídeo é diferente da composição (ex.: vídeo 16:9 numa composição 48:9). Tamanho nativo usa os píxeis reais do vídeo."));
        param(app, T("Posição X"), &c.offsetX, -(float)p.comp.width, (float)p.comp.width, 0, "%.0f");
        param(app, T("Posição Y"), &c.offsetY, -(float)p.comp.height, (float)p.comp.height, 0, "%.0f");
        param(app, T("Escala"), &c.scale, 0.01f, 4, 1, "%.3f");
        param(app, T("Esticar X"), &c.scaleX, 0.05f, 4, 1, "%.3f");
        param(app, T("Esticar Y"), &c.scaleY, 0.05f, 4, 1, "%.3f");
        param(app, T("Rotação"), &c.rotation, -180, 180, 0, "%.1f°");
        if (ImGui::Checkbox(T("Espelhar H"), &c.flipH)) app.checkpoint();
        ImGui::SameLine();
        if (ImGui::Checkbox(T("Espelhar V"), &c.flipV)) app.checkpoint();
        if (ImGui::Button(T("Repor transformação"))) { app.checkpoint(); c.scale = c.scaleX = c.scaleY = 1; c.offsetX = c.offsetY = 0; c.rotation = 0; c.flipH = c.flipV = false; }
    }
}

static void effectsBrowser(App& app) {
    if (!app.ui.panel.effects) return;
    if (!ImGui::Begin(winTitle("Efeitos", "effects").c_str(), &app.ui.panel.effects)) { ImGui::End(); return; }
    static char filter[64] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##f", T("Procurar..."), filter, sizeof(filter));
    ImGui::TextDisabled("%s", T("Arrasta um efeito para um clip, o nome de uma layer ou um painel de propriedades."));
    ImGui::Separator();
    const auto& cat = effectCatalog();
    for (int i = 0; i < (int)cat.size(); ++i) {
        std::string nm = T(cat[i].name);
        if (filter[0]) {
            std::string a = nm, b = filter;
            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
            std::transform(b.begin(), b.end(), b.begin(), ::tolower);
            if (a.find(b) == std::string::npos) continue;
        }
        ImGui::PushID(i);
        ImGui::Selectable(nm.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(cat[i].help));
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("PS_FX", &i, sizeof(int));
            ImGui::Text("%s", nm.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
    }
    ImGui::End();
}

void drawProperties(App& app) {
    compositionPanel(app);
    layerPanel(app);
    columnPanel(app);
    clipPanel(app);
    effectsBrowser(app);
}

} // namespace ps
