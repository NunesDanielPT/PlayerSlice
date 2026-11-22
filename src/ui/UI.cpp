#include "ui/UI.h"
#include "app/App.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "video/Thumbnail.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdio>
#include <shellapi.h>

namespace ps {

std::string winTitle(const char* pt, const char* id) { return std::string(T(pt)) + "###" + id; }

static void buildDefaultLayout(ImGuiID dock) {
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->WorkSize);
    ImGuiID right, left, bottomLeft;
    ImGui::DockBuilderSplitNode(dock, ImGuiDir_Right, 0.22f, &right, &left);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.34f, &bottomLeft, &left);
    ImGuiID bl1, bl2;
    ImGui::DockBuilderSplitNode(bottomLeft, ImGuiDir_Left, 0.5f, &bl1, &bl2);
    ImGui::DockBuilderDockWindow("###clips", left);
    ImGui::DockBuilderDockWindow("###slices", left);
    ImGui::DockBuilderDockWindow("###comp", bl1);
    ImGui::DockBuilderDockWindow("###monitor", bl2);
    ImGui::DockBuilderDockWindow("###log", bl2);
    ImGui::DockBuilderDockWindow("###props", right);
    ImGui::DockBuilderFinish(dock);
}

static void fitImage(ImTextureID tex, float tw, float th, ImVec2 avail) {
    if (tw <= 0 || th <= 0 || avail.x <= 0 || avail.y <= 0) return;
    float s = std::min(avail.x / tw, avail.y / th);
    ImVec2 sz(tw * s, th * s);
    ImVec2 c = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(c.x + (avail.x - sz.x) * 0.5f, c.y + (avail.y - sz.y) * 0.5f));
    ImGui::Image(tex, sz);
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(p0, p1, IM_COL32(70, 72, 78, 255));
}

// ---------------------------------------------------------------------------
// Monitores PREVIEW / PROGRAMA, cada um com "Ver": composição, um ecrã ou uma slice
// ---------------------------------------------------------------------------
static std::string viewLabel(App& app, int mode, uint32_t id) {
    Project& p = app.project;
    if (mode == 1) if (Screen* s = p.findScreen(id)) return std::string(T("Ecrã: ")) + s->name;
    if (mode == 2) { Screen* owner = nullptr; if (Slice* sl = p.findSlice(id, &owner)) return std::string(T("Slice: ")) + owner->name + " / " + sl->name; }
    return T("Composição");
}

static void viewCombo(App& app, int idx) {
    Project& p = app.project;
    int& mode = app.ui.viewMode[idx];
    uint32_t& id = app.ui.viewId[idx];
    // se o ecrã/slice foi apagado, voltar à composição
    if ((mode == 1 && !p.findScreen(id)) || (mode == 2 && !p.findSlice(id))) { mode = 0; id = 0; }
    ImGui::SetNextItemWidth(std::min(260.f, ImGui::GetContentRegionAvail().x));
    std::string cur = viewLabel(app, mode, id);
    std::string cid = std::string("##view") + std::to_string(idx);
    if (ImGui::BeginCombo(cid.c_str(), cur.c_str())) {
        if (ImGui::Selectable(T("Composição"), mode == 0)) { mode = 0; id = 0; }
        if (!p.screens.empty()) ImGui::SeparatorText(T("Ecrãs (como saem nas saídas)"));
        for (auto& s : p.screens) {
            std::string l = s.name + "##vs" + std::to_string(s.id);
            if (ImGui::Selectable(l.c_str(), mode == 1 && id == s.id)) { mode = 1; id = s.id; }
        }
        bool any = false;
        for (auto& s : p.screens) if (!s.slices.empty()) any = true;
        if (any) ImGui::SeparatorText(T("Slices (só o recorte)"));
        for (auto& s : p.screens)
            for (auto& sl : s.slices) {
                std::string l = s.name + " / " + sl.name + "##vl" + std::to_string(sl.id);
                if (ImGui::Selectable(l.c_str(), mode == 2 && id == sl.id)) { mode = 2; id = sl.id; }
            }
        ImGui::EndCombo();
    }
}

