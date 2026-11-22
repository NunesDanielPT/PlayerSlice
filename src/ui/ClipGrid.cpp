// Grelha de clips: layers (linhas) × colunas.
//   Clique: metade de cima = programa, metade de baixo = preview
//   Ctrl+clique / Shift+clique = selecionar vários; Ctrl+C / Ctrl+V / Ctrl+X / Delete
//   Arrastar clips entre células, colunas pelo cabeçalho, efeitos do painel Efeitos
#include "ui/UI.h"
#include <functional>
#include "core/Timecode.h"
#include "core/I18n.h"
#include "app/App.h"
#include "video/Thumbnail.h"
#include "core/Log.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

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
    Clip n;
    n.id = p.newId();
    n.path = path;
    n.name = fileStem(path);
    p.comp.layers[li].clips[col] = n;
    ThumbnailCache::get().request(path);
}

struct CellPayload { int layer, col; };

static int firstFreeColumn(Project& p, int li) {
    Layer& l = p.comp.layers[li];
    for (int c = 0; c < (int)l.clips.size(); ++c) if (l.clips[c].empty()) return c;
    p.ensureColumns((int)p.comp.columns.size() + 1);
    return (int)p.comp.columns.size() - 1;
}

// ---------------------------------------------------------------------------
// animação de arrastar: origem esbatida, fantasma a seguir o rato, linha de inserção
// ---------------------------------------------------------------------------
static bool isDragging(const char* type, int a, int b = -1) {
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (!pl || !pl->IsDataType(type)) return false;
    const int* d = (const int*)pl->Data;
    return d[0] == a && (b < 0 || (pl->DataSize >= (int)(2 * sizeof(int)) && d[1] == b));
}

static void dimSource(ImVec2 p0, ImVec2 p1) {
    ImDrawList* fg = ImGui::GetWindowDrawList();
    fg->AddRectFilled(p0, p1, IM_COL32(10, 10, 12, 170), 3);
    fg->AddRect(p0, p1, IM_COL32(64, 224, 192, 160), 3, 0, 1.5f);
}

// linha de inserção (pulsa ligeiramente)
static void insertLine(ImVec2 a, ImVec2 b) {
    float t = (float)ImGui::GetTime();
    int alpha = 180 + (int)(75 * std::sin(t * 10.f));
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    fg->AddLine(a, b, IM_COL32(64, 224, 192, alpha), 4.f);
    fg->AddCircleFilled(a, 5, IM_COL32(64, 224, 192, alpha));
    fg->AddCircleFilled(b, 5, IM_COL32(64, 224, 192, alpha));
}

static void moveLayer(App& app, int from, int to) {
    Project& p = app.project;
    int n = (int)p.comp.layers.size();
    if (from == to || from < 0 || to < 0 || from >= n || to >= n) return;
    app.checkpoint();
    Layer l = p.comp.layers[from];
    p.comp.layers.erase(p.comp.layers.begin() + from);
    p.comp.layers.insert(p.comp.layers.begin() + to, l);
    app.ui.selLayer = to;
}

// paleta de cores para layers e colunas (0 = sem cor)
static const uint32_t kSwatches[] = {0, 0xFF3C50D0, 0xFF30A060, 0xFFB0A030, 0xFFC07020, 0xFFB040C0, 0xFF3090E0, 0xFF808080};
static ImU32 tintU32(uint32_t c, int a) { return IM_COL32(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, a); }

void colorSwatches(App& app, uint32_t& target) {
    for (int i = 0; i < (int)(sizeof(kSwatches) / sizeof(kSwatches[0])); ++i) {
        if (i) ImGui::SameLine(0, 4);
        ImGui::PushID(i);
        uint32_t c = kSwatches[i];
        ImVec4 col = c ? ImVec4((c & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, ((c >> 16) & 0xFF) / 255.f, 1) : ImVec4(0.25f, 0.25f, 0.28f, 1);
        if (ImGui::ColorButton("##sw", col, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(20, 20))) {
            app.checkpoint();
            target = c;
            ImGui::CloseCurrentPopup();
        }
        if (target == c) ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(255, 255, 255, 255), 2.0f, 0, 2.0f);
        ImGui::PopID();
    }
}

// mover/inserir/apagar colunas mantendo o estado em execução coerente
// cues da stack visível acompanham as colunas movidas/inseridas/apagadas
static void remapCues(App& app, const std::function<int(int)>& map) {
    Project& p = app.project;
    auto& q = p.comp.cues;
    for (int i = (int)q.size() - 1; i >= 0; --i) {
        if (q[i].stack != p.comp.activeStack) continue;
        int nc = map(q[i].column);
        if (nc < 0) q[i].column = std::max(0, q[i].column - 1);   // coluna apagada: fica na anterior
        else q[i].column = nc;
    }
}

void moveColumn(App& app, int from, int to) {
    Project& p = app.project;
    int n = (int)p.comp.columns.size();
    if (from == to || from < 0 || to < 0 || from >= n || to >= n) return;
    app.checkpoint();
    auto mv = [&](auto& v) { auto x = v[from]; v.erase(v.begin() + from); v.insert(v.begin() + to, x); };
    mv(p.comp.columns);
    for (auto& l : p.comp.layers) if ((int)l.clips.size() == n) mv(l.clips);
    auto mapFn = [&](int c) {
        if (c == from) return to;
        if (from < to && c > from && c <= to) return c - 1;
        if (from > to && c >= to && c < from) return c + 1;
        return c;
    };
    app.comp.remapColumns(p, mapFn);
    remapCues(app, mapFn);
    app.ui.selCells.clear();
    if (app.ui.selColumnProp == from) app.ui.selColumnProp = to;
}

