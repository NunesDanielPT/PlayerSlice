#include "ui/UI.h"
#include "app/App.h"
#include "core/Log.h"
#include "video/Thumbnail.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdio>
#include <shellapi.h>

namespace ps {

static void buildDefaultLayout(ImGuiID dock) {
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->WorkSize);
    ImGuiID right, left, bottomLeft;
    ImGui::DockBuilderSplitNode(dock, ImGuiDir_Right, 0.22f, &right, &left);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.34f, &bottomLeft, &left);
    ImGuiID bl1, bl2;
    ImGui::DockBuilderSplitNode(bottomLeft, ImGuiDir_Left, 0.5f, &bl1, &bl2);
    ImGui::DockBuilderDockWindow("Clips", left);
    ImGui::DockBuilderDockWindow("Saídas e Slices", left);
    ImGui::DockBuilderDockWindow("Composição", bl1);
    ImGui::DockBuilderDockWindow("Monitor de saídas", bl2);
    ImGui::DockBuilderDockWindow("Registo", bl2);
    ImGui::DockBuilderDockWindow("Propriedades", right);
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

static void compositionWindow(App& app) {
    if (!ImGui::Begin("Composição")) { ImGui::End(); return; }
    const RenderTarget& rt = app.comp.compRT();
    ImGui::TextDisabled("%dx%d", app.project.comp.width, app.project.comp.height);
    ImGui::SameLine();
    if (ImGui::SmallButton("Definições...")) app.ui.showCompSettings = true;
    if (rt.valid()) fitImage(texId(rt.srv.Get()), (float)rt.w, (float)rt.h, ImGui::GetContentRegionAvail());
    ImGui::End();
}

static void outputsMonitor(App& app) {
    if (!ImGui::Begin("Monitor de saídas")) { ImGui::End(); return; }
    Project& p = app.project;
    if (p.screens.empty()) ImGui::TextDisabled("Sem ecrãs. Cria um em \"Saídas e Slices\".");
    float availW = ImGui::GetContentRegionAvail().x;
    int n = std::max(1, (int)p.screens.size());
    int cols = std::clamp((int)(availW / 220), 1, n);
    float w = (availW - (cols - 1) * 8) / cols;
    for (int i = 0; i < (int)p.screens.size(); ++i) {
        Screen& s = p.screens[i];
        if (i % cols) ImGui::SameLine(0, 8);
        ImGui::BeginGroup();
        ImGui::PushID((int)s.id);
        bool live = s.device != ScreenDevice::Disabled;
        bool missing = s.device == ScreenDevice::Monitor &&
                       std::none_of(app.displays.begin(), app.displays.end(), [&](const DisplayInfo& d) { return d.key == s.monitorKey; });
        ImVec4 dot = missing ? ImVec4(1, 0.3f, 0.3f, 1) : live ? ImVec4(0.35f, 0.9f, 0.45f, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1);
        ImGui::TextColored(dot, "●");
        ImGui::SameLine();
        ImGui::Text("%s  %dx%d%s", s.name.c_str(), s.width, s.height, missing ? "  SEM SINAL" : "");
        float h = w * s.height / std::max(1, s.width);
        if (const RenderTarget* rt = app.comp.screenRT(s.id)) {
            if (ImGui::ImageButton("##scr", texId(rt->srv.Get()), ImVec2(w - 8, h))) {
                app.ui.selScreen = s.id; app.ui.selSlice = 0; app.ui.focusEditor = true;
            }
        }
        ImGui::PopID();
        ImGui::EndGroup();
    }
    ImGui::End();
}

