#include "ui/UI.h"
#include "audio/AudioEngine.h"
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
    ImGuiID right, left, bottomLeft, rightTop, rightBottom;
    ImGui::DockBuilderSplitNode(dock, ImGuiDir_Right, 0.24f, &right, &left);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.5f, &rightBottom, &rightTop);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.36f, &bottomLeft, &left);
    ImGuiID bl1, bl2;
    ImGui::DockBuilderSplitNode(bottomLeft, ImGuiDir_Left, 0.55f, &bl1, &bl2);
    ImGui::DockBuilderDockWindow("###clips", left);
    ImGui::DockBuilderDockWindow("###comp", bl1);
    ImGui::DockBuilderDockWindow("###monitor", bl2);
    ImGui::DockBuilderDockWindow("###effects", bl2);
    ImGui::DockBuilderDockWindow("###log", bl2);
    ImGui::DockBuilderDockWindow("###cliptime", bl2);
    ImGui::DockBuilderDockWindow("###timecode", bl2);
    ImGui::DockBuilderDockWindow("###cues", bl2);
    ImGui::DockBuilderDockWindow("###props_clip", rightTop);
    ImGui::DockBuilderDockWindow("###props_col", rightTop);
    ImGui::DockBuilderDockWindow("###props_layer", rightBottom);
    ImGui::DockBuilderDockWindow("###props_comp", rightBottom);
    ImGui::DockBuilderFinish(dock);
}

static void fitImage(ImTextureID tex, float tw, float th, ImVec2 avail, ImVec2* outMin = nullptr, float* outScale = nullptr) {
    if (tw <= 0 || th <= 0 || avail.x <= 0 || avail.y <= 0) return;
    float s = std::min(avail.x / tw, avail.y / th);
    if (outScale) *outScale = s;
    ImVec2 sz(tw * s, th * s);
    ImVec2 c = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(c.x + (avail.x - sz.x) * 0.5f, c.y + (avail.y - sz.y) * 0.5f));
    ImGui::Image(tex, sz);
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    if (outMin) *outMin = p0;
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

