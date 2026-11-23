#include "ui/UI.h"
#include "app/App.h"
#include <imgui.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>
#include <imgui_internal.h>
#include "core/I18n.h"

namespace ps {

ImTextureID texId(void* srv) { return (ImTextureID)(uintptr_t)srv; }

ImU32 sliceColorU32(uint32_t c, int alpha) {
    return IM_COL32(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, alpha);
}

void undoOnActivate(App& app) {
    if (ImGui::IsItemActivated()) app.checkpoint();
    if (ImGui::IsItemDeactivatedAfterEdit()) app.markDirty();
}

bool valueSlider(App& app, const char* label, float* v, float lo, float hi, float def, const char* fmt) {
    ImGuiIO& io = ImGui::GetIO();
    float range = std::max(1e-6f, hi - lo);
    float w = ImGui::CalcItemWidth();
    float speed = range / std::max(100.f, w * 1.5f);   // arrastar a largura toda ≈ 2/3 do intervalo
    bool ch = ImGui::DragFloat(label, v, speed, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
    undoOnActivate(app);
    const bool hov = ImGui::IsItemHovered();
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { app.checkpoint(); *v = def; ch = true; app.markDirty(); }
    // barra de posição (do zero, se o intervalo tiver negativos)
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGuiStyle& st = ImGui::GetStyle();
    float x1 = b.x;
    if (label[0] && !(label[0] == '#' && label[1] == '#')) {
        // label visível à direita: a moldura acaba antes do texto
        const char* end = strstr(label, "##");
        float tw = ImGui::CalcTextSize(label, end).x;
        if (tw > 0) x1 = b.x - tw - st.ItemInnerSpacing.x;
    }
    float fw = x1 - a.x;
    if (fw > 8) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float t = std::clamp((*v - lo) / range, 0.f, 1.f);
        float z = (lo < 0 && hi > 0) ? (-lo / range) : 0.f;
        float y0 = b.y - 3.f, y1 = b.y - 1.f;
        dl->AddRectFilled(ImVec2(a.x + 2, y0), ImVec2(x1 - 2, y1), IM_COL32(255, 255, 255, 22));
        float p0 = a.x + 2 + (fw - 4) * std::min(t, z), p1 = a.x + 2 + (fw - 4) * std::max(t, z);
        if (p1 - p0 < 2) { p0 -= 1; p1 += 1; }
        dl->AddRectFilled(ImVec2(p0, y0), ImVec2(p1, y1), IM_COL32(64, 224, 192, 255));
    }
    if (hov && !ImGui::IsItemActive() && ImGui::GetCurrentContext()->HoveredIdTimer > 0.8f)
        ImGui::SetTooltip("%s", T("Arrastar = mudar  ·  duplo clique = escrever o valor  ·  botão direito = repor"));
    (void)io;
    return ch;
}

static float vuNorm(float peak) {
    float db = peak > 1e-5f ? 20.f * std::log10(peak) : -100.f;
    return std::clamp((db + 60.f) / 66.f, 0.f, 1.f);
}

void vuDraw(ImDrawList* dl, ImVec2 p0, ImVec2 size, float l, float r, bool vertical) {
    const float lv[2] = {vuNorm(l), vuNorm(r)};
    const float m12 = 48.f / 66.f, m3 = 57.f / 66.f;   // −12 dB e −3 dB
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(22, 23, 26, 255), 2);
    for (int c = 0; c < 2; ++c) {
        auto seg = [&](float a, float b, ImU32 col) {
            if (lv[c] <= a) return;
            b = std::min(b, lv[c]);
            if (vertical) {
                float w = size.x * 0.5f - 1, x = p0.x + c * (w + 2);
                dl->AddRectFilled(ImVec2(x, p0.y + size.y * (1 - b)), ImVec2(x + w, p0.y + size.y * (1 - a)), col);
            } else {
                float h = size.y * 0.5f - 0.5f, y = p0.y + c * (h + 1);
                dl->AddRectFilled(ImVec2(p0.x + size.x * a, y), ImVec2(p0.x + size.x * b, y + h), col);
            }
        };
        seg(0, m12, IM_COL32(60, 200, 110, 255));
        seg(m12, m3, IM_COL32(230, 200, 60, 255));
        seg(m3, 1, IM_COL32(235, 70, 60, 255));
    }
    // marca do 0 dB
    float z = 60.f / 66.f;
    if (vertical) dl->AddLine(ImVec2(p0.x, p0.y + size.y * (1 - z)), ImVec2(p0.x + size.x, p0.y + size.y * (1 - z)), IM_COL32(255, 255, 255, 70));
    else dl->AddLine(ImVec2(p0.x + size.x * z, p0.y), ImVec2(p0.x + size.x * z, p0.y + size.y), IM_COL32(255, 255, 255, 70));
}

void vuItem(float l, float r, float width, float height) {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(width, height));
    vuDraw(ImGui::GetWindowDrawList(), p0, ImVec2(width, height), l, r, false);
    if (ImGui::IsItemHovered()) {
        float pk = std::max(l, r);
        ImGui::SetTooltip("%s", pk > 1e-5f ? (std::to_string((int)std::lround(20.f * std::log10(pk))) + " dB").c_str() : "-inf dB");
    }
}

