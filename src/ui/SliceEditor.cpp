// Saída avançada (Advanced Output) — janela própria, inspirada no Resolume Arena.
//   Esquerda : ecrãs e slices (+ caixa de ajuda)
//   Centro   : separadores Input Selection / Output Transformation, canvas editável
//   Direita  : propriedades do ecrã e da slice
//   Rodapé   : Cancelar / Gravar e fechar
// Modos de edição: Transformar (mover/redimensionar/rodar) e Editar pontos (4 cantos com perspetiva).
#include "ui/UI.h"
#include "core/I18n.h"
#include "app/App.h"
#include "core/Log.h"
#include "gfx/ImageLoader.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ps {

namespace {

const char* g_help = nullptr;   // texto da caixa de ajuda (elemento debaixo do rato)
void hint(const char* text) { if (ImGui::IsItemHovered()) g_help = text; }

// ---------------------------------------------------------------------------
// geometria do editor
// ---------------------------------------------------------------------------
struct ERect { float x, y, w, h, rot; };

ImVec2 rotateAround(ImVec2 p, ImVec2 c, float deg) {
    float a = deg * 3.14159265f / 180.f, cs = std::cos(a), sn = std::sin(a);
    float dx = p.x - c.x, dy = p.y - c.y;
    return ImVec2(c.x + dx * cs - dy * sn, c.y + dx * sn + dy * cs);
}

void rectCorners(const ERect& r, ImVec2 out[4]) {       // TL, TR, BR, BL
    ImVec2 c(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    ImVec2 raw[4] = {{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}};
    for (int i = 0; i < 4; ++i) out[i] = rotateAround(raw[i], c, r.rot);
}

ERect bbox(const ImVec2 p[4]) {
    float x0 = p[0].x, x1 = p[0].x, y0 = p[0].y, y1 = p[0].y;
    for (int i = 1; i < 4; ++i) { x0 = std::min(x0, p[i].x); x1 = std::max(x1, p[i].x); y0 = std::min(y0, p[i].y); y1 = std::max(y1, p[i].y); }
    return {x0, y0, std::max(1.f, x1 - x0), std::max(1.f, y1 - y0), 0};
}

// Forma editável de uma slice num separador: retângulo (com rotação) ou 4 pontos livres
struct Shape {
    bool warp = false;
    ERect r{};          // retângulo (se warp: caixa envolvente)
    ImVec2 p[4];        // cantos TL, TR, BR, BL
};

Shape getShape(const Slice& s, int tab) {
    Shape sh;
    sh.warp = tab == 0 ? s.inWarp : s.outWarp;
    if (sh.warp) {
        const float (*pts)[2] = tab == 0 ? s.inPts : s.outPts;
        for (int i = 0; i < 4; ++i) sh.p[i] = ImVec2(pts[i][0], pts[i][1]);
        sh.r = bbox(sh.p);
    } else {
        if (tab == 0) sh.r = {s.input.x, s.input.y, s.input.w, s.input.h, 0};
        else sh.r = {s.outX - s.outW * 0.5f, s.outY - s.outH * 0.5f, s.outW, s.outH, s.rotation};
        rectCorners(sh.r, sh.p);
    }
    return sh;
}

void setShape(Slice& s, int tab, const Shape& sh) {
    if (sh.warp) {
        float (*pts)[2] = tab == 0 ? s.inPts : s.outPts;
        for (int i = 0; i < 4; ++i) { pts[i][0] = sh.p[i].x; pts[i][1] = sh.p[i].y; }
        (tab == 0 ? s.inWarp : s.outWarp) = true;
        // manter o retângulo coerente (usado nos atalhos 1:1 e se os pontos forem repostos)
        ERect b = bbox(sh.p);
        if (tab == 0) s.input = {b.x, b.y, b.w, b.h};
        else { s.outW = b.w; s.outH = b.h; s.outX = b.x + b.w * 0.5f; s.outY = b.y + b.h * 0.5f; s.rotation = 0; }
        return;
    }
    (tab == 0 ? s.inWarp : s.outWarp) = false;
    const ERect& r = sh.r;
    if (tab == 0) { s.input = {r.x, r.y, r.w, r.h}; return; }
    s.outW = r.w; s.outH = r.h;
    s.outX = r.x + r.w * 0.5f; s.outY = r.y + r.h * 0.5f;
    s.rotation = r.rot;
}

bool pointInQuad(const ImVec2 q[4], ImVec2 p) {
    bool in = false;
    for (int i = 0, j = 3; i < 4; j = i++)
        if (((q[i].y > p.y) != (q[j].y > p.y)) && (p.x < (q[j].x - q[i].x) * (p.y - q[i].y) / (q[j].y - q[i].y) + q[i].x)) in = !in;
    return in;
}

bool quadConvex(const ImVec2 q[4]) {
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        const ImVec2& a = q[i]; const ImVec2& b = q[(i + 1) % 4]; const ImVec2& c = q[(i + 2) % 4];
        double cr = (double)(b.x - a.x) * (c.y - b.y) - (double)(b.y - a.y) * (c.x - b.x);
        int s = cr > 1e-6 ? 1 : cr < -1e-6 ? -1 : 0;
        if (!s) continue;
        if (!sign) sign = s; else if (s != sign) return false;
    }
    return true;
}

// pegas de transformação: 0..7 = TL, T, TR, R, BR, B, BL, L
const int kHx[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
const int kHy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};

ImVec2 handlePos(const ERect& r, int h) {
    ImVec2 c(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    return rotateAround(ImVec2(c.x + kHx[h] * r.w * 0.5f, c.y + kHy[h] * r.h * 0.5f), c, r.rot);
}

enum class Drag { None, Move, Resize, Rotate, Pan, Point };

struct EditorState {
    Drag mode = Drag::None;
    int handle = -1;          // pega (Resize) ou canto (Point)
    uint32_t slice = 0;
    Shape start{};
    ImVec2 mouseStart{};      // canvas
    float startAngle = 0;
    ImVec2 panStart{}, panOrig{};
    std::vector<float> snapX, snapY;
    std::vector<ImVec2> snapPts;
    float guideX = NAN, guideY = NAN;
    int selPoint = -1;        // canto selecionado (modo pontos)
    std::vector<Screen> snapshot;  // para "Cancelar"
    bool wasOpen = false;
} g_ed;

struct View {
    ImVec2 origin;
    float zoom;
    ImVec2 toScreen(ImVec2 c) const { return ImVec2(origin.x + c.x * zoom, origin.y + c.y * zoom); }
    ImVec2 toCanvas(ImVec2 s) const { return ImVec2((s.x - origin.x) / zoom, (s.y - origin.y) / zoom); }
};

void collectSnaps(const Screen& sc, int tab, uint32_t except, float cw, float ch) {
    g_ed.snapX = {0, cw * 0.5f, cw};
    g_ed.snapY = {0, ch * 0.5f, ch};
    g_ed.snapPts = {{0, 0}, {cw, 0}, {cw, ch}, {0, ch}};
    for (auto& s : sc.slices) {
        if (s.id == except || !s.enabled) continue;
        Shape sh = getShape(s, tab);
        for (auto& p : sh.p) g_ed.snapPts.push_back(p);
        ERect b = bbox(sh.p);
        g_ed.snapX.insert(g_ed.snapX.end(), {b.x, b.x + b.w * 0.5f, b.x + b.w});
        g_ed.snapY.insert(g_ed.snapY.end(), {b.y, b.y + b.h * 0.5f, b.y + b.h});
    }
}

float snapDelta(const std::vector<float>& vals, const std::vector<float>& targets, float thr, float& guide) {
    float best = thr, delta = 0; guide = NAN;
    for (float v : vals)
        for (float t : targets) {
            float d = t - v;
            if (std::fabs(d) < best) { best = std::fabs(d); delta = d; guide = t; }
        }
    return delta;
}

// redimensionar retângulo por uma pega (rotação suportada; snap só sem rotação)
ERect resizeRect(const ERect& r0, int handle, ImVec2 d, bool keepAspect, bool fromCenter, bool snap, float thr) {
    ERect r = r0;
    int hx = kHx[handle], hy = kHy[handle];
    ImVec2 ld = rotateAround(d, ImVec2(0, 0), -r.rot);
    float dw = hx * ld.x, dh = hy * ld.y;
    if (keepAspect && hx != 0 && hy != 0) {
        float aspect = r.w / std::max(1.f, r.h);
        if (std::fabs(dw) > std::fabs(dh * aspect)) dh = dw / aspect; else dw = dh * aspect;
    }
    float nw = std::max(1.f, r.w + dw * (fromCenter ? 2 : 1));
    float nh = std::max(1.f, r.h + dh * (fromCenter ? 2 : 1));
    if (snap && std::fabs(r.rot) < 0.01f && !fromCenter) {
        float g;
        if (hx > 0) { float e = r.x + nw; e += snapDelta({e}, g_ed.snapX, thr, g); if (!std::isnan(g)) g_ed.guideX = g; nw = e - r.x; }
        if (hx < 0) { float e = r.x + r.w - nw; e += snapDelta({e}, g_ed.snapX, thr, g); if (!std::isnan(g)) g_ed.guideX = g; nw = r.x + r.w - e; }
        if (hy > 0) { float e = r.y + nh; e += snapDelta({e}, g_ed.snapY, thr, g); if (!std::isnan(g)) g_ed.guideY = g; nh = e - r.y; }
        if (hy < 0) { float e = r.y + r.h - nh; e += snapDelta({e}, g_ed.snapY, thr, g); if (!std::isnan(g)) g_ed.guideY = g; nh = r.y + r.h - e; }
    }
    nw = std::max(1.f, std::round(nw)); nh = std::max(1.f, std::round(nh));
    ImVec2 c(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    ImVec2 shift = fromCenter ? ImVec2(0, 0) : rotateAround(ImVec2(hx * (nw - r.w) * 0.5f, hy * (nh - r.h) * 0.5f), ImVec2(0, 0), r.rot);
    c.x += shift.x; c.y += shift.y;
    r.w = nw; r.h = nh;
    r.x = c.x - nw * 0.5f; r.y = c.y - nh * 0.5f;
    if (std::fabs(r.rot) < 0.01f) { r.x = std::round(r.x); r.y = std::round(r.y); }
    return r;
}

void label(ImDrawList* dl, ImVec2 center, const char* name, const char* dims, bool compact) {
    ImVec2 ts1 = ImGui::CalcTextSize(name), ts2 = ImGui::CalcTextSize(dims);
    float w = std::max(ts1.x, compact ? 0.f : ts2.x) + 10, h = compact ? ts1.y + 6 : ts1.y + ts2.y + 8;
    ImVec2 p0(center.x - w * 0.5f, center.y - h * 0.5f);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(0, 0, 0, 200), 2);
    dl->AddText(ImVec2(center.x - ts1.x * 0.5f, p0.y + 3), IM_COL32(255, 255, 255, 255), name);
    if (!compact) dl->AddText(ImVec2(center.x - ts2.x * 0.5f, p0.y + 5 + ts1.y), IM_COL32(220, 220, 220, 255), dims);
}

Screen* selectedScreen(App& app) {
    Project& p = app.project;
    Screen* s = p.findScreen(app.ui.selScreen);
    if (!s && !p.screens.empty()) { s = &p.screens[0]; app.ui.selScreen = s->id; }
    return s;
}

Slice* selectedSlice(App& app, Screen& sc) {
    for (auto& s : sc.slices) if (s.id == app.ui.selSlice) return &s;
    return nullptr;
}

void deleteSlice(App& app, Screen& sc, uint32_t id) {
    app.checkpoint();
    sc.slices.erase(std::remove_if(sc.slices.begin(), sc.slices.end(), [&](const Slice& x) { return x.id == id; }), sc.slices.end());
    app.ui.selSlice = 0;
    g_ed.selPoint = -1;
}

Slice& duplicateSlice(App& app, Screen& sc, const Slice& src) {
    app.checkpoint();
    Slice copy = src;
    copy.id = app.project.newId();
    copy.name += T(" (cópia)");
    copy.input.x += 20; copy.input.y += 20; copy.outX += 20; copy.outY += 20;
    for (int i = 0; i < 4; ++i) { copy.inPts[i][0] += 20; copy.inPts[i][1] += 20; copy.outPts[i][0] += 20; copy.outPts[i][1] += 20; }
    sc.slices.push_back(copy);
    app.ui.selSlice = copy.id;
    return sc.slices.back();
}

// ---------------------------------------------------------------------------
// esquerda: ecrãs e slices
// ---------------------------------------------------------------------------
void screenList(App& app) {
    Project& p = app.project;
    if (ImGui::Button("+", ImVec2(32, 0))) ImGui::OpenPopup("addmenu");
    hint(T("Adicionar um ecrã (saída) ou uma slice ao ecrã selecionado."));
    if (ImGui::BeginPopup("addmenu")) {
        if (ImGui::MenuItem(T("Adicionar ecrã"))) {
            app.checkpoint();
            Screen& s = p.addScreen(T("Ecrã ") + std::to_string(p.screens.size() + 1));
            app.ui.selScreen = s.id; app.ui.selSlice = 0;
        }
        Screen* cur = selectedScreen(app);
        if (ImGui::MenuItem(T("Adicionar slice"), nullptr, false, cur != nullptr)) {
            app.checkpoint();
            Slice& sl = p.addSlice(*cur, T("Slice ") + std::to_string(cur->slices.size() + 1));
            app.ui.selSlice = sl.id;
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();
    int delScreen = -1;
    uint32_t delSlice = 0; Screen* delOwner = nullptr;
    for (int si = 0; si < (int)p.screens.size(); ++si) {
        Screen& s = p.screens[si];
        ImGui::PushID((int)s.id);
        bool selS = app.ui.selScreen == s.id;
        ImGuiTreeNodeFlags tf = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (selS && app.ui.selSlice == 0) tf |= ImGuiTreeNodeFlags_Selected;
        bool open = ImGui::TreeNodeEx("##scr", tf, "%s%s", s.name.c_str(), app.isLive(s.id) ? T("  ● NO AR") : "");
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) { app.ui.selScreen = s.id; app.ui.selSlice = 0; }
        hint(T("Ecrã = uma saída da placa gráfica. Clica para ver as propriedades à direita."));
        if (ImGui::BeginPopupContextItem("scrctx")) {
            if (ImGui::MenuItem(T("Adicionar slice"))) {
                app.checkpoint();
                Slice& sl = p.addSlice(s, T("Slice ") + std::to_string(s.slices.size() + 1));
                app.ui.selScreen = s.id; app.ui.selSlice = sl.id;
            }
            if (ImGui::MenuItem(T("Duplicar ecrã"))) {
                app.checkpoint();
                Screen copy = s; copy.id = p.newId(); copy.name += T(" (cópia)"); copy.device = ScreenDevice::Disabled;
                for (auto& sl : copy.slices) sl.id = p.newId();
                p.screens.insert(p.screens.begin() + si + 1, copy);
            }
            if (ImGui::MenuItem(T("Apagar ecrã"))) delScreen = si;
            ImGui::EndPopup();
        }
        // subtítulo com a saída, como no Resolume
        std::string dev = s.device == ScreenDevice::Disabled ? std::string(T("Desligado"))
                        : s.device == ScreenDevice::Window ? std::string(T("Janela (teste)"))
                        : s.monitorKey;
        for (auto& d : app.displays) if (s.device == ScreenDevice::Monitor && d.key == s.monitorKey) dev = d.label();
        ImGui::Indent(18);
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.75f, 1), "%s", dev.c_str());
        ImGui::Unindent(18);
        if (open) {
            for (int k = 0; k < (int)s.slices.size(); ++k) {
                Slice& sl = s.slices[k];
                ImGui::PushID((int)sl.id);
                bool en = sl.enabled;
                if (ImGui::Checkbox("##en", &en)) { app.checkpoint(); sl.enabled = en; }
                hint(T("Ligar/desligar esta slice."));
                ImGui::SameLine();
                ImVec2 cp = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cp.x, cp.y + 4), ImVec2(cp.x + 10, cp.y + 14), sliceColorU32(sl.color, 255), 2);
                ImGui::SetCursorScreenPos(ImVec2(cp.x + 14, cp.y));
                std::string nm = sl.name + ((sl.inWarp || sl.outWarp) ? "  ◇" : "");
                if (ImGui::Selectable(nm.c_str(), app.ui.selSlice == sl.id)) { app.ui.selScreen = s.id; app.ui.selSlice = sl.id; g_ed.selPoint = -1; }
                hint(T("Arrasta para mudar a ordem (as de baixo ficam por cima). Botão direito: mais opções."));
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
                if (ImGui::BeginPopupContextItem("slctx")) {
                    app.ui.selScreen = s.id; app.ui.selSlice = sl.id;
                    if (ImGui::MenuItem(T("Duplicar"), "Ctrl+D")) duplicateSlice(app, s, s.slices[k]);
                    else if (ImGui::MenuItem(T("Apagar"), "Del")) { delSlice = sl.id; delOwner = &s; }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (delSlice && delOwner) deleteSlice(app, *delOwner, delSlice);
    if (delScreen >= 0) {
        app.checkpoint();
        app.setOutput(p.screens[delScreen].id, false);
        p.screens.erase(p.screens.begin() + delScreen);
        app.ui.selScreen = p.screens.empty() ? 0 : p.screens[0].id;
        app.ui.selSlice = 0;
    }
}

// ---------------------------------------------------------------------------
// direita: propriedades
// ---------------------------------------------------------------------------
bool slider(App& app, const char* label, float* v, float lo, float hi, const char* fmt, const char* help) {
    bool ch = ImGui::SliderFloat(label, v, lo, hi, fmt);
    undoOnActivate(app);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { app.checkpoint(); *v = (lo < 0 ? 0.f : hi); ch = true; }
    hint(help);
    return ch;
}

std::string pickImage(HWND owner) {
    wchar_t file[MAX_PATH] = L"";
    std::wstring filter = fromUtf8(T("Imagem"));
    filter.push_back(0); filter += L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff"; filter.push_back(0); filter.push_back(0);
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = owner;
    of.lpstrFilter = filter.c_str();
    of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&of) ? toUtf8(file) : std::string();
}

void screenProps(App& app, Screen& s) {
    ImGui::SeparatorText(s.name.c_str());
    inputTextStd(T("Nome##scr"), s.name);
    undoOnActivate(app);

    std::string cur = s.device == ScreenDevice::Disabled ? T("Desligado") : s.device == ScreenDevice::Window ? T("Janela (teste)") : s.monitorKey;
    for (auto& d : app.displays) if (s.device == ScreenDevice::Monitor && d.key == s.monitorKey) cur = d.label();
    bool missing = s.device == ScreenDevice::Monitor &&
                   std::none_of(app.displays.begin(), app.displays.end(), [&](const DisplayInfo& d) { return d.key == s.monitorKey; });
    if (missing) cur += T("  (SEM SINAL)");
    if (ImGui::BeginCombo(T("Dispositivo"), cur.c_str())) {
        if (ImGui::Selectable(T("Desligado"), s.device == ScreenDevice::Disabled)) { app.checkpoint(); s.device = ScreenDevice::Disabled; }
        if (ImGui::Selectable(T("Janela (teste)"), s.device == ScreenDevice::Window)) { app.checkpoint(); s.device = ScreenDevice::Window; }
        ImGui::Separator();
        for (auto& d : app.displays) {
            bool sel = s.device == ScreenDevice::Monitor && s.monitorKey == d.key;
            std::string lbl = d.label() + "##" + d.key;
            if (ImGui::Selectable(lbl.c_str(), sel)) {
                app.checkpoint();
                s.device = ScreenDevice::Monitor;
                s.monitorKey = d.key;
                s.width = d.width(); s.height = d.height();
                if (d.primary) PS_WARN(T("Atenção: '%s' é o monitor principal — a saída vai tapar a interface."), d.label().c_str());
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.adapter.c_str());
        }
        ImGui::EndCombo();
    }
    hint(T("Saída da placa gráfica onde este ecrã aparece."));

    int wh[2] = {s.width, s.height};
    if (ImGui::InputInt2(T("Resolução"), wh)) { s.width = std::clamp(wh[0], 16, 16384); s.height = std::clamp(wh[1], 16, 16384); }
    undoOnActivate(app);
    hint(T("Tamanho do ecrã em píxeis (normalmente igual à resolução da saída)."));

    slider(app, T("Opacidade##scr"), &s.opacity, 0, 1, "%.2f", T("Opacidade de todo o ecrã. Duplo clique repõe."));
    slider(app, T("Brilho##scr"), &s.brightness, -1, 1, "%.2f", T("Brilho de todo o ecrã. Duplo clique repõe."));
    slider(app, T("Contraste"), &s.contrast, -1, 1, "%.2f", T("Contraste de todo o ecrã. Duplo clique repõe."));
    slider(app, T("Vermelho"), &s.red, -1, 1, "%.2f", T("Correção de cor: vermelho. Duplo clique repõe."));
    slider(app, T("Verde"), &s.green, -1, 1, "%.2f", T("Correção de cor: verde. Duplo clique repõe."));
    slider(app, T("Azul"), &s.blue, -1, 1, "%.2f", T("Correção de cor: azul. Duplo clique repõe."));

    if (s.device != ScreenDevice::Disabled) {
        bool on = app.isLive(s.id);
        if (ImGui::Checkbox(T("Saída ligada"), &on)) app.setOutput(s.id, on);
        hint(T("Liga/desliga esta saída agora."));
        ImGui::SameLine();
    }
    if (ImGui::Checkbox(T("No atalho"), &s.inToggle)) app.markDirty();
    hint(T("Este ecrã liga/desliga com o atalho das saídas."));

    ImGui::SeparatorText(T("Output Guide"));
    if (ImGui::Button(T("Carregar imagem..."))) {
        std::string f = pickImage(app.hwnd());
        if (!f.empty()) { app.checkpoint(); s.guidePath = f; }
    }
    hint(T("Fotografia ou desenho do palco por baixo das slices, para as alinhar com as paredes."));
    if (!s.guidePath.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(T("Remover"))) { app.checkpoint(); s.guidePath.clear(); }
        slider(app, T("Opacidade##guide"), &s.guideOpacity, 0, 1, "%.2f", T("Opacidade da imagem guia (só no editor)."));
        const char* fn = s.guidePath.c_str();
        if (const char* sl = strrchr(fn, '\\')) fn = sl + 1;
        ImGui::TextDisabled("%s", fn);
    }
    ImGui::Spacing();
    if (ImGui::Button(T("Ajustar resolução às slices"))) {
        app.checkpoint();
        float mx = 16, my = 16;
        for (auto& sl : s.slices) { float q[4][2]; sl.outputQuad(q); for (auto& pt : q) { mx = std::max(mx, pt[0]); my = std::max(my, pt[1]); } }
        s.width = (int)std::ceil(mx); s.height = (int)std::ceil(my);
    }
}

bool pointsEditor(App& app, const char* id, float pts[4][2]) {
    const char* names[4] = {T("Topo esq."), T("Topo dir."), T("Baixo dir."), T("Baixo esq.")};
    bool ch = false;
    ImGui::PushID(id);
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        ch |= ImGui::DragFloat2(names[i], pts[i], 0.5f, -65536, 65536, "%.1f");
        undoOnActivate(app);
        ImGui::PopID();
    }
    ImGui::PopID();
    return ch;
}