// desenha o conteúdo do monitor idx (0 = preview, 1 = programa) na área dada
static void monitorImage(App& app, int idx, ImVec2 avail) {
    Project& p = app.project;
    const int mode = app.ui.viewMode[idx];
    const uint32_t id = app.ui.viewId[idx];
    const bool prev = idx == 0;
    if (mode == 0) {
        const RenderTarget& rt = prev ? app.comp.previewRT() : app.comp.compRT();
        if (rt.valid()) fitImage(texId(rt.srv.Get()), (float)rt.w, (float)rt.h, avail);
    } else if (mode == 1) {
        const RenderTarget* rt = nullptr;
        if (prev) { app.comp.requestPreviewScreen(id); rt = app.comp.previewScreenRT(id); }
        else rt = app.comp.screenRT(id);
        if (rt) fitImage(texId(rt->srv.Get()), (float)rt->w, (float)rt->h, avail);
    } else if (Slice* sl = p.findSlice(id)) {
        ID3D11ShaderResourceView* srv = prev ? app.comp.previewRT().srv.Get() : app.comp.programSource(sl->sourceLayer);
        if (!srv || sl->input.w <= 0 || sl->input.h <= 0) return;
        float cw = (float)p.comp.width, ch = (float)p.comp.height;
        ImVec2 uv0(sl->input.x / cw, sl->input.y / ch), uv1((sl->input.x + sl->input.w) / cw, (sl->input.y + sl->input.h) / ch);
        if (sl->flipH) std::swap(uv0.x, uv1.x);
        if (sl->flipV) std::swap(uv0.y, uv1.y);
        float s = std::min(avail.x / sl->input.w, avail.y / sl->input.h);
        ImVec2 sz(sl->input.w * s, sl->input.h * s);
        ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(c.x + (avail.x - sz.x) * 0.5f, c.y + (avail.y - sz.y) * 0.5f));
        ImGui::Image(texId(srv), sz, uv0, uv1);
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), sliceColorU32(sl->color, 255));
    }
}

static void monitorPanel(App& app, int idx, ImVec2 size) {
    ImGui::BeginChild(idx == 0 ? "##mon0" : "##mon1", size, ImGuiChildFlags_Borders);
    ImVec4 tagCol = idx == 0 ? ImVec4(0.95f, 0.67f, 0.16f, 1) : ImVec4(0.92f, 0.25f, 0.25f, 1);
    ImGui::TextColored(tagCol, "%s", idx == 0 ? T("PREVIEW") : T("PROGRAMA"));
    ImGui::SameLine();
    viewCombo(app, idx);
    if (idx == 0) {
        bool has = app.comp.hasPreview();
        ImGui::BeginDisabled(!has);
        std::string take = std::string(T("Enviar para programa")) + "  (" + bindingLabel(settings().keys[(int)Action::TakePreview]) + ")";
        if (ImGui::Button(take.c_str())) app.comp.takePreview(app.project);
        ImGui::SameLine();
        if (ImGui::Button(T("Limpar"))) app.comp.clearPreview();
        ImGui::EndDisabled();
        if (has) { ImGui::SameLine(); ImGui::TextDisabled("%s", app.comp.previewClipInfo().name.c_str()); }
    } else {
        int playing = 0;
        for (auto& l : app.project.comp.layers) if (l.activeColumn >= 0) playing++;
        ImGui::TextDisabled(T("%d layer(s) no ar  ·  %dx%d"), playing, app.project.comp.width, app.project.comp.height);
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Definições..."))) app.ui.showCompSettings = true;
    }
    monitorImage(app, idx, ImGui::GetContentRegionAvail());
    ImGui::EndChild();
}

static void compositionWindow(App& app) {
    if (!ImGui::Begin(winTitle("Preview / Programa", "comp").c_str())) { ImGui::End(); return; }
    ImVec2 a = ImGui::GetContentRegionAvail();
    bool sideBySide = a.x > a.y * 1.2f;
    float sp = ImGui::GetStyle().ItemSpacing.x;
    if (sideBySide) {
        ImVec2 half((a.x - sp) * 0.5f, a.y);
        monitorPanel(app, 0, half);
        ImGui::SameLine();
        monitorPanel(app, 1, half);
    } else {
        ImVec2 half(a.x, (a.y - sp) * 0.5f);
        monitorPanel(app, 0, half);
        monitorPanel(app, 1, half);
    }
    ImGui::End();
}