bool volumeSlider(App& app, const char* label, float* gain) {
    float db = *gain <= 0.001f ? -60.f : std::clamp(20.f * std::log10(*gain), -60.f, 12.f);
    bool ch = valueSlider(app, label, &db, -60.f, 12.f, 0.f, db <= -59.9f ? "-inf dB" : "%.1f dB");
    if (ch) *gain = db <= -59.9f ? 0.f : std::pow(10.f, db / 20.f);
    return ch;
}

bool inputTextStd(const char* label, std::string& s, float width) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", s.c_str());
    if (width > 0) ImGui::SetNextItemWidth(width);
    if (ImGui::InputText(label, buf, sizeof(buf))) { s = buf; return true; }
    return false;
}

void setupTheme(float dpi) {
    ImGuiIO& io = ImGui::GetIO();
    // Segoe UI (com acentos portugueses) se existir
    char winDir[MAX_PATH]; GetWindowsDirectoryA(winDir, MAX_PATH);
    std::string font = std::string(winDir) + "\\Fonts\\segoeui.ttf";
    float size = 15.f * dpi;
    if (GetFileAttributesA(font.c_str()) != INVALID_FILE_ATTRIBUTES) {
        static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2000, 0x206F, 0x2190, 0x21FF, 0x25A0, 0x25FF, 0};
        io.Fonts->AddFontFromFileTTF(font.c_str(), size, nullptr, ranges);
    } else {
        io.Fonts->AddFontDefault();
    }

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 3; s.FrameRounding = 3; s.GrabRounding = 3; s.TabRounding = 3;
    s.ScrollbarRounding = 3; s.ChildRounding = 3; s.PopupRounding = 3;
    s.WindowBorderSize = 1; s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(8, 8); s.FramePadding = ImVec2(6, 4); s.ItemSpacing = ImVec2(6, 5);
    s.ScaleAllSizes(dpi);
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) s.WindowRounding = 0.0f;

    ImVec4* c = s.Colors;
    auto rgb = [](int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); };
    c[ImGuiCol_Text]              = rgb(225, 228, 232);
    c[ImGuiCol_TextDisabled]      = rgb(120, 124, 130);
    c[ImGuiCol_WindowBg]          = rgb(28, 29, 32);
    c[ImGuiCol_ChildBg]           = rgb(24, 25, 28);
    c[ImGuiCol_PopupBg]           = rgb(34, 35, 39);
    c[ImGuiCol_Border]            = rgb(48, 50, 55);
    c[ImGuiCol_FrameBg]           = rgb(42, 44, 49);
    c[ImGuiCol_FrameBgHovered]    = rgb(52, 55, 61);
    c[ImGuiCol_FrameBgActive]     = rgb(60, 64, 71);
    c[ImGuiCol_TitleBg]           = rgb(22, 23, 26);
    c[ImGuiCol_TitleBgActive]     = rgb(26, 27, 30);
    c[ImGuiCol_MenuBarBg]         = rgb(22, 23, 26);
    c[ImGuiCol_ScrollbarBg]       = rgb(22, 23, 26);
    c[ImGuiCol_CheckMark]         = rgb(64, 224, 192);
    c[ImGuiCol_SliderGrab]        = rgb(64, 224, 192);
    c[ImGuiCol_SliderGrabActive]  = rgb(110, 240, 215);
    c[ImGuiCol_Button]            = rgb(48, 51, 57);
    c[ImGuiCol_ButtonHovered]     = rgb(62, 66, 74);
    c[ImGuiCol_ButtonActive]      = rgb(40, 140, 120);
    c[ImGuiCol_Header]            = rgb(40, 110, 96);
    c[ImGuiCol_HeaderHovered]     = rgb(48, 130, 114);
    c[ImGuiCol_HeaderActive]      = rgb(56, 150, 130);
    c[ImGuiCol_Separator]         = rgb(48, 50, 55);
    c[ImGuiCol_Tab]               = rgb(34, 36, 40);
    c[ImGuiCol_TabHovered]        = rgb(48, 130, 114);
    c[ImGuiCol_TabSelected]       = rgb(40, 110, 96);
    c[ImGuiCol_TabDimmed]         = rgb(28, 29, 32);
    c[ImGuiCol_TabDimmedSelected] = rgb(36, 80, 72);
    c[ImGuiCol_DockingPreview]    = rgb(64, 224, 192, 0.5f);
    c[ImGuiCol_TableHeaderBg]     = rgb(34, 36, 40);
    c[ImGuiCol_TableBorderStrong] = rgb(48, 50, 55);
    c[ImGuiCol_TableBorderLight]  = rgb(40, 42, 46);
    c[ImGuiCol_DragDropTarget]    = rgb(64, 224, 192);
    c[ImGuiCol_NavCursor]         = rgb(64, 224, 192);
}

} // namespace ps
