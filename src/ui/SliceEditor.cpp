// Editor de Ecrãs e Slices — inspirado no Advanced Output do Resolume.
//   Separador "Entrada"  (Input Selection):     que zona da composição cada slice recorta
//   Separador "Saída"    (Output Transformation): onde essa zona aparece na saída da gráfica
// Tudo arrastável: mover, redimensionar pelos 8 pegas, rodar (só na saída),
// snap a bordas/centros, setas para afinar ao píxel, roda do rato para zoom.
#include "ui/UI.h"
#include "app/App.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "core/Log.h"

namespace ps {

namespace {

// Retângulo genérico (em píxeis do canvas) com rotação em volta do centro
struct ERect { float x, y, w, h, rot; };

ERect getRect(const Slice& s, int tab) {
    if (tab == 0) return {s.input.x, s.input.y, s.input.w, s.input.h, 0};
    return {s.outX - s.outW * 0.5f, s.outY - s.outH * 0.5f, s.outW, s.outH, s.rotation};
}

void setRect(Slice& s, int tab, const ERect& r) {
    if (tab == 0) { s.input = {r.x, r.y, r.w, r.h}; return; }
    s.outW = r.w; s.outH = r.h;
    s.outX = r.x + r.w * 0.5f; s.outY = r.y + r.h * 0.5f;
    s.rotation = r.rot;
}

ImVec2 rotateAround(ImVec2 p, ImVec2 c, float deg) {
    float a = deg * 3.14159265f / 180.f, cs = std::cos(a), sn = std::sin(a);
    float dx = p.x - c.x, dy = p.y - c.y;
    return ImVec2(c.x + dx * cs - dy * sn, c.y + dx * sn + dy * cs);
}

// cantos em coordenadas de canvas (TL, TR, BR, BL)
void corners(const ERect& r, ImVec2 out[4]) {
    ImVec2 c(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    ImVec2 raw[4] = {{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}};
    for (int i = 0; i < 4; ++i) out[i] = rotateAround(raw[i], c, r.rot);
}

bool pointInRect(const ERect& r, ImVec2 p) {
    ImVec2 c(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    ImVec2 l = rotateAround(p, c, -r.rot);
    return l.x >= r.x && l.x <= r.x + r.w && l.y >= r.y && l.y <= r.y + r.h;
}

// pegas: 0..7 = TL, T, TR, R, BR, B, BL, L  (sx, sy em -1/0/1)
const int kHx[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
const int kHy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};

ImVec2 handlePos(const ERect& r, int h) {
    ImVec2 c(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    ImVec2 p(c.x + kHx[h] * r.w * 0.5f, c.y + kHy[h] * r.h * 0.5f);
    return rotateAround(p, c, r.rot);
}

enum class Drag { None, Move, Resize, Rotate, Pan };

struct EditorState {
    Drag mode = Drag::None;
    int handle = -1;
    uint32_t slice = 0;
    ERect start{};
    ImVec2 mouseStart{};      // canvas
    float startAngle = 0;
    ImVec2 panStart{};
    std::vector<float> snapX, snapY;
    float guideX = NAN, guideY = NAN;
} g_ed;

struct View {
    ImVec2 origin;   // posição no ecrã do píxel (0,0) do canvas
    float zoom;
    ImVec2 toScreen(ImVec2 c) const { return ImVec2(origin.x + c.x * zoom, origin.y + c.y * zoom); }
    ImVec2 toCanvas(ImVec2 s) const { return ImVec2((s.x - origin.x) / zoom, (s.y - origin.y) / zoom); }
};

void collectSnaps(const Screen& sc, int tab, uint32_t except, float cw, float ch) {
    g_ed.snapX = {0, cw * 0.5f, cw};
    g_ed.snapY = {0, ch * 0.5f, ch};
    for (auto& s : sc.slices) {
        if (s.id == except || !s.enabled) continue;
        ERect r = getRect(s, tab);
        if (std::fmod(std::fabs(r.rot), 90.f) > 0.01f) continue;
        bool swap = std::fmod(std::fabs(r.rot), 180.f) > 89.f;
        float w = swap ? r.h : r.w, h = swap ? r.w : r.h;
        float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        g_ed.snapX.insert(g_ed.snapX.end(), {cx - w * 0.5f, cx, cx + w * 0.5f});
        g_ed.snapY.insert(g_ed.snapY.end(), {cy - h * 0.5f, cy, cy + h * 0.5f});
    }
}

// devolve o deslocamento para encaixar algum dos valores num alvo
float snapDelta(const std::vector<float>& vals, const std::vector<float>& targets, float thr, float& guide) {
    float best = thr, delta = 0; guide = NAN;
    for (float v : vals)
        for (float t : targets) {
            float d = t - v;
            if (std::fabs(d) < best) { best = std::fabs(d); delta = d; guide = t; }
        }
    return delta;
}

void label(ImDrawList* dl, ImVec2 center, const char* name, const char* dims, float rot, bool small) {
    ImVec2 ts1 = ImGui::CalcTextSize(name), ts2 = ImGui::CalcTextSize(dims);
    float w = std::max(ts1.x, ts2.x) + 10, h = small ? ts1.y + 6 : ts1.y + ts2.y + 8;
    (void)rot;
    ImVec2 p0(center.x - w * 0.5f, center.y - h * 0.5f);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(0, 0, 0, 200), 2);
    dl->AddText(ImVec2(center.x - ts1.x * 0.5f, p0.y + 3), IM_COL32(255, 255, 255, 255), name);
    if (!small) dl->AddText(ImVec2(center.x - ts2.x * 0.5f, p0.y + 5 + ts1.y), IM_COL32(220, 220, 220, 255), dims);
}

Screen* selectedScreen(App& app) {
    Project& p = app.project;
    Screen* s = p.findScreen(app.ui.selScreen);
    if (!s && !p.screens.empty()) { s = &p.screens[0]; app.ui.selScreen = s->id; }
    return s;
}

// ---------------------------------------------------------------------------
// lista de ecrãs e slices (esquerda)
// ---------------------------------------------------------------------------
void screenList(App& app) {
    Project& p = app.project;
    if (ImGui::Button("+ Ecrã", ImVec2(-1, 0))) {
        app.checkpoint();
        Screen& s = p.addScreen("Saída " + std::to_string(p.screens.size() + 1));
        app.ui.selScreen = s.id; app.ui.selSlice = 0;
    }
    ImGui::Separator();
    int delScreen = -1;
    for (int si = 0; si < (int)p.screens.size(); ++si) {
        Screen& s = p.screens[si];
        ImGui::PushID((int)s.id);
        bool selS = app.ui.selScreen == s.id;
        ImGuiTreeNodeFlags tf = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (selS && app.ui.selSlice == 0) tf |= ImGuiTreeNodeFlags_Selected;
        const char* dev = s.device == ScreenDevice::Monitor ? "" : s.device == ScreenDevice::Window ? "  [janela]" : "  [desligado]";
        bool open = ImGui::TreeNodeEx("##scr", tf, "%s%s", s.name.c_str(), dev);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) { app.ui.selScreen = s.id; app.ui.selSlice = 0; }
        if (ImGui::BeginPopupContextItem("scrctx")) {
            if (ImGui::MenuItem("Adicionar slice")) {
                app.checkpoint();
                Slice& sl = p.addSlice(s, "Slice " + std::to_string(s.slices.size() + 1));
                app.ui.selScreen = s.id; app.ui.selSlice = sl.id;
            }
            if (ImGui::MenuItem("Duplicar ecrã")) {
                app.checkpoint();
                Screen copy = s; copy.id = p.newId(); copy.name += " (cópia)"; copy.device = ScreenDevice::Disabled;
                for (auto& sl : copy.slices) sl.id = p.newId();
                p.screens.insert(p.screens.begin() + si + 1, copy);
            }
            if (ImGui::MenuItem("Apagar ecrã")) delScreen = si;
            ImGui::EndPopup();
        }
        if (open) {
            for (int k = 0; k < (int)s.slices.size(); ++k) {
                Slice& sl = s.slices[k];
                ImGui::PushID((int)sl.id);
                ImVec2 cp = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cp.x, cp.y + 3), ImVec2(cp.x + 10, cp.y + 13), sliceColorU32(sl.color, 255), 2);
                ImGui::SetCursorScreenPos(ImVec2(cp.x + 14, cp.y));
                bool en = sl.enabled;
                if (ImGui::Checkbox("##en", &en)) { app.checkpoint(); sl.enabled = en; }
                ImGui::SameLine();
                if (ImGui::Selectable(sl.name.c_str(), app.ui.selSlice == sl.id)) { app.ui.selScreen = s.id; app.ui.selSlice = sl.id; }
                // reordenar por arrasto
                if (ImGui::BeginDragDropSource()) {
                    int idx[2] = {si, k};
                    ImGui::SetDragDropPayload("PS_SLICE", idx, sizeof(idx));
                    ImGui::Text("%s", sl.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (auto* pl = ImGui::AcceptDragDropPayload("PS_SLICE")) {
                        int src[2]; memcpy(src, pl->Data, sizeof(src));
                        if (src[0] == si && src[1] != k) {
                            app.checkpoint();
                            Slice mv = s.slices[src[1]];
                            s.slices.erase(s.slices.begin() + src[1]);
                            s.slices.insert(s.slices.begin() + std::min(k, (int)s.slices.size()), mv);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::PopID();
            }
            if (ImGui::SmallButton("+ Slice")) {
                app.checkpoint();
                Slice& sl = p.addSlice(s, "Slice " + std::to_string(s.slices.size() + 1));
                app.ui.selScreen = s.id; app.ui.selSlice = sl.id;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (delScreen >= 0) {
        app.checkpoint();
        p.screens.erase(p.screens.begin() + delScreen);
        app.ui.selScreen = p.screens.empty() ? 0 : p.screens[0].id;
        app.ui.selSlice = 0;
    }
}

// ---------------------------------------------------------------------------
// propriedades (direita)
// ---------------------------------------------------------------------------
void screenProps(App& app, Screen& s) {
    ImGui::SeparatorText("Ecrã");
    inputTextStd("Nome##scr", s.name);
    undoOnActivate(app);

    // dispositivo: desligado / janela / saída física da gráfica
    std::string cur = s.device == ScreenDevice::Disabled ? "Desligado" : s.device == ScreenDevice::Window ? "Janela (teste)" : s.monitorKey;
    for (auto& d : app.displays) if (s.device == ScreenDevice::Monitor && d.key == s.monitorKey) cur = d.label();
    bool missing = s.device == ScreenDevice::Monitor &&
                   std::none_of(app.displays.begin(), app.displays.end(), [&](const DisplayInfo& d) { return d.key == s.monitorKey; });
    if (missing) cur += "  (SEM SINAL)";
    if (ImGui::BeginCombo("Saída", cur.c_str())) {
        if (ImGui::Selectable("Desligado", s.device == ScreenDevice::Disabled)) { app.checkpoint(); s.device = ScreenDevice::Disabled; }
        if (ImGui::Selectable("Janela (teste)", s.device == ScreenDevice::Window)) { app.checkpoint(); s.device = ScreenDevice::Window; }
        ImGui::Separator();
        for (auto& d : app.displays) {
            bool sel = s.device == ScreenDevice::Monitor && s.monitorKey == d.key;
            std::string lbl = d.label() + "##" + d.key;
            if (ImGui::Selectable(lbl.c_str(), sel)) {
                app.checkpoint();
                s.device = ScreenDevice::Monitor;
                s.monitorKey = d.key;
                s.width = d.width(); s.height = d.height();
                if (d.primary) PS_WARN("Atenção: '%s' é o monitor principal — a saída vai tapar a interface.", d.label().c_str());
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.adapter.c_str());
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Placa: %s", toUtf8(gfx().adapterName()).c_str());

    int wh[2] = {s.width, s.height};
    if (ImGui::InputInt2("Resolução", wh)) { s.width = std::clamp(wh[0], 16, 16384); s.height = std::clamp(wh[1], 16, 16384); }
    undoOnActivate(app);
    ImGui::SliderFloat("Brilho##scr", &s.brightness, -1, 1, "%.2f");
    undoOnActivate(app);
    if (ImGui::Checkbox("Test pattern", &s.testPattern)) app.markDirty();
    if (ImGui::Button("Ajustar resolução às slices")) {
        app.checkpoint();
        float mx = 16, my = 16;
        for (auto& sl : s.slices) { mx = std::max(mx, sl.outX + sl.outW * 0.5f); my = std::max(my, sl.outY + sl.outH * 0.5f); }
        s.width = (int)std::ceil(mx); s.height = (int)std::ceil(my);
    }
}

void sliceProps(App& app, Screen& s, Slice& sl) {
    Project& p = app.project;
    ImGui::SeparatorText("Slice");
    inputTextStd("Nome##sl", sl.name);
    undoOnActivate(app);

    int srcIdx = sl.sourceLayer ? p.layerIndex(sl.sourceLayer) : -1;
    std::string src = srcIdx < 0 ? "Composição" : p.comp.layers[srcIdx].name;
    if (ImGui::BeginCombo("Fonte", src.c_str())) {
        if (ImGui::Selectable("Composição", srcIdx < 0)) { app.checkpoint(); sl.sourceLayer = 0; }
        for (int i = 0; i < (int)p.comp.layers.size(); ++i) {
            std::string n = p.comp.layers[i].name + "##" + std::to_string(p.comp.layers[i].id);
            if (ImGui::Selectable(n.c_str(), srcIdx == i)) { app.checkpoint(); sl.sourceLayer = p.comp.layers[i].id; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Composição = todas as layers misturadas.\nLayer = só essa layer (dedicar layers a uma saída).");

    ImGui::TextDisabled("Entrada (recorte da composição)");
    rectEditor(app, "in", sl.input, (float)p.comp.width, (float)p.comp.height);

    ImGui::TextDisabled("Saída (posição no ecrã)");
    Rect o{sl.outX - sl.outW * 0.5f, sl.outY - sl.outH * 0.5f, sl.outW, sl.outH};
    if (rectEditor(app, "out", o, (float)s.width, (float)s.height)) {
        sl.outW = o.w; sl.outH = o.h; sl.outX = o.x + o.w * 0.5f; sl.outY = o.y + o.h * 0.5f;
    }
    ImGui::DragFloat("Rotação", &sl.rotation, 0.25f, -360, 360, "%.2f°");
    undoOnActivate(app);
    ImGui::SameLine();
    if (ImGui::SmallButton("90°")) { app.checkpoint(); sl.rotation = std::fmod(sl.rotation + 90.f, 360.f); }
    if (ImGui::Checkbox("Espelhar H", &sl.flipH)) app.checkpoint();
    ImGui::SameLine();
    if (ImGui::Checkbox("Espelhar V", &sl.flipV)) app.checkpoint();
    ImGui::SliderFloat("Opacidade##sl", &sl.opacity, 0, 1, "%.2f");
    undoOnActivate(app);
    ImGui::SliderFloat("Brilho##sl", &sl.brightness, -1, 1, "%.2f");
    undoOnActivate(app);
    float c[3] = {(sl.color & 0xFF) / 255.f, ((sl.color >> 8) & 0xFF) / 255.f, ((sl.color >> 16) & 0xFF) / 255.f};
    if (ImGui::ColorEdit3("Cor", c, ImGuiColorEditFlags_NoInputs)) {
        sl.color = 0xFF000000u | ((uint32_t)(c[2] * 255) << 16) | ((uint32_t)(c[1] * 255) << 8) | (uint32_t)(c[0] * 255);
    }
    undoOnActivate(app);

    ImGui::SeparatorText("Atalhos");
    if (ImGui::Button("Saída 1:1 (= tamanho da entrada)", ImVec2(-1, 0))) {
        app.checkpoint(); sl.outW = sl.input.w; sl.outH = sl.input.h;
    }
    if (ImGui::Button("Entrada = Saída (mesma posição)", ImVec2(-1, 0))) {
        app.checkpoint(); sl.input = {sl.outX - sl.outW * 0.5f, sl.outY - sl.outH * 0.5f, sl.outW, sl.outH};
    }
    if (ImGui::Button("Criar layer dedicada a esta slice", ImVec2(-1, 0))) {
        // fluxo típico: os clientes entregam o vídeo no formato exato do ledwall
        app.checkpoint();
        Layer& l = p.addLayer(sl.name);
        l.useRegion = true;
        l.region = sl.input;
        app.ui.selLayer = 0;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cria uma layer cuja região é o recorte desta slice.\nOs vídeos dessa layer encaixam automaticamente aqui.");
}

// ---------------------------------------------------------------------------
// canvas
// ---------------------------------------------------------------------------
void canvas(App& app, Screen& sc) {
    Project& p = app.project;
    const int tab = app.ui.editorTab;
    const float cw = tab == 0 ? (float)p.comp.width : (float)sc.width;
    const float ch = tab == 0 ? (float)p.comp.height : (float)sc.height;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImVec2 c1(c0.x + avail.x, c0.y + avail.y);
    ImGui::InvisibleButton("##canvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(c0, c1, true);
    dl->AddRectFilled(c0, c1, IM_COL32(14, 14, 16, 255));

    // zoom / pan
    float fit = std::min((avail.x - 40) / cw, (avail.y - 40) / ch);
    fit = std::max(fit, 0.01f);
    float& zoom = app.ui.zoom[tab];
    float* pan = app.ui.pan[tab];
    if (zoom <= 0) { zoom = fit; pan[0] = pan[1] = 0; }
    if (hovered && io.MouseWheel != 0) {
        ImVec2 m = io.MousePos;
        View before{ImVec2(c0.x + (avail.x - cw * zoom) * 0.5f + pan[0], c0.y + (avail.y - ch * zoom) * 0.5f + pan[1]), zoom};
        ImVec2 cm = before.toCanvas(m);
        float nz = std::clamp(zoom * (io.MouseWheel > 0 ? 1.15f : 1 / 1.15f), 0.02f, 16.f);
        // manter o ponto debaixo do rato fixo
        ImVec2 base(c0.x + (avail.x - cw * nz) * 0.5f, c0.y + (avail.y - ch * nz) * 0.5f);
        pan[0] = m.x - cm.x * nz - base.x;
        pan[1] = m.y - cm.y * nz - base.y;
        zoom = nz;
    }
    View v{ImVec2(c0.x + (avail.x - cw * zoom) * 0.5f + pan[0], c0.y + (avail.y - ch * zoom) * 0.5f + pan[1]), zoom};

    // fundo: imagem real (composição ou saída) para veres o conteúdo
    ImVec2 a0 = v.toScreen(ImVec2(0, 0)), a1 = v.toScreen(ImVec2(cw, ch));
    dl->AddRectFilled(a0, a1, IM_COL32(0, 0, 0, 255));
    if (tab == 0) {
        if (app.comp.compRT().valid()) dl->AddImage(texId(app.comp.compRT().srv.Get()), a0, a1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 150));
    } else if (const RenderTarget* rt = app.comp.screenRT(sc.id)) {
        dl->AddImage(texId(rt->srv.Get()), a0, a1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 170));
    }
    if (app.ui.showGrid && zoom * app.ui.gridSize >= 6) {
        float g = (float)app.ui.gridSize;
        for (float x = g; x < cw; x += g) dl->AddLine(v.toScreen({x, 0}), v.toScreen({x, ch}), IM_COL32(255, 255, 255, 18));
        for (float y = g; y < ch; y += g) dl->AddLine(v.toScreen({0, y}), v.toScreen({cw, y}), IM_COL32(255, 255, 255, 18));
    }
    dl->AddRect(a0, a1, IM_COL32(120, 120, 130, 255));

    // regiões das layers (só na entrada, para orientar)
    if (tab == 0) {
        for (auto& l : p.comp.layers) {
            if (!l.useRegion) continue;
            ImVec2 r0 = v.toScreen({l.region.x, l.region.y}), r1 = v.toScreen({l.region.x + l.region.w, l.region.y + l.region.h});
            dl->AddRect(r0, r1, IM_COL32(255, 255, 255, 60), 0.0f, 0, 1.0f);
            dl->AddText(ImVec2(r0.x + 3, r1.y - 16), IM_COL32(255, 255, 255, 90), l.name.c_str());
        }
    }

    // ----- interação -----
    Slice* selected = nullptr;
    for (auto& s : sc.slices) if (s.id == app.ui.selSlice) selected = &s;
    ImVec2 mouseC = v.toCanvas(io.MousePos);
    const float handlePx = 6.f;

    auto hitHandle = [&](const Slice& s) -> int {
        ERect r = getRect(s, tab);
        if (tab == 1) {
            ImVec2 top = handlePos(r, 1);
            ImVec2 c(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
            ImVec2 dir(top.x - c.x, top.y - c.y);
            float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            ImVec2 rotC = len > 0 ? ImVec2(top.x + dir.x / len * 24 / zoom, top.y + dir.y / len * 24 / zoom) : top;
            ImVec2 rs = v.toScreen(rotC);
            if (std::hypot(io.MousePos.x - rs.x, io.MousePos.y - rs.y) <= handlePx + 2) return 8;
        }
        for (int h = 0; h < 8; ++h) {
            ImVec2 hs = v.toScreen(handlePos(r, h));
            if (std::fabs(io.MousePos.x - hs.x) <= handlePx + 1 && std::fabs(io.MousePos.y - hs.y) <= handlePx + 1) return h;
        }
        return -1;
    };

    if (hovered && g_ed.mode == Drag::None) {
        int h = selected ? hitHandle(*selected) : -1;
        if (h >= 0 && h < 8) {
            ImGui::SetMouseCursor(kHx[h] == 0 ? ImGuiMouseCursor_ResizeNS : kHy[h] == 0 ? ImGuiMouseCursor_ResizeEW
                                  : (kHx[h] == kHy[h]) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
        } else if (h == 8) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        bool startPan = ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsKeyDown(ImGuiKey_Space));
        if (startPan) {
            g_ed.mode = Drag::Pan; g_ed.panStart = io.MousePos;
            g_ed.start = {pan[0], pan[1], 0, 0, 0};
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            uint32_t hit = 0;
            if (h >= 0) hit = selected->id;
            else for (int i = (int)sc.slices.size() - 1; i >= 0; --i)
                if (pointInRect(getRect(sc.slices[i], tab), mouseC)) { hit = sc.slices[i].id; break; }
            app.ui.selSlice = hit;
            for (auto& s : sc.slices) if (s.id == hit) selected = &s;
            if (hit && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                app.checkpoint();
                g_ed.slice = hit;
                g_ed.start = getRect(*selected, tab);
                g_ed.mouseStart = mouseC;
                g_ed.handle = h;
                g_ed.mode = h == 8 ? Drag::Rotate : h >= 0 ? Drag::Resize : Drag::Move;
                if (g_ed.mode == Drag::Rotate) {
                    ImVec2 c(g_ed.start.x + g_ed.start.w * 0.5f, g_ed.start.y + g_ed.start.h * 0.5f);
                    g_ed.startAngle = std::atan2(mouseC.y - c.y, mouseC.x - c.x) * 180.f / 3.14159265f;
                }
                collectSnaps(sc, tab, hit, cw, ch);
            }
        }
    }

    g_ed.guideX = g_ed.guideY = NAN;
    if (g_ed.mode == Drag::Pan) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            pan[0] = g_ed.start.x + io.MousePos.x - g_ed.panStart.x;
            pan[1] = g_ed.start.y + io.MousePos.y - g_ed.panStart.y;
        } else g_ed.mode = Drag::None;
    } else if (g_ed.mode != Drag::None) {
        Slice* s = nullptr;
        for (auto& x : sc.slices) if (x.id == g_ed.slice) s = &x;
        if (!s || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_ed.mode = Drag::None;
        } else {
            ERect r = g_ed.start;
            ImVec2 d(mouseC.x - g_ed.mouseStart.x, mouseC.y - g_ed.mouseStart.y);
            const float thr = 8.f / zoom;
            const bool snap = app.ui.snap && !io.KeyAlt;
            const bool axisAligned = std::fmod(std::fabs(r.rot), 90.f) < 0.01f;
            if (g_ed.mode == Drag::Move) {
                if (io.KeyShift) { if (std::fabs(d.x) > std::fabs(d.y)) d.y = 0; else d.x = 0; } // eixo único
                r.x += d.x; r.y += d.y;
                if (snap && axisAligned) {
                    bool swap = std::fmod(std::fabs(r.rot), 180.f) > 89.f;
                    float w = swap ? r.h : r.w, h = swap ? r.w : r.h;
                    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
                    r.x += snapDelta({cx - w * 0.5f, cx, cx + w * 0.5f}, g_ed.snapX, thr, g_ed.guideX);
                    r.y += snapDelta({cy - h * 0.5f, cy, cy + h * 0.5f}, g_ed.snapY, thr, g_ed.guideY);
                }
                r.x = std::round(r.x); r.y = std::round(r.y);
                if (axisAligned && std::fmod(std::fabs(r.rot), 180.f) > 89.f) { // rodado 90°: alinhar a bbox
                    r.x = std::round(r.x + (r.w - r.h) * 0.5f) - (r.w - r.h) * 0.5f;
                    r.y = std::round(r.y + (r.h - r.w) * 0.5f) - (r.h - r.w) * 0.5f;
                }
            } else if (g_ed.mode == Drag::Resize) {
                int hx = kHx[g_ed.handle], hy = kHy[g_ed.handle];
                // delta no referencial local (desfazer rotação)
                ImVec2 ld = rotateAround(d, ImVec2(0, 0), -r.rot);
                float dw = hx * ld.x, dh = hy * ld.y;
                if (io.KeyShift && hx != 0 && hy != 0) { // manter proporção
                    float aspect = r.w / std::max(1.f, r.h);
                    if (std::fabs(dw) > std::fabs(dh * aspect)) dh = dw / aspect; else dw = dh * aspect;
                }
                bool fromCenter = io.KeyCtrl;
                float nw = std::max(1.f, r.w + dw * (fromCenter ? 2 : 1));
                float nh = std::max(1.f, r.h + dh * (fromCenter ? 2 : 1));
                // snap das bordas que se movem (sem rotação)
                if (snap && std::fabs(r.rot) < 0.01f && !fromCenter) {
                    if (hx > 0) { float e = r.x + nw; float gx; e += snapDelta({e}, g_ed.snapX, thr, gx); g_ed.guideX = gx; nw = e - r.x; }
                    if (hx < 0) { float e = r.x + r.w - nw; float gx; e += snapDelta({e}, g_ed.snapX, thr, gx); g_ed.guideX = gx; nw = r.x + r.w - e; }
                    if (hy > 0) { float e = r.y + nh; float gy; e += snapDelta({e}, g_ed.snapY, thr, gy); g_ed.guideY = gy; nh = e - r.y; }
                    if (hy < 0) { float e = r.y + r.h - nh; float gy; e += snapDelta({e}, g_ed.snapY, thr, gy); g_ed.guideY = gy; nh = r.y + r.h - e; }
                }
                nw = std::max(1.f, std::round(nw)); nh = std::max(1.f, std::round(nh));
                // a borda oposta fica parada → o centro desloca-se metade, rodado
                ImVec2 c(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
                ImVec2 shift = fromCenter ? ImVec2(0, 0) : rotateAround(ImVec2(hx * (nw - r.w) * 0.5f, hy * (nh - r.h) * 0.5f), ImVec2(0, 0), r.rot);
                c.x += shift.x; c.y += shift.y;
                r.w = nw; r.h = nh;
                r.x = c.x - nw * 0.5f; r.y = c.y - nh * 0.5f;
                if (std::fabs(r.rot) < 0.01f) { r.x = std::round(r.x); r.y = std::round(r.y); }
            } else if (g_ed.mode == Drag::Rotate) {
                ImVec2 c(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
                float a = std::atan2(mouseC.y - c.y, mouseC.x - c.x) * 180.f / 3.14159265f;
                float rot = g_ed.start.rot + (a - g_ed.startAngle);
                if (io.KeyShift) rot = std::round(rot / 15.f) * 15.f;
                else if (snap) { float r90 = std::round(rot / 90.f) * 90.f; if (std::fabs(rot - r90) < 3.f) rot = r90; }
                rot = std::fmod(rot + 360.f, 360.f);
                if (rot > 180.f) rot -= 360.f;
                r.rot = rot;
            }
            setRect(*s, tab, r);
        }
    }

    // teclado: setas (Shift = 10 px), Delete, Ctrl+D
    if ((hovered || active || ImGui::IsWindowFocused()) && selected && !ImGui::GetIO().WantTextInput) {
        float step = io.KeyShift ? 10.f : 1.f;
        ImVec2 mv(0, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) mv.x -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) mv.x += step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) mv.y -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) mv.y += step;
        if (mv.x != 0 || mv.y != 0) {
            app.checkpoint();
            ERect r = getRect(*selected, tab);
            r.x += mv.x; r.y += mv.y;
            setRect(*selected, tab, r);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            app.checkpoint();
            uint32_t id = selected->id;
            sc.slices.erase(std::remove_if(sc.slices.begin(), sc.slices.end(), [&](const Slice& x) { return x.id == id; }), sc.slices.end());
            app.ui.selSlice = 0; selected = nullptr;
        } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
            app.checkpoint();
            Slice copy = *selected;
            copy.id = p.newId(); copy.name += " (cópia)";
            copy.input.x += 20; copy.input.y += 20; copy.outX += 20; copy.outY += 20;
            sc.slices.push_back(copy);
            app.ui.selSlice = copy.id; selected = &sc.slices.back();
        }
    }

    // ----- desenho das slices -----
    // slices de outros ecrãs (só na entrada): contorno fraco
    if (tab == 0) {
        for (auto& other : p.screens) {
            if (other.id == sc.id) continue;
            for (auto& s : other.slices) {
                ImVec2 r0 = v.toScreen({s.input.x, s.input.y}), r1 = v.toScreen({s.input.x + s.input.w, s.input.y + s.input.h});
                dl->AddRect(r0, r1, sliceColorU32(s.color, 70), 0.0f, 0, 1.0f);
            }
        }
    }
    char dims[96];
    for (auto& s : sc.slices) {
        ERect r = getRect(s, tab);
        ImVec2 q[4]; corners(r, q);
        for (auto& pt : q) pt = v.toScreen(pt);
        bool sel = s.id == app.ui.selSlice;
        int alpha = !s.enabled ? 20 : sel ? 110 : 70;
        dl->AddQuadFilled(q[0], q[1], q[2], q[3], sliceColorU32(s.color, alpha));
        // diagonais (como no test pattern do Resolume)
        dl->AddLine(q[0], q[2], sliceColorU32(s.color, 120));
        dl->AddLine(q[1], q[3], sliceColorU32(s.color, 120));
        dl->AddQuad(q[0], q[1], q[2], q[3], sel ? IM_COL32(255, 255, 255, 255) : sliceColorU32(s.color, 255), sel ? 2.f : 1.f);
        ImVec2 cen((q[0].x + q[2].x) * 0.5f, (q[0].y + q[2].y) * 0.5f);
        snprintf(dims, sizeof(dims), "%.0f,%.0f // %.0f x %.0f%s", r.x, r.y, r.w, r.h, std::fabs(r.rot) > 0.01f ? "  ⟳" : "");
        bool small = std::fabs(q[2].y - q[0].y) < 40 && std::fabs(q[2].x - q[0].x) < 140;
        label(dl, cen, s.name.c_str(), dims, r.rot, small);
    }
    // pegas da selecionada
    if (selected) {
        ERect r = getRect(*selected, tab);
        for (int h = 0; h < 8; ++h) {
            ImVec2 hs = v.toScreen(handlePos(r, h));
            dl->AddRectFilled(ImVec2(hs.x - handlePx, hs.y - handlePx), ImVec2(hs.x + handlePx, hs.y + handlePx), IM_COL32(255, 255, 255, 255));
            dl->AddRect(ImVec2(hs.x - handlePx, hs.y - handlePx), ImVec2(hs.x + handlePx, hs.y + handlePx), IM_COL32(0, 0, 0, 255));
        }
        if (tab == 1) {
            ImVec2 top = handlePos(r, 1), c(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
            ImVec2 dir(top.x - c.x, top.y - c.y);
            float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (len > 0) {
                ImVec2 rc(top.x + dir.x / len * 24 / zoom, top.y + dir.y / len * 24 / zoom);
                ImVec2 ts = v.toScreen(top), rs = v.toScreen(rc);
                dl->AddLine(ts, rs, IM_COL32(255, 255, 255, 200), 1.5f);
                dl->AddCircleFilled(rs, handlePx, col::Accent);
                dl->AddCircle(rs, handlePx, IM_COL32(0, 0, 0, 255));
            }
        }
    }
    // guias de snap
    if (!std::isnan(g_ed.guideX)) dl->AddLine(v.toScreen({g_ed.guideX, -100000}), v.toScreen({g_ed.guideX, 100000}), col::Accent, 1);
    if (!std::isnan(g_ed.guideY)) dl->AddLine(v.toScreen({-100000, g_ed.guideY}), v.toScreen({100000, g_ed.guideY}), col::Accent, 1);

    // info do rato
    if (hovered) {
        char info[64]; snprintf(info, sizeof(info), "%.0f, %.0f   zoom %.0f%%", mouseC.x, mouseC.y, zoom * 100);
        dl->AddText(ImVec2(c0.x + 8, c1.y - 20), col::TextDim, info);
    }
    dl->PopClipRect();

    // menu de contexto
    if (ImGui::BeginPopupContextItem("canvasctx")) {
        if (ImGui::MenuItem("Adicionar slice aqui")) {
            app.checkpoint();
            Slice& n = p.addSlice(sc, "Slice " + std::to_string(sc.slices.size() + 1));
            ERect r = getRect(n, tab);
            r.x = std::round(mouseC.x - r.w * 0.5f); r.y = std::round(mouseC.y - r.h * 0.5f);
            setRect(n, tab, r);
            app.ui.selSlice = n.id;
            selected = nullptr;
        }
        if (selected) {
            if (ImGui::MenuItem("Duplicar", "Ctrl+D")) {
                app.checkpoint();
                Slice copy = *selected; copy.id = p.newId(); copy.name += " (cópia)";
                copy.input.x += 20; copy.input.y += 20; copy.outX += 20; copy.outY += 20;
                sc.slices.push_back(copy); app.ui.selSlice = copy.id; selected = nullptr;
            }
            if (selected && ImGui::MenuItem("Rodar 90°", nullptr, false, tab == 1)) { app.checkpoint(); selected->rotation = std::fmod(selected->rotation + 90.f, 360.f); }
            if (selected && ImGui::MenuItem("Saída 1:1")) { app.checkpoint(); selected->outW = selected->input.w; selected->outH = selected->input.h; }
            if (selected && ImGui::MenuItem("Ocupar o canvas inteiro")) {
                app.checkpoint(); setRect(*selected, tab, ERect{0, 0, cw, ch, 0});
            }
            if (selected && ImGui::MenuItem("Apagar", "Del")) {
                app.checkpoint();
                uint32_t id = selected->id;
                sc.slices.erase(std::remove_if(sc.slices.begin(), sc.slices.end(), [&](const Slice& x) { return x.id == id; }), sc.slices.end());
                app.ui.selSlice = 0;
            }
        }
        ImGui::EndPopup();
    }
}

} // namespace

bool rectEditor(App& app, const char* id, Rect& r, float maxW, float maxH) {
    ImGui::PushID(id);
    bool changed = false;
    float xy[2] = {r.x, r.y}, wh[2] = {r.w, r.h};
    if (ImGui::DragFloat2("X / Y", xy, 1.f, -maxW * 4, maxW * 4, "%.0f")) { r.x = std::round(xy[0]); r.y = std::round(xy[1]); changed = true; }
    undoOnActivate(app);
    if (ImGui::DragFloat2("L / A", wh, 1.f, 1.f, std::max(maxW, maxH) * 8, "%.0f")) { r.w = std::max(1.f, std::round(wh[0])); r.h = std::max(1.f, std::round(wh[1])); changed = true; }
    undoOnActivate(app);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Largura / Altura em píxeis");
    ImGui::PopID();
    return changed;
}

void drawSliceEditor(App& app) {
    if (app.ui.focusEditor) { ImGui::SetNextWindowFocus(); app.ui.focusEditor = false; }
    if (!ImGui::Begin("Saídas e Slices")) { ImGui::End(); return; }
    Screen* sc = selectedScreen(app);

    // esquerda: ecrãs/slices
    ImGui::BeginChild("list", ImVec2(230, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    screenList(app);
    ImGui::EndChild();
    ImGui::SameLine();

    // centro: barra + canvas
    float propsW = 290;
    ImGui::BeginChild("center", ImVec2(ImGui::GetContentRegionAvail().x - propsW - 8, 0), ImGuiChildFlags_None);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Entrada  (Input Selection)", nullptr, app.ui.editorTab == 0 && ImGui::GetFrameCount() < 3 ? ImGuiTabItemFlags_SetSelected : 0)) { app.ui.editorTab = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Saída  (Output Transformation)", nullptr, app.ui.editorTab == 1 && ImGui::GetFrameCount() < 3 ? ImGuiTabItemFlags_SetSelected : 0)) { app.ui.editorTab = 1; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    int tab = app.ui.editorTab;
    if (ImGui::Button("Ajustar")) app.ui.zoom[tab] = 0;
    ImGui::SameLine();
    if (ImGui::Button("100%")) { app.ui.zoom[tab] = 1; app.ui.pan[tab][0] = app.ui.pan[tab][1] = 0; }
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &app.ui.snap);
    ImGui::SameLine();
    ImGui::Checkbox("Grelha", &app.ui.showGrid);
    if (app.ui.showGrid) { ImGui::SameLine(); ImGui::SetNextItemWidth(70); ImGui::InputInt("##grid", &app.ui.gridSize, 0); app.ui.gridSize = std::clamp(app.ui.gridSize, 2, 512); }
    if (sc) {
        ImGui::SameLine();
        if (ImGui::Checkbox("Test pattern", &sc->testPattern)) app.markDirty();
        ImGui::SameLine();
        ImGui::TextDisabled("  %s: %dx%d", tab == 0 ? "Composição" : sc->name.c_str(),
                            tab == 0 ? app.project.comp.width : sc->width, tab == 0 ? app.project.comp.height : sc->height);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Arrastar: mover   ·   Pegas: redimensionar (Shift = proporção, Ctrl = a partir do centro)\n"
                          "Círculo turquesa: rodar (Shift = 15°)   ·   Alt: desligar snap\n"
                          "Setas: 1 px (Shift = 10 px)   ·   Del: apagar   ·   Ctrl+D: duplicar\n"
                          "Roda do rato: zoom   ·   Botão do meio ou Espaço+arrastar: mover a vista");
    if (sc) canvas(app, *sc);
    else ImGui::TextDisabled("Adiciona um ecrã à esquerda.");
    ImGui::EndChild();
    ImGui::SameLine();

    // direita: propriedades
    ImGui::BeginChild("props", ImVec2(propsW, 0), ImGuiChildFlags_Borders);
    if (sc) {
        ImGui::PushItemWidth(-90);
        screenProps(app, *sc);
        Slice* sl = nullptr;
        for (auto& s : sc->slices) if (s.id == app.ui.selSlice) sl = &s;
        if (sl) sliceProps(app, *sc, *sl);
        else ImGui::TextDisabled("\nSeleciona uma slice para editar.");
        ImGui::PopItemWidth();
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace ps