static void insertColumn(App& app, int at, const Column* copyFrom) {
    Project& p = app.project;
    app.checkpoint();
    at = std::clamp(at, 0, (int)p.comp.columns.size());
    Column c = copyFrom ? *copyFrom : Column{std::string(T("Coluna ")) + std::to_string(p.comp.columns.size() + 1), 0};
    for (auto& l : p.comp.layers) {
        Clip nc;
        if (copyFrom && at > 0 && at - 1 < (int)l.clips.size()) { nc = l.clips[at - 1]; if (!nc.empty()) nc.id = p.newId(); }
        l.clips.insert(l.clips.begin() + std::min(at, (int)l.clips.size()), nc);
    }
    p.comp.columns.insert(p.comp.columns.begin() + at, c);
    app.comp.remapColumns(p, [&](int x) { return x >= at ? x + 1 : x; });
    remapCues(app, [&](int x) { return x >= at ? x + 1 : x; });
    app.ui.selCells.clear();
}

static void deleteColumn(App& app, int c) {
    Project& p = app.project;
    if ((int)p.comp.columns.size() <= 1) return;
    app.checkpoint();
    app.comp.remapColumns(p, [&](int x) { return x == c ? -1 : (x > c ? x - 1 : x); });
    remapCues(app, [&](int x) { return x == c ? -1 : (x > c ? x - 1 : x); });
    for (auto& l : p.comp.layers) if (c < (int)l.clips.size()) l.clips.erase(l.clips.begin() + c);
    p.comp.columns.erase(p.comp.columns.begin() + c);
    app.ui.selCells.clear();
    app.ui.selColumnProp = std::min(app.ui.selColumnProp, (int)p.comp.columns.size() - 1);
}

// ---------------------------------------------------------------------------
// copiar / colar / apagar seleção
// ---------------------------------------------------------------------------
static void copySelection(App& app, bool cut) {
    Project& p = app.project;
    auto& sel = app.ui.selCells;
    if (sel.empty() && app.ui.selLayer >= 0 && app.ui.selLayer < (int)p.comp.layers.size() && app.ui.selColumn >= 0)
        sel.insert({p.comp.layers[app.ui.selLayer].id, app.ui.selColumn});
    int minL = 1 << 30, minC = 1 << 30;
    std::vector<std::pair<int, int>> cells;
    for (auto& [lid, c] : sel) {
        int li = p.layerIndex(lid);
        if (li < 0 || c >= (int)p.comp.layers[li].clips.size() || p.comp.layers[li].clips[c].empty()) continue;
        cells.push_back({li, c});
        minL = std::min(minL, li); minC = std::min(minC, c);
    }
    if (cells.empty()) return;
    app.ui.clipboard.clear();
    for (auto& [li, c] : cells) app.ui.clipboard.push_back({li - minL, c - minC, p.comp.layers[li].clips[c]});
    if (cut) {
        app.checkpoint();
        for (auto& [li, c] : cells) {
            if (p.comp.layers[li].locked) continue;
            if (p.comp.layers[li].activeColumn == c) app.comp.stopLayer(p, li);
            p.comp.layers[li].clips[c] = Clip{};
        }
    }
    PS_INFO("%d clip(s) %s", (int)cells.size(), cut ? "cortado(s)" : "copiado(s)");
}

static void pasteClipboard(App& app) {
    Project& p = app.project;
    if (app.ui.clipboard.empty()) return;
    int L = std::max(0, app.ui.selLayer), C = std::max(0, app.ui.selColumn);
    app.checkpoint();
    app.ui.selCells.clear();
    int pasted = 0;
    for (auto& cc : app.ui.clipboard) {
        int li = L + cc.dl, c = C + cc.dc;
        if (li < 0 || li >= (int)p.comp.layers.size() || p.comp.layers[li].locked) continue;
        p.ensureColumns(c + 1);
        Clip n = cc.clip;
        n.id = p.newId();
        p.comp.layers[li].clips[c] = n;
        app.ui.selCells.insert({p.comp.layers[li].id, c});
        ++pasted;
    }
    PS_INFO("%d clip(s) colado(s)", pasted);
}

// definições de clip: tudo menos o ficheiro, o nome, o trim e a hora de timecode
void copyClipSettings(App& app, const Clip& c) {
    app.ui.clipSettings = c;
    app.ui.hasClipSettings = true;
    PS_INFO("%s", T("Definições do clip copiadas"));
}

int pasteClipSettings(App& app) {
    if (!app.ui.hasClipSettings) return 0;
    Project& p = app.project;
    std::set<std::pair<uint32_t, int>> cells = app.ui.selCells;
    if (cells.empty() && app.ui.selLayer >= 0 && app.ui.selLayer < (int)p.comp.layers.size() && app.ui.selColumn >= 0)
        cells.insert({p.comp.layers[app.ui.selLayer].id, app.ui.selColumn});
    const Clip& s = app.ui.clipSettings;
    int n = 0;
    bool cp = false;
    for (auto& [lid, ci] : cells) {
        int li = p.layerIndex(lid);
        if (li < 0 || p.comp.layers[li].locked || ci < 0 || ci >= (int)p.comp.layers[li].clips.size()) continue;
        Clip& d = p.comp.layers[li].clips[ci];
        if (d.empty()) continue;
        if (!cp) { app.checkpoint(); cp = true; }
        d.loop = s.loop; d.speed = s.speed; d.opacity = s.opacity; d.fit = s.fit;
        d.offsetX = s.offsetX; d.offsetY = s.offsetY; d.scale = s.scale; d.scaleX = s.scaleX; d.scaleY = s.scaleY;
        d.rotation = s.rotation; d.flipH = s.flipH; d.flipV = s.flipV; d.effects = s.effects; d.tcMode = s.tcMode;
        ++n;
    }
    if (n) { app.markDirty(); PS_INFO(T("Definições coladas em %d clip(s)"), n); }
    return n;
}

