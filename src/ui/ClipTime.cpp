// Clip Time (como no Resolume): hora do sistema, clip selecionado, layer selecionada
// e cada layer com o tempo que falta, em números grandes. Também serve a página web.
#include "ui/UI.h"
#include "core/I18n.h"
#include "core/Settings.h"
#include "core/WebServer.h"
#include "app/App.h"
#include "video/Thumbnail.h"
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <shellapi.h>
#include <cfloat>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ps {

namespace {

struct CTItem {
    std::string label;
    std::string time;
    double remaining = -1;     // s (−1 = não aplicável)
    float progress = -1;       // 0..1 (−1 = sem barra)
    bool live = false;
};

std::string fmtTime(double s, double fps, bool negative) {
    if (s < 0) s = 0;
    if (fps <= 0) fps = 25;
    long long totalFrames = (long long)std::floor(s * fps + 0.0001);
    int ff = (int)(totalFrames % (long long)std::llround(fps));
    long long secs = (long long)std::floor(s);
    char b[48];
    snprintf(b, sizeof(b), "%s%02lld:%02lld:%02lld.%02d", negative ? "-" : "", secs / 3600, (secs / 60) % 60, secs % 60, ff);
    return b;
}

// tempo do clip que está a tocar numa layer
bool layerTime(App& app, const Layer& l, CTItem& it) {
    VideoPlayer* v = app.comp.activePlayer(l.id);
    const Clip* cl = app.comp.activeClip(l.id);
    if (!v || !cl) return false;
    MediaInfo mi = v->info();
    if (mi.duration <= 0) { it.time = "--:--:--.--"; it.live = true; return true; }
    double pos = std::fmod(std::max(0.0, v->position()), mi.duration);
    float speed = std::max(0.01f, cl->speed);
    it.remaining = (mi.duration - pos) / speed;
    it.time = fmtTime(it.remaining, mi.fps, true);
    it.progress = (float)(pos / mi.duration);
    it.live = true;
    return true;
}

std::vector<CTItem> gather(App& app, bool forWeb) {
    Project& p = app.project;
    std::vector<CTItem> out;
    const int nL = (int)p.comp.layers.size();
    auto layerLabel = [&](int li) {
        const Layer& l = p.comp.layers[li];
        std::string n = l.name;
        if (const Clip* cl = app.comp.activeClip(l.id)) n += " - " + cl->name;
        return n;
    };
    if (app.ui.ctSystem) {
        SYSTEMTIME st; GetLocalTime(&st);
        char b[32]; snprintf(b, sizeof(b), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        out.push_back({T("Hora do sistema"), b});
    }
    int sl = app.ui.selLayer, sc = app.ui.selColumn;
    if (app.ui.ctSelClip && sl >= 0 && sl < nL && sc >= 0 && sc < (int)p.comp.layers[sl].clips.size() && !p.comp.layers[sl].clips[sc].empty()) {
        const Layer& l = p.comp.layers[sl];
        CTItem it;
        it.label = std::string(T("Clip selecionado - ")) + l.name + " - " + l.clips[sc].name;
        if (l.activeColumn == sc) layerTime(app, l, it);
        else {
            auto th = ThumbnailCache::get().request(l.clips[sc].path);
            if (th->state == ThumbnailCache::Entry::State::Ready) it.time = fmtTime(th->info.duration, th->info.fps, false);
            else it.time = "--:--:--.--";
            it.progress = 0;
        }
        out.push_back(it);
    }
    if (app.ui.ctSelLayer && sl >= 0 && sl < nL) {
        CTItem it;
        it.label = std::string(T("Layer selecionada - ")) + layerLabel(sl);
        if (!layerTime(app, p.comp.layers[sl], it)) { it.time = "--:--:--.--"; it.progress = 0; }
        out.push_back(it);
    }
    for (int li = 0; li < nL; ++li) {
        const Layer& l = p.comp.layers[li];
        if (app.ui.ctHiddenLayers.count(l.id)) continue;
        CTItem it;
        it.label = layerLabel(li);
        if (!layerTime(app, l, it)) { it.time = "--:--:--.--"; it.progress = 0; }
        out.push_back(it);
    }
    (void)forWeb;
    return out;
}

void settingsPopup(App& app) {
    ImGui::Checkbox(T("Hora do sistema"), &app.ui.ctSystem);
    ImGui::Checkbox(T("Clip selecionado"), &app.ui.ctSelClip);
    ImGui::Checkbox(T("Layer selecionada"), &app.ui.ctSelLayer);
    ImGui::SeparatorText(T("Layers"));
    for (auto& l : app.project.comp.layers) {
        bool show = !app.ui.ctHiddenLayers.count(l.id);
        std::string lb = l.name + "##ct" + std::to_string(l.id);
        if (ImGui::Checkbox(lb.c_str(), &show)) { if (show) app.ui.ctHiddenLayers.erase(l.id); else app.ui.ctHiddenLayers.insert(l.id); }
    }
    ImGui::SeparatorText(T("Link na rede"));
    Settings& s = settings();
    bool ch = ImGui::Checkbox(T("Partilhar o Clip Time num link"), &s.webEnabled);
    ImGui::SetNextItemWidth(100);
    ch |= ImGui::InputInt(T("Porta"), &s.webPort, 0);
    s.webPort = std::clamp(s.webPort, 1, 65535);
    if (ch) { WebServer::get().configure(s.webEnabled, s.webPort); s.save(); }
    if (s.webEnabled) {
        std::string err = WebServer::get().lastError();
        if (!err.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", err.c_str());
        ImGui::TextDisabled("%s", T("Abre num browser de qualquer aparelho na mesma rede:"));
        for (auto& n : listNetworkAdapters()) {
            if (!n.up || n.ipv4.empty()) continue;
            std::string url = "http://" + n.ipv4 + ":" + std::to_string(s.webPort) + "/";
            ImGui::PushID(url.c_str());
            ImGui::TextColored(ImVec4(0.25f, 0.88f, 0.75f, 1), "%s", url.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Copiar"))) ImGui::SetClipboardText(url.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Abrir"))) ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::PopID();
        }
        ImGui::TextDisabled("%s", T("Na primeira vez o Windows pode pedir para permitir o acesso à rede: aceita."));
    }
}

} // namespace

