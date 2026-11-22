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

static bool beginPanel(const char* pt, const char* id, bool* open) {
    if (!*open) return false;
    if (!ImGui::Begin(winTitle(pt, id).c_str(), open)) { ImGui::End(); return false; }
    ImGui::PushItemWidth(-110);
    return true;
}
static void endPanel() { ImGui::PopItemWidth(); ImGui::End(); }

// ---------------------------------------------------------------------------
static void compositionPanel(App& app) {
    if (!beginPanel("Composição", "props_comp", &app.ui.panel.propComp)) return;
    Composition& c = app.project.comp;
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.75f, 1), "%s (%dx%d)", c.name.c_str(), c.width, c.height);
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Definições..."))) app.ui.showCompSettings = true;
    if (ImGui::CollapsingHeader(T("Composição##sec"), ImGuiTreeNodeFlags_DefaultOpen))
        param(app, T("Master"), &c.master, 0, 1, 1, "%.2f");
    if (ImGui::CollapsingHeader(T("Efeitos##comp"), ImGuiTreeNodeFlags_DefaultOpen))
        effectsEditor(app, c.effects, "compfx");
    endPanel();
}

static void layerPanel(App& app) {
    if (!beginPanel("Layer", "props_layer", &app.ui.panel.propLayer)) return;
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
    if (!beginPanel("Coluna", "props_col", &app.ui.panel.propColumn)) return;
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
static void clipPanel(App& app) {
    if (!beginPanel("Clip", "props_clip", &app.ui.panel.propClip)) return;
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