void sliceProps(App& app, Screen& s, Slice& sl) {
    Project& p = app.project;
    ImGui::SeparatorText(sl.name.c_str());
    inputTextStd(T("Nome##sl"), sl.name);
    undoOnActivate(app);

    int srcIdx = sl.sourceLayer ? p.layerIndex(sl.sourceLayer) : -1;
    std::string src = srcIdx < 0 ? T("Composição") : p.comp.layers[srcIdx].name;
    if (ImGui::BeginCombo(T("Fonte"), src.c_str())) {
        if (ImGui::Selectable(T("Composição"), srcIdx < 0)) { app.checkpoint(); sl.sourceLayer = 0; }
        for (int i = 0; i < (int)p.comp.layers.size(); ++i) {
            std::string n = p.comp.layers[i].name + "##" + std::to_string(p.comp.layers[i].id);
            if (ImGui::Selectable(n.c_str(), srcIdx == i)) { app.checkpoint(); sl.sourceLayer = p.comp.layers[i].id; }
        }
        ImGui::EndCombo();
    }
    hint(T("Composição = todas as layers misturadas. Layer = só essa layer (dedicar layers a uma saída)."));

    ImGui::TextDisabled("%s", T("Entrada (recorte da composição)"));
    if (sl.inWarp) {
        pointsEditor(app, "inpts", sl.inPts);
        if (ImGui::SmallButton(T("Repor retângulo##in"))) { app.checkpoint(); sl.inWarp = false; }
        hint(T("Volta a um retângulo (a caixa envolvente dos pontos)."));
    } else {
        rectEditor(app, "in", sl.input, (float)p.comp.width, (float)p.comp.height);
    }

    ImGui::TextDisabled("%s", T("Saída (posição no ecrã)"));
    if (sl.outWarp) {
        pointsEditor(app, "outpts", sl.outPts);
        if (ImGui::SmallButton(T("Repor retângulo##out"))) { app.checkpoint(); sl.outWarp = false; }
        hint(T("Volta a um retângulo (a caixa envolvente dos pontos)."));
    } else {
        Rect o{sl.outX - sl.outW * 0.5f, sl.outY - sl.outH * 0.5f, sl.outW, sl.outH};
        if (rectEditor(app, "out", o, (float)s.width, (float)s.height)) {
            sl.outW = o.w; sl.outH = o.h; sl.outX = o.x + o.w * 0.5f; sl.outY = o.y + o.h * 0.5f;
        }
        ImGui::DragFloat(T("Rotação"), &sl.rotation, 0.25f, -360, 360, "%.2f°");
        undoOnActivate(app);
        hint(T("Rotação da slice na saída, em graus."));
    }
    if (ImGui::Checkbox(T("Espelhar H"), &sl.flipH)) app.checkpoint();
    ImGui::SameLine();
    if (ImGui::Checkbox(T("Espelhar V"), &sl.flipV)) app.checkpoint();
    slider(app, T("Opacidade##sl"), &sl.opacity, 0, 1, "%.2f", T("Opacidade desta slice. Duplo clique repõe."));
    slider(app, T("Brilho##sl"), &sl.brightness, -1, 1, "%.2f", T("Brilho desta slice. Duplo clique repõe."));
    float c[3] = {(sl.color & 0xFF) / 255.f, ((sl.color >> 8) & 0xFF) / 255.f, ((sl.color >> 16) & 0xFF) / 255.f};
    if (ImGui::ColorEdit3(T("Cor"), c, ImGuiColorEditFlags_NoInputs))
        sl.color = 0xFF000000u | ((uint32_t)(c[2] * 255) << 16) | ((uint32_t)(c[1] * 255) << 8) | (uint32_t)(c[0] * 255);
    undoOnActivate(app);
    hint(T("Cor da slice no editor e no test pattern."));

    ImGui::SeparatorText(T("Atalhos"));
    if (ImGui::Button(T("Saída 1:1 (= tamanho da entrada)"), ImVec2(-1, 0))) {
        app.checkpoint(); sl.outWarp = false; sl.outW = sl.input.w; sl.outH = sl.input.h;
    }
    hint(T("A saída fica com o mesmo tamanho em píxeis que o recorte (sem escalar)."));
    if (ImGui::Button(T("Entrada = Saída (mesma posição)"), ImVec2(-1, 0))) {
        app.checkpoint();
        if (sl.outWarp) { sl.inWarp = true; memcpy(sl.inPts, sl.outPts, sizeof(sl.inPts)); }
        else { sl.inWarp = false; sl.input = {sl.outX - sl.outW * 0.5f, sl.outY - sl.outH * 0.5f, sl.outW, sl.outH}; }
    }
    hint(T("O recorte da composição passa a ser igual à posição na saída."));
    if (ImGui::Button(T("Criar layer dedicada a esta slice"), ImVec2(-1, 0))) {
        app.checkpoint();
        Layer& l = p.addLayer(sl.name);
        l.useRegion = true;
        l.region = sl.input;
        app.ui.selLayer = 0;
    }
    hint(T("Cria uma layer cuja região é o recorte desta slice. Os vídeos dessa layer encaixam automaticamente aqui."));
}