static void propertiesWindow(App& app) {
    if (!ImGui::Begin("Propriedades")) { ImGui::End(); return; }
    Project& p = app.project;
    int li = app.ui.selLayer;
    if (li < 0 || li >= (int)p.comp.layers.size()) { ImGui::TextDisabled("Seleciona uma layer."); ImGui::End(); return; }
    Layer& l = p.comp.layers[li];
    ImGui::PushItemWidth(-100);

    int ci = app.ui.selColumn;
    if (ci >= 0 && ci < (int)l.clips.size() && !l.clips[ci].empty()) {
        Clip& c = l.clips[ci];
        ImGui::SeparatorText("Clip");
        inputTextStd("Nome##clip", c.name);
        undoOnActivate(app);
        ImGui::TextWrapped("%s", c.path.c_str());
        auto th = ThumbnailCache::get().request(c.path);
        if (th->state == ThumbnailCache::Entry::State::Ready) {
            const MediaInfo& mi = th->info;
            ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.8f, 1), "%dx%d  ·  %.2f fps  ·  %.1f s  ·  %s", mi.width, mi.height, mi.fps, mi.duration, mi.codec.c_str());
            if (l.useRegion && (std::abs(l.region.w - mi.width) > 0.5f || std::abs(l.region.h - mi.height) > 0.5f))
                ImGui::TextColored(ImVec4(1, 0.7f, 0.25f, 1), "Atenção: o vídeo não tem o tamanho da região (%.0fx%.0f)", l.region.w, l.region.h);
            std::string codec = mi.codec;
            if (codec != "hap" && (mi.width * mi.height >= 3840 * 2160 || mi.width >= 7680))
                ImGui::TextColored(ImVec4(1, 0.7f, 0.25f, 1), "Dica: para vídeos muito grandes usa HAP / HAP Q.");
        }
        if (VideoPlayer* v = app.comp.activePlayer(l.id); v && l.activeColumn == ci) {
            MediaInfo mi = v->info();
            ImGui::TextDisabled("A tocar: %s  ·  fila %d  ·  descartados %d", mi.codec.c_str(), v->queued(), v->dropped());
            if (v->state() == VideoPlayer::State::Error) ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s", v->error().c_str());
        }
        const char* loops[] = {"Loop", "Tocar uma vez e parar no fim", "Tocar uma vez e limpar"};
        int lm = (int)c.loop;
        if (ImGui::Combo("Repetição", &lm, loops, 3)) { app.checkpoint(); c.loop = (LoopMode)lm; }
        ImGui::SliderFloat("Velocidade", &c.speed, 0.f, 4.f, "%.2fx");
        undoOnActivate(app);
        ImGui::SliderFloat("Opacidade##clip", &c.opacity, 0, 1, "%.2f");
        undoOnActivate(app);
        const char* fits[] = {"Esticar", "Caber (com barras)", "Preencher (cortar)", "Tamanho nativo"};
        int fm = (int)c.fit;
        if (ImGui::Combo("Encaixe", &fm, fits, 4)) { app.checkpoint(); c.fit = (FitMode)fm; }
        ImGui::DragFloat("Escala", &c.scale, 0.005f, 0.01f, 20.f, "%.3f");
        undoOnActivate(app);
        float off[2] = {c.offsetX, c.offsetY};
        if (ImGui::DragFloat2("Deslocar", off, 1, -16384, 16384, "%.0f")) { c.offsetX = off[0]; c.offsetY = off[1]; }
        undoOnActivate(app);
        if (ImGui::Button("Repor transformação")) { app.checkpoint(); c.scale = 1; c.offsetX = c.offsetY = 0; }
    }

    ImGui::SeparatorText("Layer");
    inputTextStd("Nome##layer", l.name);
    undoOnActivate(app);
    ImGui::SliderFloat("Opacidade##layer", &l.opacity, 0, 1, "%.2f");
    undoOnActivate(app);
    const char* blends[] = {"Normal", "Add", "Multiply", "Screen"};
    int bm = (int)l.blend;
    if (ImGui::Combo("Mistura", &bm, blends, 4)) { app.checkpoint(); l.blend = (BlendMode)bm; }
    ImGui::SliderFloat("Fade (s)", &l.fadeTime, 0, 10, "%.2f");
    undoOnActivate(app);

    ImGui::SeparatorText("Região na composição");
    if (ImGui::Checkbox("Usar região", &l.useRegion)) app.checkpoint();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Os vídeos desta layer encaixam neste retângulo da composição\n(ex.: a zona de um ledwall). Desligado = composição inteira.");
    if (l.useRegion) {
        rectEditor(app, "region", l.region, (float)p.comp.width, (float)p.comp.height);
        if (ImGui::BeginCombo("Copiar de", "slice...")) {
            for (auto& s : p.screens)
                for (auto& sl : s.slices) {
                    std::string n = s.name + " / " + sl.name + "##" + std::to_string(sl.id);
                    if (ImGui::Selectable(n.c_str())) { app.checkpoint(); l.region = sl.input; }
                }
            ImGui::EndCombo();
        }
        if (ci >= 0 && ci < (int)l.clips.size() && !l.clips[ci].empty()) {
            auto th = ThumbnailCache::get().request(l.clips[ci].path);
            if (th->state == ThumbnailCache::Entry::State::Ready && ImGui::Button("Tamanho = vídeo selecionado")) {
                app.checkpoint(); l.region.w = (float)th->info.width; l.region.h = (float)th->info.height;
            }
        }
    }
    ImGui::PopItemWidth();
    ImGui::End();
}