static void viewCombo(App& app, int& mode, uint32_t& id, const char* cid) {
    Project& p = app.project;
    if ((mode == 1 && !p.findScreen(id)) || (mode == 2 && !p.findSlice(id))) { mode = 0; id = 0; }
    ImGui::SetNextItemWidth(std::min(260.f, ImGui::GetContentRegionAvail().x));
    std::string cur = viewLabel(app, mode, id);
    if (ImGui::BeginCombo(cid, cur.c_str())) {
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

// ---------------------------------------------------------------------------
// moldura de transformação do clip no monitor (mover / esticar com o rato)
// ---------------------------------------------------------------------------
struct ClipGizmoState {
    bool active = false; int handle = -1; std::string mon;
    ImVec2 mouse0; float offX = 0, offY = 0, sx = 1, sy = 1, w0 = 1, h0 = 1;
};
static ClipGizmoState g_cg;

// clip a transformar: preview → o clip em preview; programa → o clip no ar da layer selecionada
// (ou, se essa layer não tiver nada no ar, a layer de cima que tiver)
static bool gizmoTarget(App& app, bool prev, int& li, int& ci, VideoPlayer*& v) {
    Project& p = app.project;
    li = ci = -1; v = nullptr;
    if (prev) {
        if (!app.comp.hasPreview()) return false;
        li = p.layerIndex(app.comp.previewLayerId()); ci = app.comp.previewStack() == p.comp.activeStack ? app.comp.previewColumn() : -1;
        v = app.comp.previewPlayer();
    } else {
        li = app.ui.selLayer;
        if (li < 0 || li >= (int)p.comp.layers.size() || p.comp.layers[li].activeColumn < 0) {
            li = -1;
            for (int i = 0; i < (int)p.comp.layers.size(); ++i) if (p.comp.layers[i].activeColumn >= 0) { li = i; break; }
        }
        if (li < 0) return false;
        ci = p.comp.layers[li].activeColumn;
        v = app.comp.activePlayer(p.comp.layers[li].id);
    }
    if (li < 0 || li >= (int)p.comp.layers.size() || ci < 0 || ci >= (int)p.comp.layers[li].clips.size()) return false;
    return v && !p.comp.layers[li].clips[ci].empty();
}

static void clipGizmo(App& app, bool prev, ImVec2 imgMin, float scale, const char* monId) {
    Project& p = app.project;
    int li = -1, ci = -1;
    VideoPlayer* v = nullptr;
    if (!gizmoTarget(app, prev, li, ci, v)) return;
    Layer& l = p.comp.layers[li];
    Clip& c = l.clips[ci];
    if (c.empty() || !v || v->width() <= 0) return;
    float px[4], py[4];
    Compositor::clipCorners(p, l, c, (float)v->width(), (float)v->height(), px, py);
    ImVec2 q[4];   // TL, TR, BR, BL no ecrã
    const int ord[4] = {0, 1, 3, 2};
    for (int i = 0; i < 4; ++i) q[i] = ImVec2(imgMin.x + px[ord[i]] * scale, imgMin.y + py[ord[i]] * scale);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const bool mine = g_cg.active && g_cg.mon == monId;
    dl->AddQuad(q[0], q[1], q[2], q[3], mine ? col::Accent : IM_COL32(64, 224, 192, 170), 1.5f);
    // pegas: 0 TL, 1 TR, 2 BR, 3 BL
    const int hx[4] = {-1, 1, 1, -1}, hy[4] = {-1, -1, 1, 1};
    const float hp = 5.f;
    int hoverH = -1;
    for (int i = 0; i < 4; ++i) {
        dl->AddRectFilled(ImVec2(q[i].x - hp, q[i].y - hp), ImVec2(q[i].x + hp, q[i].y + hp), IM_COL32(255, 255, 255, 255));
        dl->AddRect(ImVec2(q[i].x - hp, q[i].y - hp), ImVec2(q[i].x + hp, q[i].y + hp), IM_COL32(0, 0, 0, 255));
        if (std::fabs(io.MousePos.x - q[i].x) <= hp + 2 && std::fabs(io.MousePos.y - q[i].y) <= hp + 2) hoverH = i;
    }
    // dentro do quadrilátero?
    auto inside = [&](ImVec2 m) {
        bool in = false;
        for (int i = 0, j = 3; i < 4; j = i++)
            if (((q[i].y > m.y) != (q[j].y > m.y)) && (m.x < (q[j].x - q[i].x) * (m.y - q[i].y) / (q[j].y - q[i].y) + q[i].x)) in = !in;
        return in;
    };
    bool winHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    if (!g_cg.active && winHovered) {
        if (hoverH >= 0) ImGui::SetMouseCursor((hx[hoverH] == hy[hoverH]) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
        else if (inside(io.MousePos)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && inside(io.MousePos)) {
            app.checkpoint(); c.offsetX = c.offsetY = 0; c.scale = c.scaleX = c.scaleY = 1; c.rotation = 0;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (hoverH >= 0 || inside(io.MousePos))) {
            app.checkpoint();
            g_cg = {};
            g_cg.active = true; g_cg.handle = hoverH; g_cg.mon = monId;
            g_cg.mouse0 = io.MousePos;
            g_cg.offX = c.offsetX; g_cg.offY = c.offsetY; g_cg.sx = c.scaleX; g_cg.sy = c.scaleY;
            g_cg.w0 = std::hypot(px[1] - px[0], py[1] - py[0]);
            g_cg.h0 = std::hypot(px[2] - px[0], py[2] - py[0]);
        }
    }
    if (mine) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { g_cg.active = false; return; }
        // delta em píxeis da composição, no referencial da layer (desfazer escala da layer)
        float ls = std::max(0.01f, l.scale);
        float dx = (io.MousePos.x - g_cg.mouse0.x) / scale / ls, dy = (io.MousePos.y - g_cg.mouse0.y) / scale / ls;
        if (g_cg.handle < 0) {
            if (io.KeyShift) { if (std::fabs(dx) > std::fabs(dy)) dy = 0; else dx = 0; }
            c.offsetX = std::round(g_cg.offX + dx); c.offsetY = std::round(g_cg.offY + dy);
        } else {
            int h = g_cg.handle;
            float w0 = std::max(1.f, g_cg.w0 / ls), h0 = std::max(1.f, g_cg.h0 / ls);
            float nw = std::max(4.f, w0 + hx[h] * dx), nh = std::max(4.f, h0 + hy[h] * dy);
            if (io.KeyShift) { float k = std::max(nw / w0, nh / h0); nw = w0 * k; nh = h0 * k; }   // manter proporção
            c.scaleX = g_cg.sx * nw / w0;
            c.scaleY = g_cg.sy * nh / h0;
            // a esquina oposta fica parada
            c.offsetX = g_cg.offX + hx[h] * (nw - w0) * 0.5f;
            c.offsetY = g_cg.offY + hy[h] * (nh - h0) * 0.5f;
        }
        app.markDirty();
        char info[80];
        snprintf(info, sizeof(info), "%.0f, %.0f  ·  %.0f%% x %.0f%%", c.offsetX, c.offsetY, c.scale * c.scaleX * 100, c.scale * c.scaleY * 100);
        dl->AddText(ImVec2(q[0].x + 4, q[0].y - 18), col::Accent, info);
    }
}

static void monitorImage(App& app, bool prev, int mode, uint32_t id, ImVec2 avail, const char* monId) {
    Project& p = app.project;
    if (mode == 0) {
        const RenderTarget& rt = prev ? app.comp.previewRT() : app.comp.compRT();
        ImVec2 imgMin; float sc = 0;
        if (rt.valid()) fitImage(texId(rt.srv.Get()), (float)rt.w, (float)rt.h, avail, &imgMin, &sc);
        if (sc > 0 && prev && app.ui.monitorGizmo) clipGizmo(app, prev, imgMin, sc, monId);
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

// coloca o próximo elemento na mesma linha se couber; senão passa para a linha seguinte
static void flowNext(float w) {
    ImGuiStyle& st = ImGui::GetStyle();
    float limit = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + w <= limit) ImGui::SameLine();
}
static float buttonW(const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2; }
static float checkW(const char* label) { return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label, nullptr, true).x; }

static void monitorPanel(App& app, bool prev, int& mode, uint32_t& id, const char* cid, ImVec2 size) {
    ImGui::BeginChild(cid, size, ImGuiChildFlags_Borders);
    ImVec4 tagCol = prev ? ImVec4(0.95f, 0.67f, 0.16f, 1) : ImVec4(0.92f, 0.25f, 0.25f, 1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(tagCol, "%s", prev ? T("PREVIEW") : T("PROGRAMA"));
    ImGui::SameLine();
    std::string combo = std::string("##view") + cid;
    viewCombo(app, mode, id, combo.c_str());
    if (prev) {
        const bool narrow = ImGui::GetContentRegionAvail().x < 430;   // textos curtos em painéis estreitos
        bool has = app.comp.hasPreview();
        std::string key = bindingLabel(settings().keys[(int)Action::TakePreview]);
        std::string take = narrow ? std::string(T("Para programa")) : std::string(T("Enviar para programa")) + "  (" + key + ")";
        take += "##take";
        ImGui::BeginDisabled(!has);
        if (ImGui::Button(take.c_str())) app.comp.takePreview(app.project);
        if (narrow && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s (%s)", T("Enviar para programa"), key.c_str());
        flowNext(buttonW(T("Limpar")));
        if (ImGui::Button(T("Limpar"))) app.comp.clearPreview();
        ImGui::EndDisabled();
        if (mode == 0) {
            std::string tr = std::string(T("Transformar")) + "##" + cid;
            flowNext(checkW(tr.c_str()));
            ImGui::Checkbox(tr.c_str(), &app.ui.monitorGizmo);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Moldura para mover e esticar o clip em preview (Shift = proporção; duplo clique = tamanho nativo)."));
            std::string big = std::string(T("Janela grande")) + "##big" + cid;
            flowNext(buttonW(big.c_str()));
            if (ImGui::Button(big.c_str())) { app.ui.showBigTransform = true; app.ui.bigPrev = prev; ImGui::SetWindowFocus("###bigtransform"); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Abre uma janela grande para transformar com precisão."));
        }
        // nome do clip: no espaço que sobrar na linha (cortado), ou em baixo
        if (has) {
            const char* nm = app.comp.previewClipInfo().name.c_str();
            float tw = ImGui::CalcTextSize(nm).x;
            float limit = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            float room = limit - (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x);
            if (room >= std::min(tw, 80.f)) ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImVec2 c0 = ImGui::GetCursorScreenPos();
            ImGui::PushClipRect(c0, ImVec2(limit, c0.y + ImGui::GetFrameHeight()), true);
            ImGui::TextDisabled("%s", nm);
            ImGui::PopClipRect();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", nm);
        }
    }
    monitorImage(app, prev, mode, id, ImGui::GetContentRegionAvail(), cid);
    ImGui::EndChild();
}

static void compositionWindow(App& app) {
    if (!app.ui.panel.monitors) return;
    if (!ImGui::Begin(winTitle("Preview / Programa", "comp").c_str(), &app.ui.panel.monitors)) { ImGui::End(); return; }
    ImVec2 a = ImGui::GetContentRegionAvail();
    bool sideBySide = a.x > a.y * 1.2f;
    const float bar = 8.0f;   // divisória arrastável
    float& f = app.ui.monSplit;
    f = std::clamp(f, 0.15f, 0.85f);
    auto splitter = [&](bool vertical, float total) {
        ImGui::InvisibleButton("##monsplit", vertical ? ImVec2(bar, a.y) : ImVec2(a.x, bar));
        bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
        if (hov || act) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        if (act) f = std::clamp(f + (vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y) / std::max(1.f, total), 0.15f, 0.85f);
        if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) f = 0.5f;
        if (hov) ImGui::SetTooltip("%s", T("Arrasta para mudar o tamanho (duplo clique = metade)"));
        ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
        ImU32 c = act ? col::Accent : hov ? IM_COL32(110, 115, 125, 255) : IM_COL32(60, 63, 70, 255);
        if (vertical) ImGui::GetWindowDrawList()->AddLine(ImVec2((r0.x + r1.x) * 0.5f, r0.y + 4), ImVec2((r0.x + r1.x) * 0.5f, r1.y - 4), c, 2.0f);
        else ImGui::GetWindowDrawList()->AddLine(ImVec2(r0.x + 4, (r0.y + r1.y) * 0.5f), ImVec2(r1.x - 4, (r0.y + r1.y) * 0.5f), c, 2.0f);
    };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    if (sideBySide) {
        float w = a.x - bar;
        float w0 = std::floor(w * f);
        monitorPanel(app, true, app.ui.viewMode[0], app.ui.viewId[0], "mon0", ImVec2(w0, a.y));
        ImGui::SameLine(); splitter(true, w); ImGui::SameLine();
        monitorPanel(app, false, app.ui.viewMode[1], app.ui.viewId[1], "mon1", ImVec2(w - w0, a.y));
    } else {
        float h = a.y - bar;
        float h0 = std::floor(h * f);
        monitorPanel(app, true, app.ui.viewMode[0], app.ui.viewId[0], "mon0", ImVec2(a.x, h0));
        splitter(false, h);
        monitorPanel(app, false, app.ui.viewMode[1], app.ui.viewId[1], "mon1", ImVec2(a.x, h - h0));
    }
    ImGui::PopStyleVar();
    ImGui::End();
}

// janela grande de transformação (não é modal: as saídas continuam a correr)
static void bigTransformWindow(App& app) {
    if (!app.ui.showBigTransform) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.8f, vp->WorkSize.y * 0.8f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(500, 350), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin((std::string(T("Transformar")) + "###bigtransform").c_str(), &app.ui.showBigTransform, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    Project& p = app.project;
    const bool prev = true;   // a transformação faz-se só no preview
    ImGui::TextColored(ImVec4(0.95f, 0.67f, 0.16f, 1), "%s", T("PREVIEW"));
    int li, ci; VideoPlayer* v;
    bool has = gizmoTarget(app, prev, li, ci, v);
    ImGui::SameLine();
    if (has) ImGui::TextColored(ImVec4(0.25f, 0.88f, 0.75f, 1), "%s  ·  %s", p.comp.layers[li].name.c_str(), p.comp.layers[li].clips[ci].name.c_str());
    else ImGui::TextDisabled("%s", T("Sem clip em preview (clica na metade de baixo de um clip)."));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("  Arrasta = mover · cantos = esticar · Shift = proporção · duplo clique = repor"));

    const float sideW = 300;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##bigimg", ImVec2(std::max(100.f, avail.x - sideW - 8), avail.y), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    {
        const RenderTarget& rt = app.comp.previewRT();
        ImVec2 imgMin; float sc = 0;
        ImVec2 a = ImGui::GetContentRegionAvail();
        if (rt.valid()) fitImage(texId(rt.srv.Get()), (float)rt.w, (float)rt.h, a, &imgMin, &sc);
        if (sc > 0) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 imgMax(imgMin.x + p.comp.width * sc, imgMin.y + p.comp.height * sc);
            dl->AddRect(imgMin, imgMax, IM_COL32(120, 125, 135, 200));
            float cx = (imgMin.x + imgMax.x) * 0.5f, cy = (imgMin.y + imgMax.y) * 0.5f;
            dl->AddLine(ImVec2(cx, imgMin.y), ImVec2(cx, imgMax.y), IM_COL32(255, 255, 255, 40));
            dl->AddLine(ImVec2(imgMin.x, cy), ImVec2(imgMax.x, cy), IM_COL32(255, 255, 255, 40));
            clipGizmo(app, prev, imgMin, sc, "big");
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##bigside", ImVec2(0, avail.y), ImGuiChildFlags_Borders);
    if (has) {
        ImGui::PushItemWidth(-110);
        clipTransformControls(app, p.comp.layers[li].clips[ci]);
        ImGui::PopItemWidth();
    } else ImGui::TextWrapped("%s", T("Escolhe um clip para transformar."));
    ImGui::EndChild();
    ImGui::End();
}

// monitores extra (menu Layout)
static void extraMonitors(App& app) {
    for (auto& m : app.ui.extraMonitors) {
        if (!m.open) continue;
        std::string title = std::string(m.preview ? T("Preview") : T("Programa")) + " " + std::to_string(m.id) + "###mon" + std::to_string(m.id);
        ImGui::SetNextWindowSize(ImVec2(480, 320), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(title.c_str(), &m.open)) {
            std::string cid = "em" + std::to_string(m.id);
            monitorPanel(app, m.preview, m.mode, m.viewId, cid.c_str(), ImGui::GetContentRegionAvail());
        }
        ImGui::End();
    }
}

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
    if (!app.ui.panel.outputs) return;
    if (!ImGui::Begin(winTitle("Monitor de saídas", "monitor").c_str(), &app.ui.panel.outputs)) { ImGui::End(); return; }
    Project& p = app.project;
    outputsButton(app, ImVec2(260, 0));
    ImGui::SameLine();
    if (ImGui::Button(T("Ligar todas"))) app.setAllOutputs(true);
    ImGui::SameLine();
    if (ImGui::Button(T("Desligar todas"))) app.setAllOutputs(false);
    ImGui::Separator();
    if (p.screens.empty()) ImGui::TextDisabled("%s", T("Sem ecrãs. Cria um em Saídas → Saída avançada."));
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

// definições da composição (nome, descrição, tamanho, frame rate, profundidade de cor)
void compositionSettingsBody(App& app, bool popup) {
    Composition& c = app.project.comp;
    static char name[128], desc[1024];
    static int wh[2], fpsIdx, depthIdx;
    static bool init = false;
    static const float fpsVals[] = {0, 23.976f, 24, 25, 29.97f, 30, 48, 50, 59.94f, 60, 120};
    const char* fpsNames[] = {T("Automático"), "23.976", "24", "25", "29.97", "30", "48", "50", "59.94", "60", "120"};
    auto load = [&] {
        snprintf(name, sizeof(name), "%s", c.name.c_str());
        snprintf(desc, sizeof(desc), "%s", c.description.c_str());
        wh[0] = c.width; wh[1] = c.height;
        fpsIdx = 0;
        for (int i = 0; i < 11; ++i) if (std::fabs(fpsVals[i] - c.fps) < 0.01f) fpsIdx = i;
        depthIdx = c.colorDepth == 16 ? 1 : 0;
        init = true;
    };
    if (!init || (popup && ImGui::IsWindowAppearing())) load();
    ImGui::InputText(T("Nome"), name, sizeof(name));
    ImGui::InputTextMultiline(T("Descrição"), desc, sizeof(desc), ImVec2(0, 70));
    ImGui::InputInt2(T("Tamanho"), wh);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##presets", ImGuiDir_Down)) ImGui::OpenPopup("presets");
    if (ImGui::BeginPopup("presets")) {
        const struct { const char* n; int w, h; } presets[] = {
            {"1280x720", 1280, 720}, {"1920x1080", 1920, 1080}, {"2560x1440", 2560, 1440}, {"3840x2160 (4K)", 3840, 2160},
            {"5760x1080 (3x HD)", 5760, 1080}, {"7680x1080 (4x HD)", 7680, 1080}, {"7680x2160 (2x 4K)", 7680, 2160},
            {"7680x4320 (8K)", 7680, 4320}};
        for (auto& pr : presets) if (ImGui::Selectable(pr.n)) { wh[0] = pr.w; wh[1] = pr.h; }
        ImGui::EndPopup();
    }
    ImGui::Combo(T("Frame rate"), &fpsIdx, fpsNames, 11);
    const char* depths[] = {"8 bpc", "16 bpc (float)"};
    ImGui::Combo(T("Profundidade de cor"), &depthIdx, depths, 2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("16 bpc: gradientes sem bandas e efeitos mais precisos; usa o dobro da memória da placa."));
    ImGui::TextDisabled(T("Máximo da GPU: 16384x16384. VRAM: %zu MB"), gfx().vramMB());
    ImGui::Spacing();
    if (ImGui::Button(T("Aplicar"), ImVec2(120, 0))) {
        app.checkpoint();
        c.name = name; c.description = desc;
        c.width = std::clamp(wh[0], 16, 16384);
        c.height = std::clamp(wh[1], 16, 16384);
        c.fps = fpsVals[fpsIdx];
        c.colorDepth = depthIdx ? 16 : 8;
        load();
        if (popup) ImGui::CloseCurrentPopup();
    }
}

static void compSettingsPopup(App& app) {
    std::string title = winTitle("Definições da composição", "compsettings");
    if (app.ui.showCompSettings) { ImGui::OpenPopup(title.c_str()); app.ui.showCompSettings = false; }
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
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
    if (!app.ui.panel.log) return;
    if (!ImGui::Begin(winTitle("Registo", "log").c_str(), &app.ui.panel.log)) { ImGui::End(); return; }
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
    // fps das SAÍDAS (o que vai para os ecrãs), comparado com o do projeto; a interface fica em tooltip
    ImGui::AlignTextToFramePadding();
    const float target = app.project.comp.fps;
    const double outFps = app.outputs.outputFps();
    if (app.outputs.count() > 0 && outFps > 0) {
        double ref = target > 0 ? target : outFps;
        ImVec4 c = outFps >= ref * 0.97 ? ImVec4(0.4f, 0.9f, 0.5f, 1) : outFps >= ref * 0.85 ? ImVec4(1, 0.75f, 0.3f, 1) : ImVec4(1, 0.4f, 0.4f, 1);
        if (target > 0) ImGui::TextColored(c, T("Saídas %.2f fps  (projeto %.3g)"), outFps, target);
        else ImGui::TextColored(c, T("Saídas %.2f fps"), outFps);
    } else {
        if (target > 0) ImGui::TextColored(ImVec4(0.6f, 0.62f, 0.66f, 1), T("Projeto %.3g fps  ·  sem saídas abertas"), target);
        else ImGui::TextColored(ImVec4(0.6f, 0.62f, 0.66f, 1), "%s", T("Projeto: fps automático  ·  sem saídas abertas"));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("Interface: %.0f fps (%.2f ms). O valor principal é o ritmo real a que as saídas recebem imagens."), app.fps, app.frameMs);
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine();
    {
        VuLevel mv = AudioEngine::get().masterVu();
        float y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(y + (ImGui::GetFrameHeight() - 10) * 0.5f);
        vuItem(mv.l, mv.r, 90, 10);
        ImGui::SameLine();
        ImGui::SetCursorPosY(y);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", app.project.comp.audioMute ? T("som mudo") : T("som"));
    }
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
            if (ImGui::MenuItem(T("Definições da composição..."))) app.ui.showCompSettings = true;
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
            if (ImGui::MenuItem(T("Saída avançada..."), menuKey(Action::OutputEditor).c_str())) app.ui.focusEditor = true;
            if (ImGui::MenuItem(T("Resolução da composição..."))) app.ui.showCompSettings = true;
            if (ImGui::MenuItem(T("Detetar monitores de novo"))) app.refreshDisplays();
            ImGui::Separator();
            ImGui::TextDisabled("%s", T("Saídas detetadas:"));
            for (auto& d : app.displays) ImGui::TextDisabled("  %s", d.label().c_str());
            ImGui::EndMenu();
        }
        drawLayoutMenu(app);
        if (ImGui::BeginMenu(T("Ajuda"))) {
            if (ImGui::MenuItem(T("Atalhos de teclado"))) openSettings(7);
            if (ImGui::MenuItem(T("Como usar"))) openSettings(10);
            if (ImGui::MenuItem(T("Novidades desta versão"))) openSettings(9);
            ImGui::Separator();
            if (ImGui::MenuItem(T("Sobre o PLAYERSLICE"))) openSettings(11);
            ImGui::EndMenu();
        }
        // "Definições" abre logo a janela (sem submenu)
        if (ImGui::MenuItem(T("Definições"))) openSettings(app.ui.settingsTab);

        // test pattern em todas as saídas (atalho configurável)
        {
            ImGui::SameLine(0, 24);
            bool tp = app.anyTestPattern();
            std::string k = bindingLabel(settings().keys[(int)Action::TestPattern]);
            std::string lbl = std::string(T("Test pattern")) + (k.empty() ? "" : "  (" + k + ")") + "###tpbtn";
            ImGui::PushStyleColor(ImGuiCol_Button, tp ? ImVec4(0.75f, 0.52f, 0.1f, 1) : ImVec4(0.2f, 0.21f, 0.24f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tp ? ImVec4(0.85f, 0.6f, 0.15f, 1) : ImVec4(0.28f, 0.29f, 0.33f, 1));
            if (ImGui::SmallButton(lbl.c_str())) app.toggleTestPattern();
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Liga/desliga o test pattern animado em todas as saídas (para ver se há quebras de fps)."));
        }
        // indicador à direita na barra de menus
        const char* st = app.anyLive() ? T("● NO AR") : T("○ saídas desligadas");
        float tw = ImGui::CalcTextSize(st).x;
        ImGui::SameLine();
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
        const char* ids[] = {"###clips", "###comp", "###monitor", "###props_clip", "###props_layer", "###log"};
        for (const char* id : ids) {
            ImGuiWindow* w = ImGui::FindWindowByName(id);
            if (w && w->DockId == 0) { app.ui.layoutBuilt = false; PS_WARN("Disposição das janelas inválida — reposta"); break; }
        }
    }

    drawClipGrid(app);
    drawSliceEditor(app);
    compositionWindow(app);
    extraMonitors(app);
    bigTransformWindow(app);
    outputsMonitor(app);
    drawProperties(app);
    drawClipTime(app);
    drawTimecodePanel(app);
    drawCuesPanel(app);
    logWindow(app);
    statusBar(app);
    compSettingsPopup(app);
    drawSettings(app);
    drawUpdatePopup(app);

    // painéis soltos não podem ficar num monitor que está a ser usado como saída
    {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        ImGuiViewport* mainVp = ImGui::GetMainViewport();
        for (ImGuiWindow* w : g.Windows) {
            if (!w->WasActive || w->ParentWindow || (w->Flags & ImGuiWindowFlags_ChildWindow) || !w->Viewport || w->Viewport == mainVp || w->DockIsActive) continue;
            POINT c{(LONG)(w->Pos.x + w->Size.x * 0.5f), (LONG)(w->Pos.y + w->Size.y * 0.5f)};
            if (app.outputs.coversMonitor(MonitorFromPoint(c, MONITOR_DEFAULTTONULL))) {
                ImGui::SetWindowPos(w, ImVec2(mainVp->WorkPos.x + 80, mainVp->WorkPos.y + 80));
                PS_WARN("%s", T("Esse monitor está a ser usado como saída — a janela voltou para o ecrã principal."));
            }
        }
    }

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