// ---------------------------------------------------------------------------
// canvas
// ---------------------------------------------------------------------------
void canvas(App& app, Screen& sc, ImVec2 size) {
    Project& p = app.project;
    const int tab = app.ui.editorTab;
    const float cw = tab == 0 ? (float)p.comp.width : (float)sc.width;
    const float ch = tab == 0 ? (float)p.comp.height : (float)sc.height;
    const bool pointsMode = app.ui.editPoints;
    if (size.x < 50 || size.y < 50) return;

    ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImVec2 c1(c0.x + size.x, c0.y + size.y);
    ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && !g_help) g_help = pointsMode ? T("Editar pontos: arrasta os cantos da slice selecionada para a deformar (perspetiva). Arrastar o interior move a slice inteira.")
                                                : T("Transformar: arrasta para mover, pegas para redimensionar (Shift = proporção, Ctrl = centro), círculo para rodar. Setas: 1 px (Shift 10 px).");
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(c0, c1, true);
    dl->AddRectFilled(c0, c1, IM_COL32(52, 54, 58, 255));

    // zoom / pan
    float fit = std::max(0.01f, std::min((size.x - 40) / cw, (size.y - 40) / ch));
    float& zoom = app.ui.zoom[tab];
    float* pan = app.ui.pan[tab];
    if (zoom <= 0) { zoom = fit; pan[0] = pan[1] = 0; }
    if (hovered && io.MouseWheel != 0) {
        ImVec2 m = io.MousePos;
        View before{ImVec2(c0.x + (size.x - cw * zoom) * 0.5f + pan[0], c0.y + (size.y - ch * zoom) * 0.5f + pan[1]), zoom};
        ImVec2 cm = before.toCanvas(m);
        float nz = std::clamp(zoom * (io.MouseWheel > 0 ? 1.15f : 1 / 1.15f), 0.02f, 16.f);
        ImVec2 base(c0.x + (size.x - cw * nz) * 0.5f, c0.y + (size.y - ch * nz) * 0.5f);
        pan[0] = m.x - cm.x * nz - base.x;
        pan[1] = m.y - cm.y * nz - base.y;
        zoom = nz;
    }
    View v{ImVec2(c0.x + (size.x - cw * zoom) * 0.5f + pan[0], c0.y + (size.y - ch * zoom) * 0.5f + pan[1]), zoom};

    // fundo: preto + vídeo real (programa / preview) + output guide
    ImVec2 a0 = v.toScreen(ImVec2(0, 0)), a1 = v.toScreen(ImVec2(cw, ch));
    dl->AddRectFilled(a0, a1, IM_COL32(0, 0, 0, 255));
    const int bgAlpha = (int)(std::clamp(app.ui.editorBgAlpha, 0.f, 1.f) * 255);
    if (app.ui.editorBg != 2) {
        const bool prev = app.ui.editorBg == 1;
        if (tab == 0) {
            // Input Selection: se a slice selecionada usa uma layer como fonte, mostrar só essa layer
            ID3D11ShaderResourceView* srv = nullptr;
            const Slice* bgSl = selectedSlice(app, sc);
            if (!prev && bgSl && bgSl->sourceLayer && p.layerIndex(bgSl->sourceLayer) >= 0) srv = app.comp.programSource(bgSl->sourceLayer);
            else { const RenderTarget& rt = prev ? app.comp.previewRT() : app.comp.compRT(); if (rt.valid()) srv = rt.srv.Get(); }
            if (srv) dl->AddImage(texId(srv), a0, a1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, bgAlpha));
            if (!prev && bgSl && bgSl->sourceLayer) {
                int li = p.layerIndex(bgSl->sourceLayer);
                if (li >= 0) {
                    std::string t = std::string(T("Fonte: ")) + p.comp.layers[li].name;
                    dl->AddText(ImVec2(a0.x + 6, a0.y + 6), IM_COL32(255, 255, 255, 200), t.c_str());
                }
            }
        } else {
            const RenderTarget* rt = nullptr;
            if (prev) { app.comp.requestPreviewScreen(sc.id); rt = app.comp.previewScreenRT(sc.id); }
            else rt = app.comp.screenRT(sc.id);
            if (rt) dl->AddImage(texId(rt->srv.Get()), a0, a1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, bgAlpha));
        }
    }
    if (tab == 1 && !sc.guidePath.empty())
        if (const ImageTexture* g = loadImageCached(sc.guidePath))
            dl->AddImage(texId(g->srv.Get()), a0, a1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(sc.guideOpacity * 255)));
    if (app.ui.showGrid && zoom * app.ui.gridSize >= 6) {
        float g = (float)app.ui.gridSize;
        for (float x = g; x < cw; x += g) dl->AddLine(v.toScreen({x, 0}), v.toScreen({x, ch}), IM_COL32(255, 255, 255, 18));
        for (float y = g; y < ch; y += g) dl->AddLine(v.toScreen({0, y}), v.toScreen({cw, y}), IM_COL32(255, 255, 255, 18));
    }
    dl->AddRect(a0, a1, IM_COL32(120, 120, 130, 255));

    if (tab == 0)
        for (auto& l : p.comp.layers) {
            if (!l.useRegion) continue;
            ImVec2 r0 = v.toScreen({l.region.x, l.region.y}), r1 = v.toScreen({l.region.x + l.region.w, l.region.y + l.region.h});
            dl->AddRect(r0, r1, IM_COL32(255, 255, 255, 60), 0.0f, 0, 1.0f);
            dl->AddText(ImVec2(r0.x + 3, r1.y - 16), IM_COL32(255, 255, 255, 90), l.name.c_str());
        }

    // ----- interação -----
    Slice* selected = selectedSlice(app, sc);
    ImVec2 mouseC = v.toCanvas(io.MousePos);
    const float hp = 6.f;   // tamanho das pegas (px)
    const bool panTool = app.ui.handTool || ImGui::IsKeyDown(ImGuiKey_Space);

    // posição do círculo de rotação (modo transformar, só na saída)
    auto rotHandle = [&](const Shape& sh, ImVec2& out) -> bool {
        if (tab != 1 || pointsMode) return false;
        ImVec2 top = handlePos(sh.r, 1), c(sh.r.x + sh.r.w * 0.5f, sh.r.y + sh.r.h * 0.5f);
        ImVec2 dir(top.x - c.x, top.y - c.y);
        float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (len <= 0) return false;
        out = ImVec2(top.x + dir.x / len * 24 / zoom, top.y + dir.y / len * 24 / zoom);
        return true;
    };
    // devolve: 0..7 pega, 8 rotação, 10..13 canto (modo pontos), -1 nada
    auto hitHandle = [&](const Slice& s) -> int {
        Shape sh = getShape(s, tab);
        if (pointsMode) {
            for (int i = 0; i < 4; ++i) {
                ImVec2 pp = v.toScreen(sh.p[i]);
                if (std::hypot(io.MousePos.x - pp.x, io.MousePos.y - pp.y) <= hp + 3) return 10 + i;
            }
            return -1;
        }
        ImVec2 rc;
        if (rotHandle(sh, rc)) {
            ImVec2 rs = v.toScreen(rc);
            if (std::hypot(io.MousePos.x - rs.x, io.MousePos.y - rs.y) <= hp + 2) return 8;
        }
        for (int h = 0; h < 8; ++h) {
            ImVec2 hs = v.toScreen(handlePos(sh.r, h));
            if (std::fabs(io.MousePos.x - hs.x) <= hp + 1 && std::fabs(io.MousePos.y - hs.y) <= hp + 1) return h;
        }
        return -1;
    };

    if (hovered && g_ed.mode == Drag::None) {
        int h = selected ? hitHandle(*selected) : -1;
        if (panTool) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        else if (h >= 0 && h < 8)
            ImGui::SetMouseCursor(kHx[h] == 0 ? ImGuiMouseCursor_ResizeNS : kHy[h] == 0 ? ImGuiMouseCursor_ResizeEW
                                  : (kHx[h] == kHy[h]) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
        else if (h >= 8) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        bool startPan = ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && panTool);
        if (startPan) {
            g_ed.mode = Drag::Pan; g_ed.panStart = io.MousePos; g_ed.panOrig = ImVec2(pan[0], pan[1]);
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            uint32_t hit = 0;
            if (h >= 0) hit = selected->id;
            else for (int i = (int)sc.slices.size() - 1; i >= 0; --i) {
                Shape sh = getShape(sc.slices[i], tab);
                if (pointInQuad(sh.p, mouseC)) { hit = sc.slices[i].id; break; }
            }
            if (hit != app.ui.selSlice) g_ed.selPoint = -1;
            app.ui.selSlice = hit;
            selected = selectedSlice(app, sc);
            if (hit && selected && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                app.checkpoint();
                g_ed.slice = hit;
                g_ed.start = getShape(*selected, tab);
                g_ed.mouseStart = mouseC;
                g_ed.handle = h;
                if (h >= 10) { g_ed.mode = Drag::Point; g_ed.handle = h - 10; g_ed.selPoint = h - 10; }
                else if (h == 8) {
                    g_ed.mode = Drag::Rotate;
                    ImVec2 c(g_ed.start.r.x + g_ed.start.r.w * 0.5f, g_ed.start.r.y + g_ed.start.r.h * 0.5f);
                    g_ed.startAngle = std::atan2(mouseC.y - c.y, mouseC.x - c.x) * 180.f / 3.14159265f;
                } else if (h >= 0) g_ed.mode = Drag::Resize;
                else g_ed.mode = Drag::Move;
                collectSnaps(sc, tab, hit, cw, ch);
            }
        }
    }

    g_ed.guideX = g_ed.guideY = NAN;
    if (g_ed.mode == Drag::Pan) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            pan[0] = g_ed.panOrig.x + io.MousePos.x - g_ed.panStart.x;
            pan[1] = g_ed.panOrig.y + io.MousePos.y - g_ed.panStart.y;
        } else g_ed.mode = Drag::None;
    } else if (g_ed.mode != Drag::None) {
        Slice* s = nullptr;
        for (auto& x : sc.slices) if (x.id == g_ed.slice) s = &x;
        if (!s || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_ed.mode = Drag::None;
        } else {
            Shape sh = g_ed.start;
            ImVec2 d(mouseC.x - g_ed.mouseStart.x, mouseC.y - g_ed.mouseStart.y);
            const float thr = 8.f / zoom;
            const bool snap = app.ui.snap && !io.KeyAlt;
            if (g_ed.mode == Drag::Move) {
                if (io.KeyShift) { if (std::fabs(d.x) > std::fabs(d.y)) d.y = 0; else d.x = 0; }
                if (snap && (sh.warp || std::fmod(std::fabs(sh.r.rot), 90.f) < 0.01f)) {
                    ImVec2 moved[4];
                    for (int i = 0; i < 4; ++i) moved[i] = ImVec2(sh.p[i].x + d.x, sh.p[i].y + d.y);
                    ERect b = bbox(moved);
                    d.x += snapDelta({b.x, b.x + b.w * 0.5f, b.x + b.w}, g_ed.snapX, thr, g_ed.guideX);
                    d.y += snapDelta({b.y, b.y + b.h * 0.5f, b.y + b.h}, g_ed.snapY, thr, g_ed.guideY);
                }
                d.x = std::round(d.x); d.y = std::round(d.y);
                for (auto& pt : sh.p) { pt.x += d.x; pt.y += d.y; }
                sh.r.x += d.x; sh.r.y += d.y;
            } else if (g_ed.mode == Drag::Resize) {
                ERect nr = resizeRect(sh.r, g_ed.handle, d, io.KeyShift, io.KeyCtrl, snap, thr);
                if (sh.warp) {   // escalar os pontos da caixa antiga para a nova
                    for (int i = 0; i < 4; ++i) {
                        float u = (g_ed.start.p[i].x - sh.r.x) / sh.r.w, t = (g_ed.start.p[i].y - sh.r.y) / sh.r.h;
                        sh.p[i] = ImVec2(nr.x + u * nr.w, nr.y + t * nr.h);
                    }
                }
                sh.r = nr;
            } else if (g_ed.mode == Drag::Rotate) {
                ImVec2 c(sh.r.x + sh.r.w * 0.5f, sh.r.y + sh.r.h * 0.5f);
                float a = std::atan2(mouseC.y - c.y, mouseC.x - c.x) * 180.f / 3.14159265f;
                float rot = sh.r.rot + (a - g_ed.startAngle);
                if (io.KeyShift) rot = std::round(rot / 15.f) * 15.f;
                else if (snap) { float r90 = std::round(rot / 90.f) * 90.f; if (std::fabs(rot - r90) < 3.f) rot = r90; }
                float delta = rot - sh.r.rot;
                if (sh.warp) { for (int i = 0; i < 4; ++i) sh.p[i] = rotateAround(g_ed.start.p[i], c, delta); }
                else { rot = std::fmod(rot + 360.f, 360.f); if (rot > 180.f) rot -= 360.f; sh.r.rot = rot; }
            } else if (g_ed.mode == Drag::Point) {
                // arrastar um canto: a slice passa a ter perspetiva (4 pontos livres)
                int i = g_ed.handle;
                ImVec2 np(g_ed.start.p[i].x + d.x, g_ed.start.p[i].y + d.y);
                if (snap) {
                    float best = thr; ImVec2 bp = np; bool pt = false;
                    for (auto& t : g_ed.snapPts) { float dd = std::hypot(t.x - np.x, t.y - np.y); if (dd < best) { best = dd; bp = t; pt = true; } }
                    if (pt) { np = bp; g_ed.guideX = bp.x; g_ed.guideY = bp.y; }
                    else {
                        np.x += snapDelta({np.x}, g_ed.snapX, thr, g_ed.guideX);
                        np.y += snapDelta({np.y}, g_ed.snapY, thr, g_ed.guideY);
                    }
                }
                np.x = std::round(np.x * 2) / 2; np.y = std::round(np.y * 2) / 2;   // meio píxel
                sh.warp = true;
                sh.p[i] = np;
            }
            setShape(*s, tab, sh);
        }
    }

    // teclado: setas (modo pontos: o canto selecionado), Delete, Ctrl+D
    if ((hovered || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) && selected && !io.WantTextInput) {
        float step = io.KeyShift ? 10.f : (pointsMode ? 0.5f : 1.f);
        ImVec2 mv(0, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) mv.x -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) mv.x += step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) mv.y -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) mv.y += step;
        if (mv.x != 0 || mv.y != 0) {
            app.checkpoint();
            Shape sh = getShape(*selected, tab);
            if (pointsMode && g_ed.selPoint >= 0) { sh.warp = true; sh.p[g_ed.selPoint].x += mv.x; sh.p[g_ed.selPoint].y += mv.y; }
            else { for (auto& pt : sh.p) { pt.x += mv.x; pt.y += mv.y; } sh.r.x += mv.x; sh.r.y += mv.y; }
            setShape(*selected, tab, sh);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) { deleteSlice(app, sc, selected->id); selected = nullptr; }
        else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) selected = &duplicateSlice(app, sc, *selected);
    }

    // ----- desenho -----
    if (tab == 0)
        for (auto& other : p.screens) {
            if (other.id == sc.id) continue;
            for (auto& s : other.slices) {
                Shape sh = getShape(s, 0);
                ImVec2 q[4]; for (int i = 0; i < 4; ++i) q[i] = v.toScreen(sh.p[i]);
                dl->AddQuad(q[0], q[1], q[2], q[3], sliceColorU32(s.color, 70), 1.f);
            }
        }
    char dims[96];
    for (auto& s : sc.slices) {
        Shape sh = getShape(s, tab);
        ImVec2 q[4]; for (int i = 0; i < 4; ++i) q[i] = v.toScreen(sh.p[i]);
        bool sel = s.id == app.ui.selSlice;
        bool convex = quadConvex(sh.p);
        int alpha = !s.enabled ? 20 : sel ? 90 : 55;
        if (convex) dl->AddQuadFilled(q[0], q[1], q[2], q[3], sliceColorU32(s.color, alpha));
        dl->AddLine(q[0], q[2], sliceColorU32(s.color, 120));
        dl->AddLine(q[1], q[3], sliceColorU32(s.color, 120));
        ImU32 edge = !convex ? IM_COL32(255, 60, 60, 255) : sel ? IM_COL32(255, 255, 255, 255) : sliceColorU32(s.color, 255);
        dl->AddQuad(q[0], q[1], q[2], q[3], edge, sel ? 2.f : 1.f);
        ImVec2 cen((q[0].x + q[1].x + q[2].x + q[3].x) * 0.25f, (q[0].y + q[1].y + q[2].y + q[3].y) * 0.25f);
        if (sh.warp) snprintf(dims, sizeof(dims), "%s", T("pontos (perspetiva)"));
        else snprintf(dims, sizeof(dims), "%.0f,%.0f // %.0f x %.0f%s", sh.r.x, sh.r.y, sh.r.w, sh.r.h, std::fabs(sh.r.rot) > 0.01f ? "  ⟳" : "");
        ERect sb = bbox(q);
        bool compact = sb.h < 40 && sb.w < 140;
        label(dl, cen, s.name.c_str(), dims, compact);
        if (!convex) dl->AddText(ImVec2(cen.x - 40, cen.y + 18), IM_COL32(255, 80, 80, 255), T("forma inválida"));
    }
    if (selected) {
        Shape sh = getShape(*selected, tab);
        if (pointsMode) {
            for (int i = 0; i < 4; ++i) {
                ImVec2 c = v.toScreen(sh.p[i]);
                bool sp = g_ed.selPoint == i;
                dl->AddCircleFilled(c, hp + (sp ? 2 : 0), sp ? col::Accent : IM_COL32(255, 255, 255, 255));
                dl->AddCircle(c, hp + (sp ? 2 : 0), IM_COL32(0, 0, 0, 255), 0, 1.5f);
            }
        } else {
            if (sh.warp) {   // caixa envolvente
                ImVec2 b0 = v.toScreen({sh.r.x, sh.r.y}), b1 = v.toScreen({sh.r.x + sh.r.w, sh.r.y + sh.r.h});
                dl->AddRect(b0, b1, IM_COL32(255, 255, 255, 110), 0.0f, 0, 1.0f);
            }
            for (int h = 0; h < 8; ++h) {
                ImVec2 hs = v.toScreen(handlePos(sh.r, h));
                dl->AddRectFilled(ImVec2(hs.x - hp, hs.y - hp), ImVec2(hs.x + hp, hs.y + hp), IM_COL32(255, 255, 255, 255));
                dl->AddRect(ImVec2(hs.x - hp, hs.y - hp), ImVec2(hs.x + hp, hs.y + hp), IM_COL32(0, 0, 0, 255));
            }
            ImVec2 rc;
            if (rotHandle(sh, rc)) {
                ImVec2 ts = v.toScreen(handlePos(sh.r, 1)), rs = v.toScreen(rc);
                dl->AddLine(ts, rs, IM_COL32(255, 255, 255, 200), 1.5f);
                dl->AddCircleFilled(rs, hp, col::Accent);
                dl->AddCircle(rs, hp, IM_COL32(0, 0, 0, 255));
            }
        }
    }
    if (!std::isnan(g_ed.guideX)) dl->AddLine(v.toScreen({g_ed.guideX, -100000}), v.toScreen({g_ed.guideX, 100000}), col::Accent, 1);
    if (!std::isnan(g_ed.guideY)) dl->AddLine(v.toScreen({-100000, g_ed.guideY}), v.toScreen({100000, g_ed.guideY}), col::Accent, 1);
    if (hovered) {
        char info[64]; snprintf(info, sizeof(info), "%.0f, %.0f", mouseC.x, mouseC.y);
        dl->AddText(ImVec2(c0.x + 8, c1.y - 20), col::TextDim, info);
    }
    dl->PopClipRect();

    // menu de contexto
    if (ImGui::BeginPopupContextItem("canvasctx")) {
        if (ImGui::MenuItem(T("Adicionar slice aqui"))) {
            app.checkpoint();
            Slice& n = p.addSlice(sc, T("Slice ") + std::to_string(sc.slices.size() + 1));
            Shape sh = getShape(n, tab);
            sh.r.x = std::round(mouseC.x - sh.r.w * 0.5f); sh.r.y = std::round(mouseC.y - sh.r.h * 0.5f);
            setShape(n, tab, sh);
            app.ui.selSlice = n.id;
            selected = nullptr;
        }
        if (selected) {
            if (ImGui::MenuItem(T("Duplicar"), "Ctrl+D")) { duplicateSlice(app, sc, *selected); selected = nullptr; }
            if (selected && ImGui::MenuItem(T("Rodar 90°"), nullptr, false, tab == 1 && !selected->outWarp)) { app.checkpoint(); selected->rotation = std::fmod(selected->rotation + 90.f, 360.f); }
            bool warped = selected && (tab == 0 ? selected->inWarp : selected->outWarp);
            if (selected && ImGui::MenuItem(T("Repor retângulo (tirar perspetiva)"), nullptr, false, warped)) {
                app.checkpoint(); (tab == 0 ? selected->inWarp : selected->outWarp) = false;
            }
            if (selected && ImGui::MenuItem(T("Saída 1:1"))) { app.checkpoint(); selected->outWarp = false; selected->outW = selected->input.w; selected->outH = selected->input.h; }
            if (selected && ImGui::MenuItem(T("Ocupar o canvas inteiro"))) {
                app.checkpoint(); Shape sh; sh.warp = false; sh.r = {0, 0, cw, ch, 0}; setShape(*selected, tab, sh);
            }
            if (selected && ImGui::MenuItem(T("Apagar"), "Del")) deleteSlice(app, sc, selected->id);
        }
        ImGui::EndPopup();
    }
}