static void compSettingsPopup(App& app) {
    if (app.ui.showCompSettings) { ImGui::OpenPopup("Definições da composição"); app.ui.showCompSettings = false; }
    if (ImGui::BeginPopupModal("Definições da composição", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Composition& c = app.project.comp;
        static int wh[2];
        if (ImGui::IsWindowAppearing()) { wh[0] = c.width; wh[1] = c.height; }
        ImGui::InputInt2("Resolução", wh);
        const struct { const char* n; int w, h; } presets[] = {
            {"1920x1080", 1920, 1080}, {"2560x1440", 2560, 1440}, {"3840x2160 (4K)", 3840, 2160},
            {"5760x1080 (3x HD)", 5760, 1080}, {"7680x1080 (4x HD)", 7680, 1080}, {"7680x2160 (2x 4K)", 7680, 2160},
            {"7680x4320 (8K)", 7680, 4320}};
        for (auto& pr : presets) {
            if (ImGui::SmallButton(pr.n)) { wh[0] = pr.w; wh[1] = pr.h; }
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::DragFloat("FPS de referência", &c.fps, 0.1f, 1, 240, "%.2f");
        ImGui::TextDisabled("Máximo da GPU: 16384x16384. VRAM livre estimada: %zu MB", gfx().vramMB());
        if (ImGui::Button("Aplicar", ImVec2(120, 0))) {
            app.checkpoint();
            c.width = std::clamp(wh[0], 16, 16384);
            c.height = std::clamp(wh[1], 16, 16384);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void aboutPopup(App& app) {
    if (app.ui.showAbout) { ImGui::OpenPopup("Sobre o PLAYERSLICE"); app.ui.showAbout = false; }
    if (ImGui::BeginPopupModal("Sobre o PLAYERSLICE", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(0.25f, 0.88f, 0.75f, 1), "PLAYERSLICE %s", PLAYERSLICE_VERSION);
        ImGui::Text("Player de vídeo para ecrãs LED fora de formato.");
        ImGui::Separator();
        ImGui::Text("Criado por Daniel Nunes");
        if (ImGui::SmallButton("github.com/NunesDanielPT"))
            ShellExecuteW(nullptr, L"open", L"https://github.com/NunesDanielPT", nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::Separator();
        ImGui::TextDisabled("GPU: %s (%zu MB)", toUtf8(gfx().adapterName()).c_str(), gfx().vramMB());
        ImGui::TextDisabled("Usa FFmpeg (LGPL/GPL), Dear ImGui, HAP (Vidvox), Snappy, nlohmann/json.");
        if (ImGui::Button("Fechar", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void logWindow(App& app) {
    if (!ImGui::Begin("Registo")) { ImGui::End(); return; }
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
    float h = ImGui::GetFrameHeight() + 4;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 2));
    ImGui::Begin("##status", nullptr, f);
    int playing = 0, dropped = 0;
    for (auto& l : app.project.comp.layers)
        if (VideoPlayer* v = app.comp.activePlayer(l.id)) { playing++; dropped += v->dropped(); }
    ImVec4 fpsCol = app.fps >= 49 ? ImVec4(0.4f, 0.9f, 0.5f, 1) : app.fps >= 29 ? ImVec4(1, 0.75f, 0.3f, 1) : ImVec4(1, 0.4f, 0.4f, 1);
    ImGui::TextColored(fpsCol, "%.1f fps", app.fps);
    ImGui::SameLine(); ImGui::TextDisabled("|  %.2f ms", app.frameMs);
    ImGui::SameLine(); ImGui::TextDisabled("|  clips a tocar: %d", playing);
    ImGui::SameLine(); ImGui::TextDisabled("|  frames descartados: %d", dropped);
    ImGui::SameLine(); ImGui::TextDisabled("|  saídas abertas: %d", app.outputs.count());
    ImGui::SameLine(); ImGui::TextDisabled("|  %s", toUtf8(gfx().adapterName()).c_str());
    std::string name = app.projectPath.empty() ? "sem título" : toUtf8(app.projectPath);
    ImGui::SameLine(); ImGui::TextDisabled("|  %s%s", name.c_str(), app.dirty ? " *" : "");
    ImGui::End();
    ImGui::PopStyleVar();
}

static void shortcuts(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) { if (io.KeyShift) app.saveAsDialog(); else app.saveOrSaveAs(); }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) app.openDialog();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) app.redo(); else app.undo(); }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) app.redo();
}

void drawUI(App& app) {
    shortcuts(app);

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Ficheiro")) {
            if (ImGui::MenuItem("Novo projeto")) {
                if (!app.dirty || MessageBoxW(app.hwnd(), L"Descartar alterações?", L"PLAYERSLICE", MB_YESNO | MB_ICONQUESTION) == IDYES)
                    app.newProject();
            }
            if (ImGui::MenuItem("Abrir...", "Ctrl+O")) app.openDialog();
            if (ImGui::MenuItem("Gravar", "Ctrl+S")) app.saveOrSaveAs();
            if (ImGui::MenuItem("Gravar como...", "Ctrl+Shift+S")) app.saveAsDialog();
            ImGui::Separator();
            if (ImGui::MenuItem("Abrir pasta de registos")) ShellExecuteW(nullptr, L"open", appDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::Separator();
            if (ImGui::MenuItem("Sair")) PostMessageW(app.hwnd(), WM_CLOSE, 0, 0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Editar")) {
            if (ImGui::MenuItem("Desfazer", "Ctrl+Z", false, app.canUndo())) app.undo();
            if (ImGui::MenuItem("Refazer", "Ctrl+Y", false, app.canRedo())) app.redo();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Composição")) {
            if (ImGui::MenuItem("Definições...")) app.ui.showCompSettings = true;
            if (ImGui::MenuItem("Adicionar layer")) { app.checkpoint(); app.project.addLayer("Layer " + std::to_string(app.project.comp.layers.size() + 1)); }
            if (ImGui::MenuItem("Adicionar coluna")) { app.checkpoint(); app.project.ensureColumns((int)app.project.comp.columns.size() + 1); }
            ImGui::Separator();
            if (ImGui::MenuItem("Parar tudo")) app.comp.stopAll(app.project);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Saídas")) {
            if (ImGui::MenuItem("Editor de saídas e slices")) app.ui.focusEditor = true;
            if (ImGui::MenuItem("Detetar monitores de novo")) app.refreshDisplays();
            ImGui::Separator();
            bool anyLive = false;
            for (auto& s : app.project.screens) if (s.device != ScreenDevice::Disabled) anyLive = true;
            if (ImGui::MenuItem("Desligar todas as saídas", nullptr, false, anyLive)) {
                app.checkpoint();
                for (auto& s : app.project.screens) s.device = ScreenDevice::Disabled;
            }
            ImGui::Separator();
            ImGui::TextDisabled("Saídas detetadas:");
            for (auto& d : app.displays) ImGui::TextDisabled("  %s", d.label().c_str());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Ver")) {
            if (ImGui::MenuItem("Repor disposição das janelas")) app.ui.layoutBuilt = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Ajuda")) {
            if (ImGui::MenuItem("Sobre o PLAYERSLICE")) app.ui.showAbout = true;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // dockspace por baixo de tudo (deixando espaço para a barra de estado)
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float statusH = ImGui::GetFrameHeight() + 4;
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

    drawClipGrid(app);
    drawSliceEditor(app);
    compositionWindow(app);
    outputsMonitor(app);
    propertiesWindow(app);
    logWindow(app);
    statusBar(app);
    compSettingsPopup(app);
    aboutPopup(app);

    // aviso: saída em ecrã cheio por cima da interface
    HMONITOR mine = MonitorFromWindow(app.hwnd(), MONITOR_DEFAULTTONEAREST);
    if (app.outputs.coversMonitor(mine)) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 40), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::Begin("##warn", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "Uma saída está no mesmo monitor que a interface.");
        ImGui::End();
    }
}

} // namespace ps