static void deleteSelection(App& app) {
    Project& p = app.project;
    if (app.ui.selCells.empty()) return;
    app.checkpoint();
    for (auto& [lid, c] : app.ui.selCells) {
        int li = p.layerIndex(lid);
        if (li < 0 || c >= (int)p.comp.layers[li].clips.size() || p.comp.layers[li].locked) continue;
        if (p.comp.layers[li].activeColumn == c) app.comp.stopLayer(p, li);
        p.comp.layers[li].clips[c] = Clip{};
    }
    app.ui.selCells.clear();
}

// ---------------------------------------------------------------------------
// cabeçalho de layer
// ---------------------------------------------------------------------------
static void layerMenu(App& app, int li) {
    Project& p = app.project;
    Layer& l = p.comp.layers[li];
    ImGui::TextDisabled("%s", l.name.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem(T("Nova layer"))) { app.checkpoint(); p.addLayer("Layer " + std::to_string(p.comp.layers.size() + 1)); app.ui.selLayer = 0; return; }
    if (ImGui::MenuItem(T("Inserir acima"))) { app.checkpoint(); p.insertLayer(li, "Layer " + std::to_string(p.comp.layers.size() + 1)); app.ui.selLayer = li; return; }
    if (ImGui::MenuItem(T("Inserir abaixo"))) { app.checkpoint(); p.insertLayer(li + 1, "Layer " + std::to_string(p.comp.layers.size() + 1)); app.ui.selLayer = li + 1; return; }
    if (ImGui::MenuItem(T("Duplicar"))) {
        app.checkpoint();
        Layer copy = p.comp.layers[li]; copy.id = p.newId(); copy.name += T(" (cópia)"); copy.activeColumn = -1;
        for (auto& c : copy.clips) if (!c.empty()) c.id = p.newId();
        p.comp.layers.insert(p.comp.layers.begin() + li, copy);
        return;
    }
    if (ImGui::MenuItem(T("Renomear"))) app.ui.renameLayer = l.id;
    ImGui::Separator();
    if (ImGui::MenuItem(T("Copiar efeitos"), nullptr, false, !l.effects.empty())) app.ui.fxClipboard = l.effects;
    if (ImGui::MenuItem(T("Colar efeitos"), nullptr, false, !app.ui.fxClipboard.empty())) {
        app.checkpoint(); l.effects.insert(l.effects.end(), app.ui.fxClipboard.begin(), app.ui.fxClipboard.end());
    }
    ImGui::Separator();
    if (ImGui::MenuItem(T("Mover para cima"), nullptr, false, li > 0)) { app.checkpoint(); std::swap(p.comp.layers[li], p.comp.layers[li - 1]); return; }
    if (ImGui::MenuItem(T("Mover para baixo"), nullptr, false, li + 1 < (int)p.comp.layers.size())) { app.checkpoint(); std::swap(p.comp.layers[li], p.comp.layers[li + 1]); return; }
    ImGui::Separator();
    if (ImGui::MenuItem(T("Limpar clips"), nullptr, false, !l.locked)) { app.checkpoint(); app.comp.stopLayer(p, li); for (auto& c : p.comp.layers[li].clips) c = Clip{}; }
    if (ImGui::MenuItem(T("Remover"), nullptr, false, p.comp.layers.size() > 1)) {
        app.checkpoint();
        app.comp.stopLayer(p, li);
        for (auto& s : p.screens) for (auto& sl : s.slices) if (sl.sourceLayer == l.id) sl.sourceLayer = 0;
        p.comp.layers.erase(p.comp.layers.begin() + li);
        app.ui.selLayer = std::min(app.ui.selLayer, (int)p.comp.layers.size() - 1);
        return;
    }
    ImGui::Separator();
    if (ImGui::MenuItem(T("Ignorar disparo de coluna"), nullptr, l.ignoreColumn)) { app.checkpoint(); l.ignoreColumn = !l.ignoreColumn; }
    if (ImGui::MenuItem(T("Bloquear conteúdo"), nullptr, l.locked)) { app.checkpoint(); l.locked = !l.locked; }
    ImGui::Separator();
    colorSwatches(app, l.color);
}