// botão de ferramenta (ativo = destacado)
bool toolButton(const char* label, bool active, const char* help) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    bool r = ImGui::Button(label);
    if (active) ImGui::PopStyleColor();
    hint(help);
    return r;
}

void toolbar(App& app, Screen* sc) {
    int tab = app.ui.editorTab;
    char z[32];
    snprintf(z, sizeof(z), "%.0f%%", app.ui.zoom[tab] * 100);
    ImGui::SetNextItemWidth(80);
    if (ImGui::BeginCombo("##zoom", z)) {
        if (ImGui::Selectable(T("Ajustar"))) app.ui.zoom[tab] = 0;
        const float zs[] = {0.25f, 0.5f, 1.f, 2.f, 4.f};
        for (float f : zs) {
            char l[16]; snprintf(l, sizeof(l), "%.0f%%", f * 100);
            if (ImGui::Selectable(l)) { app.ui.zoom[tab] = f; app.ui.pan[tab][0] = app.ui.pan[tab][1] = 0; }
        }
        ImGui::EndCombo();
    }
    hint(T("Zoom do canvas (também com a roda do rato)."));
    ImGui::SameLine();
    ImGui::BeginDisabled(!app.canUndo());
    if (ImGui::Button(T("Desfazer"))) app.undo();
    ImGui::EndDisabled();
    hint(T("Desfazer (Ctrl+Z)"));
    ImGui::SameLine(0, 2);
    ImGui::BeginDisabled(!app.canRedo());
    if (ImGui::Button(T("Refazer"))) app.redo();
    ImGui::EndDisabled();
    hint(T("Refazer (Ctrl+Y)"));
    ImGui::SameLine(0, 12);
    if (toolButton(T("Mão"), app.ui.handTool, T("Mover a vista com o rato (também: botão do meio ou Espaço + arrastar)."))) app.ui.handTool = !app.ui.handTool;
    ImGui::SameLine(0, 12);
    if (toolButton(T("Transformar"), !app.ui.editPoints, T("Mover, redimensionar e rodar slices inteiras."))) { app.ui.editPoints = false; app.ui.handTool = false; }
    ImGui::SameLine(0, 2);
    if (toolButton(T("Editar pontos"), app.ui.editPoints, T("Arrastar os 4 cantos de cada slice (perspetiva) para encaixar em paredes, ecrãs inclinados, etc."))) { app.ui.editPoints = true; app.ui.handTool = false; }
    ImGui::SameLine(0, 12);
    ImGui::Checkbox(T("Snap"), &app.ui.snap);
    hint(T("Encaixar em bordas, centros e cantos (Alt desliga temporariamente)."));
    ImGui::SameLine();
    ImGui::Checkbox(T("Grelha"), &app.ui.showGrid);
    if (app.ui.showGrid) { ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::InputInt("##grid", &app.ui.gridSize, 0); app.ui.gridSize = std::clamp(app.ui.gridSize, 2, 512); }
    ImGui::SameLine(0, 12);
    const char* bgs[] = {T("Programa (no ar)"), T("Preview"), T("Nenhum")};
    ImGui::TextUnformatted(T("Fundo:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    ImGui::Combo("##bg", &app.ui.editorBg, bgs, 3);
    hint(T("Vídeo mostrado por baixo das slices, para as alinhar com o conteúdo."));
    if (app.ui.editorBg != 2) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::SliderFloat("##bga", &app.ui.editorBgAlpha, 0.1f, 1.f, "%.2f");
        hint(T("Opacidade do vídeo de fundo"));
    }
    if (sc) {
        ImGui::SameLine(0, 12);
        if (ImGui::Checkbox(T("Test pattern"), &sc->testPattern)) app.markDirty();
        hint(T("Desenha xadrez, círculo e diagonais em cada slice, na saída real."));
    }
}

} // namespace