// botão grande ligar/desligar (usado na barra de estado e no monitor)
static void outputsButton(App& app, ImVec2 size) {
    bool on = app.anyLive();
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.75f, 0.15f, 0.15f, 1) : ImVec4(0.22f, 0.24f, 0.27f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? ImVec4(0.85f, 0.2f, 0.2f, 1) : ImVec4(0.3f, 0.33f, 0.37f, 1));
    std::string lbl = std::string(on ? T("SAÍDAS LIGADAS") : T("SAÍDAS DESLIGADAS")) + "  (" +
                      bindingLabel(settings().keys[(int)Action::ToggleOutputs]) + ")##outbtn";
    if (ImGui::Button(lbl.c_str(), size)) app.toggleOutputs();
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Liga/desliga os ecrãs marcados \"no atalho\" (se nenhum estiver marcado, todos)."));
}

static void outputsMonitor(App& app) {
    if (!ImGui::Begin(winTitle("Monitor de saídas", "monitor").c_str())) { ImGui::End(); return; }
    Project& p = app.project;
    outputsButton(app, ImVec2(260, 0));
    ImGui::SameLine();
    if (ImGui::Button(T("Ligar todas"))) app.setAllOutputs(true);
    ImGui::SameLine();
    if (ImGui::Button(T("Desligar todas"))) app.setAllOutputs(false);
    ImGui::Separator();
    if (p.screens.empty()) ImGui::TextDisabled("%s", T("Sem ecrãs. Cria um em \"Saídas e Slices\"."));
    float availW = ImGui::GetContentRegionAvail().x;
    int n = std::max(1, (int)p.screens.size());
    int cols = std::clamp((int)(availW / 230), 1, n);
    float w = (availW - (cols - 1) * 8) / cols;
    for (int i = 0; i < (int)p.screens.size(); ++i) {
        Screen& s = p.screens[i];
        if (i % cols) ImGui::SameLine(0, 8);
        ImGui::BeginGroup();
        ImGui::PushID((int)s.id);
        bool configured = s.device != ScreenDevice::Disabled;
        bool missing = s.device == ScreenDevice::Monitor &&
                       std::none_of(app.displays.begin(), app.displays.end(), [&](const DisplayInfo& d) { return d.key == s.monitorKey; });
        bool live = app.isLive(s.id);
        ImVec4 dot = missing ? ImVec4(1, 0.3f, 0.3f, 1) : live ? ImVec4(1, 0.25f, 0.25f, 1) : configured ? ImVec4(0.35f, 0.9f, 0.45f, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1);
        ImGui::TextColored(dot, "●");
        ImGui::SameLine();
        ImGui::Text("%s  %dx%d", s.name.c_str(), s.width, s.height);
        if (missing) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s", T("SEM SINAL")); }
        else if (live) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", T("NO AR")); }
        float h = w * s.height / std::max(1, s.width);
        if (const RenderTarget* rt = app.comp.screenRT(s.id)) {
            if (ImGui::ImageButton("##scr", texId(rt->srv.Get()), ImVec2(w - 8, h))) {
                app.ui.selScreen = s.id; app.ui.selSlice = 0; app.ui.focusEditor = true;
            }
        }
        if (configured) {
            bool on = live;
            if (ImGui::Checkbox(T("Ligada"), &on)) app.setOutput(s.id, on);
            ImGui::SameLine();
        }
        if (ImGui::Checkbox(T("No atalho"), &s.inToggle)) app.markDirty();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Este ecrã liga/desliga com o atalho das saídas."));
        ImGui::PopID();
        ImGui::EndGroup();
    }
    ImGui::End();
}

