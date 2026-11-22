#include "ui/UI.h"
#include "core/I18n.h"
#include "app/App.h"
#include "video/Thumbnail.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdio>
#include <cmath>

namespace ps {

static std::string fileStem(const std::string& path) {
    size_t s = path.find_last_of("\\/");
    std::string n = s == std::string::npos ? path : path.substr(s + 1);
    size_t d = n.find_last_of('.');
    return d == std::string::npos ? n : n.substr(0, d);
}

static void assignClip(App& app, int li, int col, const std::string& path) {
    Project& p = app.project;
    if (li < 0 || li >= (int)p.comp.layers.size()) return;
    p.ensureColumns(col + 1);
    Clip& c = p.comp.layers[li].clips[col];
    Clip n;
    n.id = p.newId();
    n.path = path;
    n.name = fileStem(path);
    c = n;
    ThumbnailCache::get().request(path);
}

struct CellPayload { int layer, col; };

static void layerHeader(App& app, int li, float rowH) {
    Project& p = app.project;
    Layer& l = p.comp.layers[li];
    ImGui::PushID((int)l.id);
    bool sel = app.ui.selLayer == li;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + rowH - 2), sel ? IM_COL32(36, 70, 64, 255) : IM_COL32(34, 36, 40, 255), 3);
    if (l.activeColumn >= 0) dl->AddRectFilled(p0, ImVec2(p0.x + 3, p0.y + rowH - 2), col::Playing);

    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p0.y + 4));
    if (ImGui::InvisibleButton("##sel", ImVec2(w - 8, 18))) app.ui.selLayer = li;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8, p0.y + 4));
    ImGui::TextUnformatted(l.name.c_str());
    if (l.useRegion) {
        char reg[64]; snprintf(reg, sizeof(reg), "%.0fx%.0f", l.region.w, l.region.h);
        ImVec2 ts = ImGui::CalcTextSize(reg);
        dl->AddText(ImVec2(p0.x + w - ts.x - 8, p0.y + 4), col::TextDim, reg);
    }

    // botões: X parar, B esconder, S solo
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p0.y + 26));
    float bs = 22;
    if (ImGui::Button("X", ImVec2(bs, bs))) app.comp.stopLayer(p, li);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("Parar layer"));
    ImGui::SameLine(0, 3);
    ImGui::PushStyleColor(ImGuiCol_Button, l.bypass ? ImVec4(0.75f, 0.3f, 0.3f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::Button("B", ImVec2(bs, bs))) { app.checkpoint(); l.bypass = !l.bypass; }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("Bypass (esconder)"));
    ImGui::SameLine(0, 3);
    ImGui::PushStyleColor(ImGuiCol_Button, l.solo ? ImVec4(0.85f, 0.7f, 0.2f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::Button("S", ImVec2(bs, bs))) { app.checkpoint(); l.solo = !l.solo; }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("Solo"));
    ImGui::SameLine(0, 6);
    ImGui::SetNextItemWidth(std::max(40.f, w - (bs + 3) * 3 - 18));
    ImGui::SliderFloat("##op", &l.opacity, 0, 1, "%.2f");
    undoOnActivate(app);

    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8, p0.y + 54));
    const char* blends[] = {"Normal", "Add", "Multiply", "Screen"};
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.62f, 1), T("%s  ·  fade %.1fs"), blends[(int)l.blend], l.fadeTime);

    // menu de contexto
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##ctx", ImVec2(w, rowH - 2), ImGuiButtonFlags_MouseButtonRight);
    if (ImGui::BeginPopupContextItem("layerctx")) {
        app.ui.selLayer = li;
        if (ImGui::MenuItem(T("Adicionar layer acima"))) {
            app.checkpoint();
            Layer nl; nl.id = p.newId(); nl.name = "Layer " + std::to_string(p.comp.layers.size() + 1);
            nl.region = {0, 0, (float)p.comp.width, (float)p.comp.height};
            nl.clips.resize(p.comp.columns.size());
            p.comp.layers.insert(p.comp.layers.begin() + li, nl);
        }
        if (ImGui::MenuItem(T("Duplicar layer"))) {
            app.checkpoint();
            Layer copy = l; copy.id = p.newId(); copy.name += T(" (cópia)"); copy.activeColumn = -1;
            for (auto& c : copy.clips) if (!c.empty()) c.id = p.newId();
            p.comp.layers.insert(p.comp.layers.begin() + li, copy);
        }
        if (ImGui::MenuItem(T("Mover para cima"), nullptr, false, li > 0)) { app.checkpoint(); std::swap(p.comp.layers[li], p.comp.layers[li - 1]); }
        if (ImGui::MenuItem(T("Mover para baixo"), nullptr, false, li + 1 < (int)p.comp.layers.size())) { app.checkpoint(); std::swap(p.comp.layers[li], p.comp.layers[li + 1]); }
        ImGui::Separator();
        if (ImGui::MenuItem(T("Esvaziar clips"))) { app.checkpoint(); app.comp.stopLayer(p, li); for (auto& c : p.comp.layers[li].clips) c = Clip{}; }
        if (ImGui::MenuItem(T("Apagar layer"), nullptr, false, p.comp.layers.size() > 1)) {
            app.checkpoint();
            app.comp.stopLayer(p, li);
            // slices que usavam esta layer como fonte passam para a composição
            for (auto& s : p.screens) for (auto& sl : s.slices)
                if (sl.sourceLayer == p.comp.layers[li].id) sl.sourceLayer = 0;
            p.comp.layers.erase(p.comp.layers.begin() + li);
            app.ui.selLayer = std::min(app.ui.selLayer, (int)p.comp.layers.size() - 1);
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

static void clipCell(App& app, int li, int ci, float cellW, float cellH) {
    Project& p = app.project;
    Layer& l = p.comp.layers[li];
    Clip& c = l.clips[ci];
    // ID único por célula: layer + coluna (antes só a coluna → conflito entre layers)
    ImGui::PushID("cell");
    ImGui::PushID((int)l.id);
    ImGui::PushID(ci);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1(p0.x + cellW, p0.y + cellH);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    app.ui.cells.push_back({li, ci, p0.x, p0.y, p1.x, p1.y});

    bool active = l.activeColumn == ci && !c.empty();
    bool inPreview = !c.empty() && app.comp.previewLayerId() == l.id && app.comp.previewColumn() == ci;
    bool sel = app.ui.selLayer == li && app.ui.selColumn == ci;
    ImGui::InvisibleButton("##cell", ImVec2(cellW, cellH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    // metade de cima → PROGRAMA (no ar) · metade de baixo → PREVIEW
    bool topHalf = ImGui::GetIO().MousePos.y < p0.y + cellH * 0.5f;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        app.ui.selLayer = li; app.ui.selColumn = ci;
        if (!c.empty()) {
            if (topHalf) app.comp.trigger(p, li, ci);
            else app.comp.previewClip(p, li, ci);
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && c.empty()) {
        auto files = app.pickVideoFiles();
        if (!files.empty()) {
            app.checkpoint();
            for (size_t i = 0; i < files.size(); ++i) assignClip(app, li, ci + (int)i, files[i]);
        }
    }

    dl->AddRectFilled(p0, p1, c.empty() ? IM_COL32(30, 31, 35, 255) : IM_COL32(18, 18, 20, 255), 3);
    if (!c.empty()) {
        auto th = ThumbnailCache::get().request(c.path);
        if (th->srv) {
            float aw = cellW - 4, ah = cellH - 22;
            float s = std::min(aw / th->w, ah / th->h);
            float iw = th->w * s, ih = th->h * s;
            ImVec2 i0(p0.x + 2 + (aw - iw) * 0.5f, p0.y + 2 + (ah - ih) * 0.5f);
            dl->AddImage(texId(th->srv.Get()), i0, ImVec2(i0.x + iw, i0.y + ih));
        } else if (th->state == ThumbnailCache::Entry::State::Failed) {
            dl->AddText(ImVec2(p0.x + 6, p0.y + 6), col::Error, T("ficheiro inválido"));
        }
        // nome
        dl->AddRectFilled(ImVec2(p0.x, p1.y - 20), p1, IM_COL32(0, 0, 0, 170), 3);
        dl->PushClipRect(ImVec2(p0.x + 2, p1.y - 20), ImVec2(p1.x - 2, p1.y), true);
        dl->AddText(ImVec2(p0.x + 5, p1.y - 18), IM_COL32(230, 230, 230, 255), c.name.c_str());
        dl->PopClipRect();
        // progresso do clip ativo
        if (active) {
            if (VideoPlayer* v = app.comp.activePlayer(l.id)) {
                MediaInfo mi = v->info();
                if (mi.duration > 0) {
                    double pos = fmod(std::max(0.0, v->position()), mi.duration) / mi.duration;
                    dl->AddRectFilled(ImVec2(p0.x, p1.y - 3), ImVec2(p0.x + (float)(cellW * pos), p1.y), col::Playing);
                }
                if (v->state() == VideoPlayer::State::Error)
                    dl->AddText(ImVec2(p0.x + 6, p0.y + 6), col::Error, T("ERRO"));
            }
        }
    } else {
        dl->AddText(ImVec2(p0.x + cellW * 0.5f - 4, p0.y + cellH * 0.5f - 8), IM_COL32(70, 72, 78, 255), "+");
    }
    // indicação das duas zonas ao passar o rato
    if (hovered && !c.empty() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        float mid = p0.y + cellH * 0.5f;
        ImVec2 z0 = topHalf ? p0 : ImVec2(p0.x, mid), z1 = topHalf ? ImVec2(p1.x, mid) : p1;
        dl->AddRectFilled(z0, z1, topHalf ? IM_COL32(220, 50, 50, 70) : IM_COL32(240, 170, 40, 70), 3);
        const char* t = topHalf ? T("▲ PROGRAMA") : T("▼ PREVIEW");
        ImVec2 ts = ImGui::CalcTextSize(t);
        ImVec2 tp(p0.x + (cellW - ts.x) * 0.5f, (z0.y + z1.y) * 0.5f - ts.y * 0.5f);
        dl->AddRectFilled(ImVec2(tp.x - 4, tp.y - 1), ImVec2(tp.x + ts.x + 4, tp.y + ts.y + 1), IM_COL32(0, 0, 0, 190), 2);
        dl->AddText(tp, IM_COL32(255, 255, 255, 255), t);
        dl->AddLine(ImVec2(p0.x + 4, mid), ImVec2(p1.x - 4, mid), IM_COL32(255, 255, 255, 60));
    }
    ImU32 border = active ? col::Playing : inPreview ? IM_COL32(240, 170, 40, 255) : sel ? col::Accent : hovered ? IM_COL32(90, 94, 100, 255) : IM_COL32(48, 50, 55, 255);
    dl->AddRect(p0, p1, border, 3, 0, active || sel || inPreview ? 2.f : 1.f);
    if (inPreview) dl->AddText(ImVec2(p0.x + 5, p0.y + 3), IM_COL32(240, 170, 40, 255), "PV");

    // arrastar clips entre células (Ctrl = copiar)
    if (!c.empty() && ImGui::BeginDragDropSource()) {
        CellPayload pl{li, ci};
        ImGui::SetDragDropPayload("PS_CLIP", &pl, sizeof(pl));
        ImGui::Text("%s", c.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PS_CLIP")) {
            CellPayload src = *(const CellPayload*)pl->Data;
            if (!(src.layer == li && src.col == ci)) {
                app.checkpoint();
                Clip& from = p.comp.layers[src.layer].clips[src.col];
                if (ImGui::GetIO().KeyCtrl) { Clip cp = from; cp.id = p.newId(); c = cp; }
                else std::swap(from, c);
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem("cellctx")) {
        app.ui.selLayer = li; app.ui.selColumn = ci;
        if (ImGui::MenuItem(T("Escolher vídeo(s)..."))) {
            auto files = app.pickVideoFiles();
            if (!files.empty()) { app.checkpoint(); for (size_t i = 0; i < files.size(); ++i) assignClip(app, li, ci + (int)i, files[i]); }
        }
        if (ImGui::MenuItem(T("Pôr no programa"), nullptr, false, !c.empty())) app.comp.trigger(p, li, ci);
        if (ImGui::MenuItem(T("Pôr em preview"), nullptr, false, !c.empty())) app.comp.previewClip(p, li, ci);
        if (ImGui::MenuItem(T("Remover clip"), nullptr, false, !c.empty())) {
            app.checkpoint();
            if (l.activeColumn == ci) app.comp.stopLayer(p, li);
            c = Clip{};
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}

void drawClipGrid(App& app) {
    Project& p = app.project;
    app.ui.cells.clear();

    ImGui::SetNextWindowSize(ImVec2(900, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(winTitle("Clips", "clips").c_str())) { ImGui::End(); return; }

    // barra de ferramentas
    if (ImGui::Button(T("+ Layer"))) {
        app.checkpoint();
        p.addLayer("Layer " + std::to_string(p.comp.layers.size() + 1));
        app.ui.selLayer = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("+ Coluna"))) { app.checkpoint(); p.ensureColumns((int)p.comp.columns.size() + 1); }
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir vídeos..."))) {
        auto files = app.pickVideoFiles();
        if (!files.empty()) {
            app.checkpoint();
            int li = std::clamp(app.ui.selLayer, 0, (int)p.comp.layers.size() - 1);
            int ci = std::max(0, app.ui.selColumn);
            for (size_t i = 0; i < files.size(); ++i) assignClip(app, li, ci + (int)i, files[i]);
        }
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.2f, 0.2f, 1));
    if (ImGui::Button(T("Parar tudo"))) app.comp.stopAll(p);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("  Clique: metade de cima = programa · metade de baixo = preview · cabeçalho = coluna inteira no ar"));

    const float headerW = 210, cellW = 132, cellH = 84;
    const int ncols = (int)p.comp.columns.size();
    ImGuiTableFlags tf = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                         ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
    if (ImGui::BeginTable("grid", ncols + 1, tf)) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Layers", ImGuiTableColumnFlags_WidthFixed, headerW);
        for (int c = 0; c < ncols; ++c) ImGui::TableSetupColumn(p.comp.columns[c].c_str(), ImGuiTableColumnFlags_WidthFixed, cellW);

        // cabeçalho: colunas clicáveis (dispara a coluna inteira)
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled(T("Composição %dx%d"), p.comp.width, p.comp.height);
        for (int c = 0; c < ncols; ++c) {
            ImGui::TableSetColumnIndex(c + 1);
            ImGui::PushID(c);
            bool anyActive = false;
            for (auto& l : p.comp.layers) if (l.activeColumn == c) anyActive = true;
            if (anyActive) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.3f, 1));
            if (ImGui::Button(p.comp.columns[c].c_str(), ImVec2(cellW - 4, 0))) app.comp.triggerColumn(p, c);
            if (anyActive) ImGui::PopStyleColor();
            if (ImGui::BeginPopupContextItem("colctx")) {
                ImGui::SetNextItemWidth(160);
                inputTextStd(T("Nome"), p.comp.columns[c]);
                undoOnActivate(app);
                if (ImGui::MenuItem(T("Apagar coluna"), nullptr, false, ncols > 1)) {
                    app.checkpoint();
                    for (int li = 0; li < (int)p.comp.layers.size(); ++li) {
                        auto& l = p.comp.layers[li];
                        if (l.activeColumn == c) app.comp.stopLayer(p, li);
                        if (c < (int)l.clips.size()) l.clips.erase(l.clips.begin() + c);
                    }
                    p.comp.columns.erase(p.comp.columns.begin() + c);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        for (int li = 0; li < (int)p.comp.layers.size(); ++li) {
            if (li >= (int)p.comp.layers.size()) break;
            ImGui::TableNextRow(0, cellH + 6);
            ImGui::TableSetColumnIndex(0);
            layerHeader(app, li, cellH + 4);
            if (li >= (int)p.comp.layers.size()) break; // layer apagada no menu
            for (int c = 0; c < ncols && c < (int)p.comp.layers[li].clips.size(); ++c) {
                ImGui::TableSetColumnIndex(c + 1);
                clipCell(app, li, c, cellW - 4, cellH);
            }
        }
        ImGui::EndTable();
    }

    // ficheiros largados do Explorador
    if (!app.pendingDrops.empty()) {
        for (auto& cell : app.ui.cells) {
            if (app.dropX >= cell.x0 && app.dropX < cell.x1 && app.dropY >= cell.y0 && app.dropY < cell.y1) {
                app.checkpoint();
                for (size_t i = 0; i < app.pendingDrops.size(); ++i) assignClip(app, cell.layer, cell.column + (int)i, app.pendingDrops[i]);
                app.ui.selLayer = cell.layer; app.ui.selColumn = cell.column;
                break;
            }
        }
        app.pendingDrops.clear();
    }
    ImGui::End();
}

} // namespace ps