bool rectEditor(App& app, const char* id, Rect& r, float maxW, float maxH) {
    ImGui::PushID(id);
    bool changed = false;
    float xy[2] = {r.x, r.y}, wh[2] = {r.w, r.h};
    if (ImGui::DragFloat2(T("X / Y"), xy, 1.f, -maxW * 4, maxW * 4, "%.0f")) { r.x = std::round(xy[0]); r.y = std::round(xy[1]); changed = true; }
    undoOnActivate(app);
    if (ImGui::DragFloat2(T("L / A"), wh, 1.f, 1.f, std::max(maxW, maxH) * 8, "%.0f")) { r.w = std::max(1.f, std::round(wh[0])); r.h = std::max(1.f, std::round(wh[1])); changed = true; }
    undoOnActivate(app);
    hint(T("Largura / Altura em píxeis"));
    ImGui::PopID();
    return changed;
}

void drawSliceEditor(App& app) {
    if (app.ui.focusEditor) { app.ui.showAdvOutput = true; app.ui.focusEditor = false; ImGui::SetNextWindowFocus(); }
    if (!app.ui.showAdvOutput) { g_ed.wasOpen = false; g_ed.mode = Drag::None; return; }
    if (!g_ed.wasOpen) {
        // ao abrir: guardar o estado para "Cancelar"
        g_ed.snapshot = app.project.screens;
        g_ed.wasOpen = true;
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.92f, vp->WorkSize.y * 0.9f), ImGuiCond_Always);
        ImGui::SetNextWindowPos(vp->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    g_help = nullptr;
    std::string title = winTitle("Saída avançada", "advout");
    bool open = true;
    if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!open) app.ui.showAdvOutput = false;
        return;
    }
    Screen* sc = selectedScreen(app);
    const float footer = ImGui::GetFrameHeightWithSpacing() + 10;
    const float bodyH = std::max(200.f, ImGui::GetContentRegionAvail().y - footer);

    // esquerda: lista + ajuda
    ImGui::BeginGroup();
    ImGui::BeginChild("list", ImVec2(260, bodyH - 134), ImGuiChildFlags_Borders);
    screenList(app);
    ImGui::EndChild();
    ImGui::BeginChild("help", ImVec2(260, 128), ImGuiChildFlags_Borders);
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.75f, 1), "%s", T("Ajuda"));
    ImGui::Separator();
    static std::string lastHelp;
    if (g_help) lastHelp = g_help;
    ImGui::TextWrapped("%s", lastHelp.empty() ? T("Passa o rato por cima de um elemento para ver aqui o que faz.") : lastHelp.c_str());
    ImGui::EndChild();
    ImGui::EndGroup();
    ImGui::SameLine();

    // centro: separadores + barra + canvas
    const float propsW = 320;
    ImGui::BeginChild("center", ImVec2(ImGui::GetContentRegionAvail().x - propsW - 8, bodyH), ImGuiChildFlags_None);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem(T("Input Selection"))) { app.ui.editorTab = 0; ImGui::EndTabItem(); }
        hint(T("Que zona da composição cada slice recorta."));
        if (ImGui::BeginTabItem(T("Output Transformation"))) { app.ui.editorTab = 1; ImGui::EndTabItem(); }
        hint(T("Onde e com que forma cada slice aparece na saída."));
        ImGui::EndTabBar();
    }
    toolbar(app, sc);
    if (sc) canvas(app, *sc, ImGui::GetContentRegionAvail());
    else ImGui::TextDisabled("%s", T("Adiciona um ecrã com o botão +."));
    ImGui::EndChild();
    ImGui::SameLine();

    // direita: propriedades (slice selecionada, senão o ecrã)
    ImGui::BeginChild("props", ImVec2(propsW, bodyH), ImGuiChildFlags_Borders);
    if (sc) {
        ImGui::PushItemWidth(-110);
        if (Slice* sl = selectedSlice(app, *sc)) sliceProps(app, *sc, *sl);
        else screenProps(app, *sc);
        ImGui::PopItemWidth();
    }
    ImGui::EndChild();

    // rodapé
    ImGui::Separator();
    float bw = 150;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - bw * 2 - 8);
    if (ImGui::Button(T("Cancelar"), ImVec2(bw, 0))) {
        app.checkpoint();
        app.project.screens = g_ed.snapshot;
        app.ui.selSlice = 0;
        app.ui.showAdvOutput = false;
    }
    hint(T("Desfaz tudo o que foi alterado desde que esta janela abriu."));
    ImGui::SameLine();
    if (ImGui::Button(T("Gravar e fechar"), ImVec2(bw, 0))) { app.markDirty(); app.ui.showAdvOutput = false; }
    ImGui::End();
    if (!open) app.ui.showAdvOutput = false;   // X = manter as alterações
}

} // namespace ps