static void propertiesWindow(App& app) {
    if (!ImGui::Begin(winTitle("Propriedades", "props").c_str())) { ImGui::End(); return; }
    Project& p = app.project;
    int li = app.ui.selLayer;
    if (li < 0 || li >= (int)p.comp.layers.size()) { ImGui::TextDisabled("%s", T("Seleciona uma layer.")); ImGui::End(); return; }
    Layer& l = p.comp.layers[li];
    ImGui::PushItemWidth(-110);

    int ci = app.ui.selColumn;
    if (ci >= 0 && ci < (int)l.clips.size() && !l.clips[ci].empty()) {
        Clip& c = l.clips[ci];
        ImGui::SeparatorText(T("Clip"));
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
        const char* loops[] = {T("Loop"), T("Tocar uma vez e parar no fim"), T("Tocar uma vez e limpar")};
        int lm = (int)c.loop;
        if (ImGui::Combo(T("Repetição"), &lm, loops, 3)) { app.checkpoint(); c.loop = (LoopMode)lm; }
        ImGui::SliderFloat(T("Velocidade"), &c.speed, 0.f, 4.f, "%.2fx");
        undoOnActivate(app);
        ImGui::SliderFloat(T("Opacidade##clip"), &c.opacity, 0, 1, "%.2f");
        undoOnActivate(app);
        const char* fits[] = {T("Esticar"), T("Caber (com barras)"), T("Preencher (cortar)"), T("Tamanho nativo")};
        int fm = (int)c.fit;
        if (ImGui::Combo(T("Encaixe"), &fm, fits, 4)) { app.checkpoint(); c.fit = (FitMode)fm; }
        ImGui::DragFloat(T("Escala"), &c.scale, 0.005f, 0.01f, 20.f, "%.3f");
        undoOnActivate(app);
        float off[2] = {c.offsetX, c.offsetY};
        if (ImGui::DragFloat2(T("Deslocar"), off, 1, -16384, 16384, "%.0f")) { c.offsetX = off[0]; c.offsetY = off[1]; }
        undoOnActivate(app);
        if (ImGui::Button(T("Repor transformação"))) { app.checkpoint(); c.scale = 1; c.offsetX = c.offsetY = 0; }
    }

    ImGui::SeparatorText(T("Layer"));
    inputTextStd(T("Nome##layer"), l.name);
    undoOnActivate(app);
    ImGui::SliderFloat(T("Opacidade##layer"), &l.opacity, 0, 1, "%.2f");
    undoOnActivate(app);
    const char* blends[] = {"Normal", "Add", "Multiply", "Screen"};
    int bm = (int)l.blend;
    if (ImGui::Combo(T("Mistura"), &bm, blends, 4)) { app.checkpoint(); l.blend = (BlendMode)bm; }
    ImGui::SliderFloat(T("Fade (s)"), &l.fadeTime, 0, 10, "%.2f");
    undoOnActivate(app);

    ImGui::SeparatorText(T("Região na composição"));
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
        if (ci >= 0 && ci < (int)l.clips.size() && !l.clips[ci].empty()) {
            auto th = ThumbnailCache::get().request(l.clips[ci].path);
            if (th->state == ThumbnailCache::Entry::State::Ready && ImGui::Button(T("Tamanho = vídeo selecionado"))) {
                app.checkpoint(); l.region.w = (float)th->info.width; l.region.h = (float)th->info.height;
            }
        }
    }
    ImGui::PopItemWidth();
    ImGui::End();
}

// editor da resolução da composição (usado no popup e nas Definições → Vídeo)
void compositionSettingsBody(App& app, bool popup) {
    Composition& c = app.project.comp;
    static int wh[2] = {0, 0};
    static bool init = false;
    if (!init || (popup && ImGui::IsWindowAppearing())) { wh[0] = c.width; wh[1] = c.height; init = true; }
    ImGui::InputInt2(T("Resolução"), wh);
    const struct { const char* n; int w, h; } presets[] = {
        {"1920x1080", 1920, 1080}, {"2560x1440", 2560, 1440}, {"3840x2160 (4K)", 3840, 2160},
        {"5760x1080 (3x HD)", 5760, 1080}, {"7680x1080 (4x HD)", 7680, 1080}, {"7680x2160 (2x 4K)", 7680, 2160},
        {"7680x4320 (8K)", 7680, 4320}};
    for (int i = 0; i < 7; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(presets[i].n).x + 20) ImGui::NewLine();
        if (ImGui::SmallButton(presets[i].n)) { wh[0] = presets[i].w; wh[1] = presets[i].h; }
    }
    ImGui::DragFloat(T("FPS de referência"), &c.fps, 0.1f, 1, 240, "%.2f");
    ImGui::TextDisabled(T("Máximo da GPU: 16384x16384. VRAM: %zu MB"), gfx().vramMB());
    bool changed = wh[0] != c.width || wh[1] != c.height;
    ImGui::BeginDisabled(!changed);
    if (ImGui::Button(T("Aplicar"), ImVec2(120, 0))) {
        app.checkpoint();
        c.width = std::clamp(wh[0], 16, 16384);
        c.height = std::clamp(wh[1], 16, 16384);
        wh[0] = c.width; wh[1] = c.height;
        if (popup) ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (!popup && !changed && (wh[0] != c.width || wh[1] != c.height)) { wh[0] = c.width; wh[1] = c.height; }
}