static void layerHeader(App& app, int li, float rowH) {
    Project& p = app.project;
    Layer& l = p.comp.layers[li];
    ImGui::PushID("layer");
    ImGui::PushID((int)l.id);
    bool sel = app.ui.selLayer == li;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = l.color ? tintU32(l.color, sel ? 150 : 95) : (sel ? IM_COL32(36, 70, 64, 255) : IM_COL32(34, 36, 40, 255));
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + rowH - 2), bg, 3);
    if (l.activeColumn >= 0) dl->AddRectFilled(p0, ImVec2(p0.x + 3, p0.y + rowH - 2), col::Playing);

    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p0.y + 4));
    if (app.ui.renameLayer == l.id) {
        ImGui::SetNextItemWidth(w - 14);
        if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
        char buf[128]; snprintf(buf, sizeof(buf), "%s", l.name.c_str());
        if (ImGui::InputText("##rename", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            app.checkpoint(); l.name = buf; app.ui.renameLayer = 0;
        }
        if (ImGui::IsItemDeactivated()) app.ui.renameLayer = 0;
    } else {
        if (ImGui::InvisibleButton("##sel", ImVec2(w - 8, 18))) app.ui.selLayer = li;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) app.ui.renameLayer = l.id;
        if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        // arrastar a layer pelo nome para mudar a ordem
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
            int pl[2] = {li, 0};
            ImGui::SetDragDropPayload("PS_LAYER", pl, sizeof(pl));
            ImVec2 gp = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(200, 26));
            ImDrawList* td = ImGui::GetWindowDrawList();
            td->AddRectFilled(gp, ImVec2(gp.x + 200, gp.y + 26), l.color ? tintU32(l.color, 200) : IM_COL32(40, 110, 96, 230), 3);
            td->AddText(ImVec2(gp.x + 8, gp.y + 5), IM_COL32(255, 255, 255, 255), l.name.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 8, p0.y + 4));
        ImGui::TextUnformatted(l.name.c_str());
        std::string info;
        if (l.locked) info += std::string(T("bloqueada")) + " ";
        if (l.ignoreColumn) info += std::string(T("sem coluna")) + " ";
        if (l.useRegion) { char reg[64]; snprintf(reg, sizeof(reg), "%.0fx%.0f", l.region.w, l.region.h); info += reg; }
        if (!info.empty()) {
            ImVec2 ts = ImGui::CalcTextSize(info.c_str());
            dl->AddText(ImVec2(p0.x + w - ts.x - 8, p0.y + 4), col::TextDim, info.c_str());
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p0.y + 26));
    float bs = 22;
    if (ImGui::Button("X", ImVec2(bs, bs))) app.comp.stopLayer(p, li);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Parar layer"));
    ImGui::SameLine(0, 3);
    ImGui::PushStyleColor(ImGuiCol_Button, l.bypass ? ImVec4(0.75f, 0.3f, 0.3f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::Button("B", ImVec2(bs, bs))) { app.checkpoint(); l.bypass = !l.bypass; }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Bypass (esconder)"));
    ImGui::SameLine(0, 3);
    ImGui::PushStyleColor(ImGuiCol_Button, l.solo ? ImVec4(0.85f, 0.7f, 0.2f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::Button("S", ImVec2(bs, bs))) { app.checkpoint(); l.solo = !l.solo; }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Solo"));
    ImGui::SameLine(0, 6);
    ImGui::SetNextItemWidth(std::max(40.f, w - (bs + 3) * 3 - 18));
    valueSlider(app, "##op", &l.opacity, 0, 1, 1, "%.2f");

    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8, p0.y + 54));
    const char* blends[] = {"Normal", "Add", "Multiply", "Screen"};
    if (l.effects.empty()) ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.62f, 1), "%s  ·  fade %.1fs", blends[(int)l.blend], l.fadeTime);
    else ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.62f, 1), T("%s  ·  fade %.1fs  ·  %d efeito(s)"), blends[(int)l.blend], l.fadeTime, (int)l.effects.size());

    // área inteira: menu de contexto + largar clips, ficheiros e efeitos
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##ctx", ImVec2(w, rowH - 2), ImGuiButtonFlags_MouseButtonRight);
    app.ui.cells.push_back({li, -1, p0.x, p0.y, p0.x + w, p0.y + rowH - 2});
    if (isDragging("PS_LAYER", li)) dimSource(p0, ImVec2(p0.x + w, p0.y + rowH - 2));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PS_LAYER", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            int from = ((const int*)pl->Data)[0];
            if (from != li) {
                float y = from < li ? p0.y + rowH : p0.y - 2;     // cai por baixo / por cima desta layer
                float gridW = ImGui::GetWindowContentRegionMax().x + ImGui::GetWindowPos().x - p0.x;
                insertLine(ImVec2(p0.x, y), ImVec2(p0.x + std::max(w, gridW), y));
            }
            if (pl->IsDelivery()) moveLayer(app, from, li);
        }
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PS_CLIP")) {
            CellPayload src = *(const CellPayload*)pl->Data;
            if (src.layer != li && !l.locked) {
                app.checkpoint();
                int dc = firstFreeColumn(p, li);
                Clip moved = p.comp.layers[src.layer].clips[src.col];
                if (ImGui::GetIO().KeyCtrl) moved.id = p.newId(); else p.comp.layers[src.layer].clips[src.col] = Clip{};
                p.comp.layers[li].clips[dc] = moved;
                app.ui.selLayer = li; app.ui.selColumn = dc;
            }
        }
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PS_FX")) {
            int idx = *(const int*)pl->Data;
            if (idx >= 0 && idx < (int)effectCatalog().size()) { app.checkpoint(); l.effects.push_back(makeEffect(effectCatalog()[idx].id)); app.ui.selLayer = li; }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("layerctx")) {
        app.ui.selLayer = li;
        layerMenu(app, li);
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// célula de clip
// ---------------------------------------------------------------------------
static void clipCell(App& app, int li, int ci, float cellW, float cellH) {
    Project& p = app.project;
    Layer& l = p.comp.layers[li];
    Clip& c = l.clips[ci];
    ImGui::PushID("cell");
    ImGui::PushID((int)l.id);
    ImGui::PushID(ci);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1(p0.x + cellW, p0.y + cellH);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    app.ui.cells.push_back({li, ci, p0.x, p0.y, p1.x, p1.y});

    ImGuiIO& io = ImGui::GetIO();
    bool active = l.activeColumn == ci && !c.empty();
    bool inPreview = !c.empty() && app.comp.previewLayerId() == l.id && app.comp.previewColumn() == ci && app.comp.previewStack() == app.project.comp.activeStack;
    bool multiSel = app.ui.selCells.count({l.id, ci}) > 0;
    bool sel = (app.ui.selLayer == li && app.ui.selColumn == ci) || multiSel;
    ImGui::InvisibleButton("##cell", ImVec2(cellW, cellH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    bool topHalf = io.MousePos.y < p0.y + cellH * 0.5f;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (io.KeyCtrl) {                       // alternar na seleção (não dispara)
            auto key = std::make_pair(l.id, ci);
            if (app.ui.selCells.count(key)) app.ui.selCells.erase(key); else app.ui.selCells.insert(key);
            app.ui.anchorLayer = li; app.ui.anchorCol = ci;
        } else if (io.KeyShift && app.ui.anchorLayer >= 0) {   // intervalo retangular
            app.ui.selCells.clear();
            int l0 = std::min(li, app.ui.anchorLayer), l1 = std::max(li, app.ui.anchorLayer);
            int c0 = std::min(ci, app.ui.anchorCol), c1 = std::max(ci, app.ui.anchorCol);
            for (int a = l0; a <= l1 && a < (int)p.comp.layers.size(); ++a)
                for (int b = c0; b <= c1; ++b) app.ui.selCells.insert({p.comp.layers[a].id, b});
        } else {
            app.ui.selCells.clear();
            app.ui.anchorLayer = li; app.ui.anchorCol = ci;
            if (!c.empty()) {
                if (topHalf) app.comp.trigger(p, li, ci);
                else app.comp.previewClip(p, li, ci);
            }
        }
        app.ui.selLayer = li; app.ui.selColumn = ci; app.ui.selColumnProp = ci;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && c.empty() && !l.locked) {
        auto files = app.pickVideoFiles();
        if (!files.empty()) {
            app.checkpoint();
            for (size_t i = 0; i < files.size(); ++i) assignClip(app, li, ci + (int)i, files[i]);
        }
    }

    dl->AddRectFilled(p0, p1, c.empty() ? IM_COL32(30, 31, 35, 255) : IM_COL32(18, 18, 20, 255), 3);
    if (l.color) dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + 3), tintU32(l.color, 220), 3);
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
        dl->AddRectFilled(ImVec2(p0.x, p1.y - 20), p1, IM_COL32(0, 0, 0, 170), 3);
        dl->PushClipRect(ImVec2(p0.x + 2, p1.y - 20), ImVec2(p1.x - 2, p1.y), true);
        std::string nm = c.effects.empty() ? c.name : "fx · " + c.name;
        dl->AddText(ImVec2(p0.x + 5, p1.y - 18), IM_COL32(230, 230, 230, 255), nm.c_str());
        dl->PopClipRect();
        if (active) {
            if (VideoPlayer* v = app.comp.activePlayer(l.id)) {
                MediaInfo mi = v->info();
                if (mi.duration > 0) {
                    double pos = std::fmod(std::max(0.0, v->position()), mi.duration) / mi.duration;
                    dl->AddRectFilled(ImVec2(p0.x, p1.y - 3), ImVec2(p0.x + (float)(cellW * pos), p1.y), col::Playing);
                }
                if (v->state() == VideoPlayer::State::Error) dl->AddText(ImVec2(p0.x + 6, p0.y + 6), col::Error, T("ERRO"));
            }
        }
    } else {
        dl->AddText(ImVec2(p0.x + cellW * 0.5f - 4, p0.y + cellH * 0.5f - 8), IM_COL32(70, 72, 78, 255), "+");
    }
    if (multiSel) dl->AddRectFilled(p0, p1, IM_COL32(64, 224, 192, 45), 3);
    if (hovered && !c.empty() && !io.KeyCtrl && !io.KeyShift && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
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
    if (inPreview) dl->AddText(ImVec2(p0.x + 5, p0.y + 4), IM_COL32(240, 170, 40, 255), "PV");
    if (!c.empty() && c.tcMode != TcMode::Off) {
        // etiqueta TC no canto (azul = segue; laranja = dispara)
        const char* tg = "TC";
        ImVec2 ts = ImGui::CalcTextSize(tg);
        ImVec2 q(p1.x - ts.x - 9, p0.y + 4);
        dl->AddRectFilled(ImVec2(q.x - 3, q.y - 1), ImVec2(q.x + ts.x + 3, q.y + ts.y), tcTriggers(c.tcMode) ? IM_COL32(220, 130, 30, 230) : IM_COL32(40, 120, 220, 230), 3);
        dl->AddText(q, IM_COL32(255, 255, 255, 255), tg);
        if (hovered) ImGui::SetTooltip(T("Timecode: %s"), tcFormat(c.tcStart, Timecode::get().status().fps).c_str());
    }

    // arrastar clips entre células (Ctrl = copiar)
    if (!c.empty() && !l.locked && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
        CellPayload pl{li, ci};
        ImGui::SetDragDropPayload("PS_CLIP", &pl, sizeof(pl));
        ImVec2 gp = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(cellW, cellH));
        ImDrawList* td = ImGui::GetWindowDrawList();
        td->AddRectFilled(gp, ImVec2(gp.x + cellW, gp.y + cellH), IM_COL32(18, 18, 20, 240), 3);
        auto th = ThumbnailCache::get().request(c.path);
        if (th->srv) {
            float s = std::min((cellW - 4) / th->w, (cellH - 22) / th->h);
            ImVec2 i0(gp.x + 2 + (cellW - 4 - th->w * s) * 0.5f, gp.y + 2);
            td->AddImage(texId(th->srv.Get()), i0, ImVec2(i0.x + th->w * s, i0.y + th->h * s), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 230));
        }
        td->AddText(ImVec2(gp.x + 5, gp.y + cellH - 18), IM_COL32(230, 230, 230, 255), c.name.c_str());
        td->AddRect(gp, ImVec2(gp.x + cellW, gp.y + cellH), col::Accent, 3, 0, 2.f);
        if (io.KeyCtrl) td->AddText(ImVec2(gp.x + cellW - 18, gp.y + 4), col::Accent, "+");
        ImGui::EndDragDropSource();
    }
    if (isDragging("PS_CLIP", li, ci)) dimSource(p0, p1);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PS_CLIP", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            CellPayload src = *(const CellPayload*)pl->Data;
            if (!(src.layer == li && src.col == ci) && !pl->IsDelivery()) {
                float t = (float)ImGui::GetTime();
                int a = 160 + (int)(80 * std::sin(t * 10.f));
                ImGui::GetForegroundDrawList()->AddRect(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2), IM_COL32(64, 224, 192, a), 4, 0, 3.f);
                if (!c.empty()) ImGui::GetForegroundDrawList()->AddText(ImVec2(p0.x + 6, p0.y + 6), IM_COL32(255, 255, 255, 220), io.KeyCtrl ? T("substituir") : T("trocar"));
            }
            if (pl->IsDelivery() && !(src.layer == li && src.col == ci) && !l.locked) {
                app.checkpoint();
                Clip& from = p.comp.layers[src.layer].clips[src.col];
                if (io.KeyCtrl) { Clip cp = from; cp.id = p.newId(); c = cp; }
                else std::swap(from, c);
                app.ui.selLayer = li; app.ui.selColumn = ci;
            }
        }
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PS_FX")) {
            int idx = *(const int*)pl->Data;
            if (!c.empty() && idx >= 0 && idx < (int)effectCatalog().size()) {
                app.checkpoint(); c.effects.push_back(makeEffect(effectCatalog()[idx].id));
                app.ui.selLayer = li; app.ui.selColumn = ci;
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem("cellctx")) {
        if (!multiSel) app.ui.selCells.clear();
        app.ui.selLayer = li; app.ui.selColumn = ci;
        if (ImGui::MenuItem(T("Pôr no programa"), nullptr, false, !c.empty())) app.comp.trigger(p, li, ci);
        if (ImGui::MenuItem(T("Pôr em preview"), nullptr, false, !c.empty())) app.comp.previewClip(p, li, ci);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Escolher vídeo(s)..."), nullptr, false, !l.locked)) {
            auto files = app.pickVideoFiles();
            if (!files.empty()) { app.checkpoint(); for (size_t i = 0; i < files.size(); ++i) assignClip(app, li, ci + (int)i, files[i]); }
        }
        if (ImGui::MenuItem(T("Copiar"), "Ctrl+C", false, !c.empty() || !app.ui.selCells.empty())) copySelection(app, false);
        if (ImGui::MenuItem(T("Cortar"), "Ctrl+X", false, (!c.empty() || !app.ui.selCells.empty()) && !l.locked)) copySelection(app, true);
        if (ImGui::MenuItem(T("Colar"), "Ctrl+V", false, !app.ui.clipboard.empty() && !l.locked)) pasteClipboard(app);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Copiar definições"), "Ctrl+Shift+C", false, !c.empty())) copyClipSettings(app, c);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Repetição, velocidade, opacidade, encaixe, transformar, efeitos e modo de timecode."));
        if (ImGui::MenuItem(T("Colar definições"), "Ctrl+Shift+V", false, app.ui.hasClipSettings && !c.empty() && !l.locked)) pasteClipSettings(app);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Copiar efeitos"), nullptr, false, !c.effects.empty())) app.ui.fxClipboard = c.effects;
        if (ImGui::MenuItem(T("Colar efeitos"), nullptr, false, !c.empty() && !app.ui.fxClipboard.empty())) {
            app.checkpoint(); c.effects.insert(c.effects.end(), app.ui.fxClipboard.begin(), app.ui.fxClipboard.end());
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T("Remover clip"), "Del", false, !c.empty() && !l.locked)) {
            if (multiSel) deleteSelection(app);
            else {
                app.checkpoint();
                if (l.activeColumn == ci) app.comp.stopLayer(p, li);
                c = Clip{};
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// cabeçalho de coluna
// ---------------------------------------------------------------------------
// devolve false se a coluna foi apagada (parar de desenhar esta coluna)
static bool columnHeader(App& app, int c, float cellW) {
    Project& p = app.project;
    Column& col = p.comp.columns[c];
    ImGui::PushID("colhdr");
    ImGui::PushID(c);
    bool alive = true;
    bool anyActive = false;
    for (auto& l : p.comp.layers) if (l.activeColumn == c) anyActive = true;
    ImVec4 bc = anyActive ? ImVec4(0.2f, 0.5f, 0.3f, 1)
              : col.color ? ImVec4((col.color & 0xFF) / 255.f * 0.8f, ((col.color >> 8) & 0xFF) / 255.f * 0.8f, ((col.color >> 16) & 0xFF) / 255.f * 0.8f, 1)
              : ImGui::GetStyleColorVec4(ImGuiCol_Button);
    if (app.ui.renameColumn == c) {
        ImGui::SetNextItemWidth(cellW - 4);
        if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
        char buf[128]; snprintf(buf, sizeof(buf), "%s", col.name.c_str());
        if (ImGui::InputText("##rn", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            app.checkpoint(); col.name = buf; app.ui.renameColumn = -1;
        }
        if (ImGui::IsItemDeactivated()) app.ui.renameColumn = -1;
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, bc);
        bool selc = app.ui.selColumnProp == c;
        if (selc) ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
        if (ImGui::Button(col.name.c_str(), ImVec2(cellW - 4, 0))) { app.comp.triggerColumn(p, c); app.ui.selColumnProp = c; }
        if (selc) ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) app.ui.renameColumn = c;
        if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            ImGui::SetTooltip("%s", T("Clique: pôr a coluna inteira no ar · Arrastar: mudar a ordem · Duplo clique: renomear"));
        ImVec2 h0 = ImGui::GetItemRectMin(), h1 = ImGui::GetItemRectMax();
        {
            std::string qs;
            for (auto& q : p.comp.cues) if (q.stack == p.comp.activeStack && q.column == c) { if (!qs.empty()) qs += ","; qs += q.number; }
            if (!qs.empty()) {
                qs = "Q" + qs;
                ImDrawList* d = ImGui::GetWindowDrawList();
                ImVec2 ts = ImGui::CalcTextSize(qs.c_str());
                ImVec2 q0(h0.x + 3, h0.y + (h1.y - h0.y - ts.y) * 0.5f);
                d->AddRectFilled(ImVec2(q0.x - 2, q0.y), ImVec2(q0.x + ts.x + 2, q0.y + ts.y), IM_COL32(30, 110, 70, 230), 3);
                d->AddText(q0, IM_COL32(255, 255, 255, 255), qs.c_str());
            }
            if (col.tcTrigger) {
                ImDrawList* d = ImGui::GetWindowDrawList();
                ImVec2 ts = ImGui::CalcTextSize("TC");
                ImVec2 q0(h1.x - ts.x - 5, h0.y + (h1.y - h0.y - ts.y) * 0.5f);
                d->AddRectFilled(ImVec2(q0.x - 2, q0.y), ImVec2(q0.x + ts.x + 2, q0.y + ts.y), IM_COL32(220, 130, 30, 230), 3);
                d->AddText(q0, IM_COL32(255, 255, 255, 255), "TC");
            }
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
            int pl[2] = {c, 0};
            ImGui::SetDragDropPayload("PS_COL", pl, sizeof(pl));
            ImVec2 gp = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(cellW, 24));
            ImGui::GetWindowDrawList()->AddRectFilled(gp, ImVec2(gp.x + cellW, gp.y + 24), ImGui::GetColorU32(bc), 3);
            ImGui::GetWindowDrawList()->AddText(ImVec2(gp.x + 8, gp.y + 4), IM_COL32(255, 255, 255, 255), col.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (isDragging("PS_COL", c)) {
            // esbater a coluna inteira (cabeçalho + células)
            float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
            ImGui::GetForegroundDrawList()->AddRectFilled(h0, ImVec2(h1.x, bottom), IM_COL32(10, 10, 12, 120), 3);
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PS_COL", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                int from = ((const int*)pl->Data)[0];
                if (from != c) {
                    float x = from < c ? h1.x + 3 : h0.x - 3;
                    float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - 8;
                    insertLine(ImVec2(x, h0.y), ImVec2(x, bottom));
                }
                if (pl->IsDelivery()) moveColumn(app, from, c);
            }
            ImGui::EndDragDropTarget();
        }
    }
    if (ImGui::BeginPopupContextItem("colctx")) {
        app.ui.selColumnProp = c;
        ImGui::TextDisabled("%s", p.comp.columns[c].name.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(T("Pôr no ar"))) app.comp.triggerColumn(p, c);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Inserir antes"))) insertColumn(app, c, nullptr);
        else if (ImGui::MenuItem(T("Inserir depois"))) insertColumn(app, c + 1, nullptr);
        else if (ImGui::MenuItem(T("Duplicar"))) { Column cp = p.comp.columns[c]; cp.name += T(" (cópia)"); insertColumn(app, c + 1, &cp); }
        else if (ImGui::MenuItem(T("Renomear"))) app.ui.renameColumn = c;
        ImGui::Separator();
        if (ImGui::MenuItem(T("Mover para a esquerda"), nullptr, false, c > 0)) moveColumn(app, c, c - 1);
        else if (ImGui::MenuItem(T("Mover para a direita"), nullptr, false, c + 1 < (int)p.comp.columns.size())) moveColumn(app, c, c + 1);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Remover"), nullptr, false, p.comp.columns.size() > 1)) { deleteColumn(app, c); alive = false; }
        if (alive && c < (int)p.comp.columns.size()) {
            ImGui::Separator();
            colorSwatches(app, p.comp.columns[c].color);
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::PopID();
    return alive;
}

// abas das stacks (como os Decks do Resolume)
static void stackTabs(App& app) {
    Project& p = app.project;
    static int renameIdx = -1, dragFrom = -1;
    bool openRename = false;
    static char nameBuf[128];
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    int n = (int)p.comp.stacks.size();
    int openMenu = -1;
    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        bool act = i == p.comp.activeStack;
        bool live = false;     // há algo desta stack no ar?
        if (!act) for (auto& l : p.comp.layers) if (const LayerRuntime* r = app.comp.runtime(l.id); r && r->cur && r->stack == i) { live = true; break; }
        ImGui::PushStyleColor(ImGuiCol_Button, act ? ImVec4(0.13f, 0.55f, 0.47f, 1) : ImVec4(0.17f, 0.18f, 0.2f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, act ? ImVec4(0.16f, 0.62f, 0.53f, 1) : ImVec4(0.24f, 0.25f, 0.28f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, act ? ImVec4(1, 1, 1, 1) : ImVec4(0.75f, 0.76f, 0.8f, 1));
        std::string label = p.comp.stacks[i].name;
        if (live) label = "● " + label;
        if (ImGui::Button(label.c_str())) app.selectStack(i);
        ImGui::PopStyleColor(3);
        if (live && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Há clips desta stack no ar."));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            renameIdx = i; openRename = true;
            snprintf(nameBuf, sizeof(nameBuf), "%s", p.comp.stacks[i].name.c_str());
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) openMenu = i;
        // arrastar para mudar a ordem
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            dragFrom = i;
            ImGui::SetDragDropPayload("PS_STACK", &i, sizeof(int));
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            float x = dragFrom >= 0 && dragFrom < i ? b.x + 2 : a.x - 2;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), IM_COL32(64, 224, 190, 255), 3.0f);
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("PS_STACK", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                int from = *(const int*)pl->Data;
                if (from != i) { app.checkpoint(); p.moveStack(from, i); app.dirty = true; }
                dragFrom = -1;
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    if (ImGui::Button("+##newstack")) app.newStack(false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Nova stack"));
    ImGui::PopStyleVar(2);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragFrom = -1;

    static int menuIdx = -1;
    if (openMenu >= 0) { menuIdx = openMenu; ImGui::OpenPopup("##stackmenu"); }
    if (ImGui::BeginPopup("##stackmenu")) {
        int i = menuIdx;
        if (i >= 0 && i < (int)p.comp.stacks.size()) {
            ImGui::TextDisabled("%s", p.comp.stacks[i].name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem(T("Mudar o nome"))) { renameIdx = i; openRename = true; snprintf(nameBuf, sizeof(nameBuf), "%s", p.comp.stacks[i].name.c_str()); }
            if (ImGui::MenuItem(T("Nova stack"))) { app.selectStack(i); app.newStack(false); }
            if (ImGui::MenuItem(T("Duplicar"))) { app.selectStack(i); app.newStack(true); }
            if (ImGui::MenuItem(T("Mover para a esquerda"), nullptr, false, i > 0)) { app.checkpoint(); p.moveStack(i, i - 1); app.dirty = true; }
            if (ImGui::MenuItem(T("Mover para a direita"), nullptr, false, i + 1 < (int)p.comp.stacks.size())) { app.checkpoint(); p.moveStack(i, i + 1); app.dirty = true; }
            ImGui::Separator();
            if (ImGui::MenuItem(T("Apagar stack"), nullptr, false, p.comp.stacks.size() > 1)) {
                app.checkpoint();
                p.removeStack(i);
                app.ui.selCells.clear();
                app.ui.selColumn = std::min(app.ui.selColumn, (int)p.comp.columns.size() - 1);
                app.ui.selColumnProp = std::min(app.ui.selColumnProp, (int)p.comp.columns.size() - 1);
                app.dirty = true;
            }
        }
        ImGui::EndPopup();
    }
    if (openRename) ImGui::OpenPopup("##stackrename");
    if (ImGui::BeginPopup("##stackrename")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(220);
        bool ok = ImGui::InputText("##n", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::SameLine();
        ok |= ImGui::Button(T("OK"));
        if (ok) {
            if (renameIdx < (int)p.comp.stacks.size() && nameBuf[0]) { app.checkpoint(); p.comp.stacks[renameIdx].name = nameBuf; app.dirty = true; }
            renameIdx = -1;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { renameIdx = -1; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    } else renameIdx = -1;
    ImGui::Separator();
}

void drawClipGrid(App& app) {
    Project& p = app.project;
    app.ui.cells.clear();
    if (!app.ui.panel.clips) return;

    ImGui::SetNextWindowSize(ImVec2(900, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(winTitle("Stack", "clips").c_str(), &app.ui.panel.clips)) { ImGui::End(); return; }

    stackTabs(app);

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
    if (!app.ui.selCells.empty()) ImGui::TextColored(ImVec4(0.25f, 0.88f, 0.75f, 1), T("  %d selecionado(s) · Ctrl+C copiar · Ctrl+V colar · Del apagar"), (int)app.ui.selCells.size());
    else ImGui::TextDisabled("%s", T("  Cima = programa · baixo = preview · Ctrl/Shift+clique = selecionar vários"));

    // atalhos da grelha (só com a janela de clips ativa)
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
        const bool sh = io.KeyShift;
        if (io.KeyCtrl && sh && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            int li = app.ui.selLayer, ci = app.ui.selColumn;
            if (li >= 0 && li < (int)p.comp.layers.size() && ci >= 0 && ci < (int)p.comp.layers[li].clips.size() && !p.comp.layers[li].clips[ci].empty())
                copyClipSettings(app, p.comp.layers[li].clips[ci]);
        }
        if (io.KeyCtrl && sh && ImGui::IsKeyPressed(ImGuiKey_V, false)) pasteClipSettings(app);
        if (io.KeyCtrl && !sh && ImGui::IsKeyPressed(ImGuiKey_C, false)) copySelection(app, false);
        if (io.KeyCtrl && !sh && ImGui::IsKeyPressed(ImGuiKey_X, false)) copySelection(app, true);
        if (io.KeyCtrl && !sh && ImGui::IsKeyPressed(ImGuiKey_V, false)) pasteClipboard(app);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            if (app.ui.selCells.empty() && app.ui.selLayer >= 0 && app.ui.selLayer < (int)p.comp.layers.size() && app.ui.selColumn >= 0)
                app.ui.selCells.insert({p.comp.layers[app.ui.selLayer].id, app.ui.selColumn});
            deleteSelection(app);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !io.KeyShift) app.ui.selCells.clear();
    }

    const float headerW = 210, cellW = 132, cellH = 84;
    const int ncols = (int)p.comp.columns.size();
    ImGuiTableFlags tf = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                         ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
    if (ImGui::BeginTable("grid", ncols + 1, tf)) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Layers", ImGuiTableColumnFlags_WidthFixed, headerW);
        for (int c = 0; c < ncols; ++c) { ImGui::PushID(c); ImGui::TableSetupColumn("##c", ImGuiTableColumnFlags_WidthFixed, cellW); ImGui::PopID(); }

        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled(T("Composição %dx%d"), p.comp.width, p.comp.height);
        for (int c = 0; c < ncols && c < (int)p.comp.columns.size(); ++c) {
            ImGui::TableSetColumnIndex(c + 1);
            if (!columnHeader(app, c, cellW)) break;
        }

        for (int li = 0; li < (int)p.comp.layers.size(); ++li) {
            ImGui::TableNextRow(0, cellH + 6);
            ImGui::TableSetColumnIndex(0);
            layerHeader(app, li, cellH + 4);
            if (li >= (int)p.comp.layers.size()) break;
            for (int c = 0; c < (int)p.comp.columns.size() && c < ncols && c < (int)p.comp.layers[li].clips.size(); ++c) {
                ImGui::TableSetColumnIndex(c + 1);
                clipCell(app, li, c, cellW - 4, cellH);
            }
        }
        ImGui::EndTable();
    }

    // ficheiros largados do Explorador
    if (!app.pendingDrops.empty()) {
        bool placed = false;
        for (auto& cell : app.ui.cells) {
            if (app.dropX >= cell.x0 && app.dropX < cell.x1 && app.dropY >= cell.y0 && app.dropY < cell.y1) {
                if (cell.layer >= (int)p.comp.layers.size() || p.comp.layers[cell.layer].locked) break;
                app.checkpoint();
                int col = cell.column >= 0 ? cell.column : firstFreeColumn(p, cell.layer);
                for (size_t i = 0; i < app.pendingDrops.size(); ++i) assignClip(app, cell.layer, col + (int)i, app.pendingDrops[i]);
                app.ui.selLayer = cell.layer; app.ui.selColumn = col;
                PS_INFO("%d ficheiro(s) largado(s) na layer %d, coluna %d", (int)app.pendingDrops.size(), cell.layer + 1, col + 1);
                placed = true;
                break;
            }
        }
        if (!placed) PS_WARN("%s", T("Larga os vídeos em cima de uma célula ou do nome de uma layer."));
        app.pendingDrops.clear();
    }
    ImGui::End();
}

} // namespace ps