std::string clipTimeJson(App& app) {
    nlohmann::json j;
    j["project"] = app.project.comp.name;
    j["items"] = nlohmann::json::array();
    for (auto& it : gather(app, true)) {
        nlohmann::json o;
        o["label"] = it.label; o["time"] = it.time; o["live"] = it.live;
        o["remaining_s"] = it.remaining >= 0 ? nlohmann::json(it.remaining) : nlohmann::json(nullptr);
        o["progress"] = it.progress >= 0 ? nlohmann::json(it.progress) : nlohmann::json(nullptr);
        j["items"].push_back(o);
    }
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void drawClipTime(App& app) {
    if (!app.ui.panel.clipTime) return;
    if (!ImGui::Begin(winTitle("Clip Time", "cliptime").c_str(), &app.ui.panel.clipTime)) { ImGui::End(); return; }
    // roda dentada (definições) no canto
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 start = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(start.x + avail.x - 70, start.y));
    if (ImGui::SmallButton(T("Opções"))) ImGui::OpenPopup("ctopts");
    if (WebServer::get().running()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.25f, 0.88f, 0.75f, 1), "%s", "●"); }
    if (ImGui::BeginPopup("ctopts")) { settingsPopup(app); ImGui::EndPopup(); }
    ImGui::SetCursorPos(start);

    auto items = gather(app, false);
    // painel estreito → letra mais pequena; painel largo → nunca maior do que o tamanho normal
    // o tamanho também baixa quando o painel fica baixo (para caber tudo na altura disponível)
    float k = std::clamp((avail.x - 24.f) / 300.f, 0.3f, 1.f);
    {
        const int nItems = std::max(1, (int)gather(app, false).size());
        const float lineH = ImGui::GetTextLineHeight();
        auto fits = [&](float kk) {
            float iw = 300 * kk + 20 * kk;
            int perRow = std::max(1, (int)((avail.x - (kk < 1 ? 4 : 80)) / iw));
            int rows = (nItems + perRow - 1) / perRow;
            float itemH = lineH * std::max(0.75f, kk) + 2 + 40.f * kk * 1.15f + 8;
            float top = kk < 1 ? ImGui::GetFrameHeight() + 2 : 4;
            return top + rows * itemH + (rows - 1) * 14 <= avail.y;
        };
        while (k > 0.3f && !fits(k)) k -= 0.05f;
        k = std::max(k, 0.3f);
    }
    const float itemW = 300 * k, big = 40.f * k;
    float x0 = ImGui::GetCursorScreenPos().x, xMax = x0 + avail.x - (k < 1 ? 4 : 80);
    ImVec2 cur = ImGui::GetCursorScreenPos();
    cur.y += k < 1 ? ImGui::GetFrameHeight() + 2 : 4;   // estreito: não ficar por baixo do botão Opções
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rowH = 0;
    for (auto& it : items) {
        if (cur.x + itemW > xMax && cur.x > x0) { cur.x = x0; cur.y += rowH + 14; }
        ImU32 col = it.live ? (it.remaining >= 0 && it.remaining < 5 ? IM_COL32(255, 80, 80, 255) : it.remaining >= 0 && it.remaining < 15 ? IM_COL32(255, 176, 64, 255) : IM_COL32(184, 240, 96, 255))
                            : IM_COL32(230, 232, 235, 255);
        // etiqueta (cortada à largura)
        dl->PushClipRect(cur, ImVec2(cur.x + itemW - 16, cur.y + 400), true);
        dl->AddText(nullptr, ImGui::GetFontSize() * std::max(0.75f, k), cur, it.live ? col : IM_COL32(200, 202, 206, 255), it.label.c_str());
        dl->PopClipRect();
        float ly = ImGui::GetTextLineHeight() * std::max(0.75f, k) + 2;
        ImFont* f = ImGui::GetFont();
        dl->AddText(f, big, ImVec2(cur.x, cur.y + ly), col, it.time.c_str());
        ImVec2 ts = f->CalcTextSizeA(big, FLT_MAX, 0, it.time.c_str());
        float by = cur.y + ly + ts.y + 4;
        if (it.progress >= 0) {
            dl->AddLine(ImVec2(cur.x, by), ImVec2(cur.x + ts.x, by), IM_COL32(80, 82, 88, 255), 2);
            dl->AddLine(ImVec2(cur.x, by), ImVec2(cur.x + ts.x * std::clamp(it.progress, 0.f, 1.f), by), col, 2);
        }
        rowH = std::max(rowH, by - cur.y + 4);
        cur.x += itemW + 20 * k;
    }
    ImGui::Dummy(ImVec2(avail.x, cur.y + rowH - ImGui::GetCursorScreenPos().y + 10));
    ImGui::End();
}

} // namespace ps