static void compSettingsPopup(App& app) {
    std::string title = winTitle("Definições da composição", "compsettings");
    if (app.ui.showCompSettings) { ImGui::OpenPopup(title.c_str()); app.ui.showCompSettings = false; }
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushItemWidth(260);
        compositionSettingsBody(app, true);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void logWindow(App& app) {
    if (!ImGui::Begin(winTitle("Registo", "log").c_str())) { ImGui::End(); return; }
    (void)app;
    auto entries = Log::recent(400);
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    for (auto& e : entries) {
        ImVec4 c = e.level == LogLevel::Error ? ImVec4(1, 0.4f, 0.4f, 1) : e.level == LogLevel::Warn ? ImVec4(1, 0.75f, 0.3f, 1) : ImVec4(0.75f, 0.75f, 0.78f, 1);
        ImGui::TextColored(c, "%s", e.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

static void statusBar(App& app) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float h = ImGui::GetFrameHeight() + 6;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 3));
    ImGui::Begin("##status", nullptr, f);
    outputsButton(app, ImVec2(0, 0));
    ImGui::SameLine(0, 14);
    int playing = 0, dropped = 0;
    for (auto& l : app.project.comp.layers)
        if (VideoPlayer* v = app.comp.activePlayer(l.id)) { playing++; dropped += v->dropped(); }
    ImVec4 fpsCol = app.fps >= 49 ? ImVec4(0.4f, 0.9f, 0.5f, 1) : app.fps >= 29 ? ImVec4(1, 0.75f, 0.3f, 1) : ImVec4(1, 0.4f, 0.4f, 1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(fpsCol, "%.1f fps", app.fps);
    ImGui::SameLine(); ImGui::TextDisabled("|  %.2f ms", app.frameMs);
    ImGui::SameLine(); ImGui::TextDisabled(T("|  clips a tocar: %d"), playing);
    ImGui::SameLine(); ImGui::TextDisabled(T("|  frames descartados: %d"), dropped);
    ImGui::SameLine(); ImGui::TextDisabled(T("|  saídas no ar: %d"), (int)app.liveScreens.size());
    ImGui::SameLine(); ImGui::TextDisabled("|  %s", toUtf8(gfx().adapterName()).c_str());
    std::string name = app.projectPath.empty() ? std::string(T("sem título")) : toUtf8(app.projectPath);
    ImGui::SameLine(); ImGui::TextDisabled("|  %s%s", name.c_str(), app.dirty ? " *" : "");
    ImGui::End();
    ImGui::PopStyleVar();
}

static void shortcuts(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || shortcutCaptureActive()) return;
    const Settings& s = settings();
    for (int a = 0; a < (int)Action::Count; ++a) {
        const KeyBinding& b = s.keys[a];
        if (!b.key) continue;
        if (b.ctrl != io.KeyCtrl || b.shift != io.KeyShift || b.alt != io.KeyAlt) continue;
        if (ImGui::IsKeyPressed((ImGuiKey)b.key, false)) { app.runAction(a); break; }
    }
}

static std::string menuKey(Action a) { return bindingLabel(settings().keys[(int)a]); }

void drawUI(App& app) {
    shortcuts(app);

    auto openSettings = [&](int tab) { app.ui.showSettings = true; app.ui.settingsTab = tab; };
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(T("Ficheiro"))) {
            if (ImGui::MenuItem(T("Novo projeto"))) {
                if (!app.dirty || MessageBoxW(app.hwnd(), fromUtf8(T("Descartar alterações?")).c_str(), L"PLAYERSLICE", MB_YESNO | MB_ICONQUESTION) == IDYES)
                    app.newProject();
            }
            if (ImGui::MenuItem(T("Abrir..."), menuKey(Action::Open).c_str())) app.openDialog();
            if (ImGui::MenuItem(T("Gravar"), menuKey(Action::Save).c_str())) app.saveOrSaveAs();
            if (ImGui::MenuItem(T("Gravar como..."), menuKey(Action::SaveAs).c_str())) app.saveAsDialog();
            ImGui::Separator();
            if (ImGui::MenuItem(T("Sair"))) PostMessageW(app.hwnd(), WM_CLOSE, 0, 0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Editar"))) {
            if (ImGui::MenuItem(T("Desfazer"), menuKey(Action::Undo).c_str(), false, app.canUndo())) app.undo();
            if (ImGui::MenuItem(T("Refazer"), menuKey(Action::Redo).c_str(), false, app.canRedo())) app.redo();
            ImGui::Separator();
            if (ImGui::MenuItem(T("Adicionar layer"))) { app.checkpoint(); app.project.addLayer("Layer " + std::to_string(app.project.comp.layers.size() + 1)); }
            if (ImGui::MenuItem(T("Adicionar coluna"))) { app.checkpoint(); app.project.ensureColumns((int)app.project.comp.columns.size() + 1); }
            ImGui::Separator();
            if (ImGui::MenuItem(T("Parar todos os clips"), menuKey(Action::StopAll).c_str())) app.comp.stopAll(app.project);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Saídas"))) {
            if (ImGui::MenuItem(T("Ligar/desligar saídas marcadas"), menuKey(Action::ToggleOutputs).c_str())) app.toggleOutputs();
            if (ImGui::MenuItem(T("Ligar todas as saídas"), menuKey(Action::OutputsOn).c_str())) app.setAllOutputs(true);
            if (ImGui::MenuItem(T("Desligar todas as saídas"), menuKey(Action::OutputsOff).c_str())) app.setAllOutputs(false);
            ImGui::Separator();
            if (ImGui::MenuItem(T("Editor de saídas e slices"), menuKey(Action::OutputEditor).c_str())) app.ui.focusEditor = true;
            if (ImGui::MenuItem(T("Resolução da composição..."))) app.ui.showCompSettings = true;
            if (ImGui::MenuItem(T("Detetar monitores de novo"))) app.refreshDisplays();
            ImGui::Separator();
            ImGui::TextDisabled("%s", T("Saídas detetadas:"));
            for (auto& d : app.displays) ImGui::TextDisabled("  %s", d.label().c_str());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Ajuda"))) {
            if (ImGui::MenuItem(T("Atalhos de teclado"))) openSettings(5);
            if (ImGui::MenuItem(T("Como usar"))) openSettings(7);
            if (ImGui::MenuItem(T("Novidades desta versão"))) openSettings(8);
            ImGui::Separator();
            if (ImGui::MenuItem(T("Sobre o PLAYERSLICE"))) openSettings(9);
            ImGui::EndMenu();
        }
        // "Definições" abre logo a janela (sem submenu)
        if (ImGui::MenuItem(T("Definições"))) openSettings(app.ui.settingsTab);

        // indicador à direita na barra de menus
        const char* st = app.anyLive() ? T("● NO AR") : T("○ saídas desligadas");
        float tw = ImGui::CalcTextSize(st).x;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - tw - 16);
        ImGui::TextColored(app.anyLive() ? ImVec4(1, 0.3f, 0.3f, 1) : ImVec4(0.55f, 0.55f, 0.58f, 1), "%s", st);
        ImGui::EndMainMenuBar();
    }

    // dockspace por baixo de tudo (deixando espaço para a barra de estado)
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float statusH = ImGui::GetFrameHeight() + 6;
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - statusH));
    ImGuiWindowFlags hf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                          ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##host", nullptr, hf);
    ImGui::PopStyleVar();
    ImGuiID dock = ImGui::GetID("MainDock");
    if (!app.ui.layoutBuilt) {
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock);
        if (!node || !node->IsSplitNode() || ImGui::GetFrameCount() > 2) buildDefaultLayout(dock);
        app.ui.layoutBuilt = true;
    }
    ImGui::DockSpace(dock, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();

    // proteção: logo após o arranque, se alguma janela principal não estiver encaixada
    // (disposição antiga ou corrompida), repor a disposição de origem
    if (ImGui::GetFrameCount() == 3) {
        const char* ids[] = {"###clips", "###slices", "###comp", "###monitor", "###props", "###log"};
        for (const char* id : ids) {
            ImGuiWindow* w = ImGui::FindWindowByName(id);
            if (w && w->DockId == 0) { app.ui.layoutBuilt = false; PS_WARN("Disposição das janelas inválida — reposta"); break; }
        }
    }

    drawClipGrid(app);
    drawSliceEditor(app);
    compositionWindow(app);
    outputsMonitor(app);
    propertiesWindow(app);
    logWindow(app);
    statusBar(app);
    compSettingsPopup(app);
    drawSettings(app);
    drawUpdatePopup(app);

    // aviso: saída em ecrã cheio por cima da interface
    HMONITOR mine = MonitorFromWindow(app.hwnd(), MONITOR_DEFAULTTONEAREST);
    if (app.outputs.coversMonitor(mine)) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 40), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::Begin("##warn", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s", T("Uma saída está no mesmo monitor que a interface."));
        ImGui::End();
    }
}

} // namespace ps
