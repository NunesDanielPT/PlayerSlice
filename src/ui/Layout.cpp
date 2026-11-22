// Menu "Layout": mostrar/esconder painéis, monitores extra de programa/preview,
// gravar e carregar disposições completas (docking + painéis + monitores).
#include "ui/UI.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "app/App.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;
using nlohmann::json;

namespace ps {

static fs::path layoutsDir() {
    fs::path d = fs::path(appDataDir()) / L"layouts";
    std::error_code ec; fs::create_directories(d, ec);
    return d;
}

static json uiStateJson(App& app) {
    auto& pn = app.ui.panel;
    json j;
    j["panels"] = {{"clips", pn.clips}, {"monitors", pn.monitors}, {"outputs", pn.outputs}, {"log", pn.log}, {"effects", pn.effects},
                   {"propComp", pn.propComp}, {"propLayer", pn.propLayer}, {"propColumn", pn.propColumn}, {"propClip", pn.propClip},
                   {"clipTime", pn.clipTime}};
    j["clipTime"] = {{"system", app.ui.ctSystem}, {"selClip", app.ui.ctSelClip}, {"selLayer", app.ui.ctSelLayer},
                     {"hidden", std::vector<uint32_t>(app.ui.ctHiddenLayers.begin(), app.ui.ctHiddenLayers.end())}};
    j["monitors"] = json::array();
    for (auto& m : app.ui.extraMonitors)
        j["monitors"].push_back({{"id", m.id}, {"preview", m.preview}, {"mode", m.mode}, {"view", m.viewId}});
    j["nextMonitorId"] = app.ui.nextMonitorId;
    j["main"] = {{"m0", app.ui.viewMode[0]}, {"v0", app.ui.viewId[0]}, {"m1", app.ui.viewMode[1]}, {"v1", app.ui.viewId[1]}, {"split", app.ui.monSplit}};
    return j;
}

static void applyUiState(App& app, const json& j) {
    auto& pn = app.ui.panel;
    if (j.contains("panels")) {
        auto& p = j["panels"];
        pn.clips = p.value("clips", true); pn.monitors = p.value("monitors", true); pn.outputs = p.value("outputs", true);
        pn.log = p.value("log", true); pn.effects = p.value("effects", true);
        pn.propComp = p.value("propComp", true); pn.propLayer = p.value("propLayer", true);
        pn.propColumn = p.value("propColumn", true); pn.propClip = p.value("propClip", true);
        pn.clipTime = p.value("clipTime", true);
    }
    if (j.contains("clipTime")) {
        auto& c = j["clipTime"];
        app.ui.ctSystem = c.value("system", true); app.ui.ctSelClip = c.value("selClip", true); app.ui.ctSelLayer = c.value("selLayer", true);
        app.ui.ctHiddenLayers.clear();
        if (c.contains("hidden") && c["hidden"].is_array()) for (auto& h : c["hidden"]) if (h.is_number()) app.ui.ctHiddenLayers.insert(h.get<uint32_t>());
    }
    app.ui.extraMonitors.clear();
    for (auto& m : j.value("monitors", json::array())) {
        App* a = &app; (void)a;
        UIState::ExtraMonitor em;
        em.id = m.value("id", 0u); em.preview = m.value("preview", false);
        em.mode = m.value("mode", 0); em.viewId = m.value("view", 0u);
        if (em.id) app.ui.extraMonitors.push_back(em);
    }
    app.ui.nextMonitorId = std::max(app.ui.nextMonitorId, j.value("nextMonitorId", 1u));
    for (auto& m : app.ui.extraMonitors) app.ui.nextMonitorId = std::max(app.ui.nextMonitorId, m.id + 1);
    if (j.contains("main")) {
        auto& m = j["main"];
        app.ui.viewMode[0] = m.value("m0", 0); app.ui.viewId[0] = m.value("v0", 0u);
        app.ui.viewMode[1] = m.value("m1", 0); app.ui.viewId[1] = m.value("v1", 0u);
        app.ui.monSplit = std::clamp(m.value("split", 0.5f), 0.15f, 0.85f);
    }
}

void saveUiState(App& app) {
    std::ofstream f(fs::path(appDataDir()) / L"ui.json", std::ios::binary | std::ios::trunc);
    f << uiStateJson(app).dump(2);
}

void loadUiState(App& app) {
    std::ifstream f(fs::path(appDataDir()) / L"ui.json", std::ios::binary);
    if (!f) return;
    try { std::stringstream ss; ss << f.rdbuf(); applyUiState(app, json::parse(ss.str())); } catch (...) {}
}

// chamado no início de um frame (antes de ImGui::NewFrame)
void applyPendingLayout(App& app) {
    if (app.ui.pendingLayout.empty()) return;
    fs::path base = fs::path(fromUtf8(app.ui.pendingLayout));
    app.ui.pendingLayout.clear();
    fs::path ini = base; ini += L".ini";
    fs::path js = base; js += L".json";
    std::ifstream f(js, std::ios::binary);
    if (f) { try { std::stringstream ss; ss << f.rdbuf(); applyUiState(app, json::parse(ss.str())); } catch (...) {} }
    if (fs::exists(ini)) ImGui::LoadIniSettingsFromDisk(toUtf8(ini.wstring()).c_str());
    PS_INFO("Layout carregado: %s", toUtf8(base.filename().wstring()).c_str());
}

static void saveLayout(App& app, const std::string& name) {
    fs::path base = layoutsDir() / fromUtf8(name);
    fs::path ini = base; ini += L".ini";
    fs::path js = base; js += L".json";
    ImGui::SaveIniSettingsToDisk(toUtf8(ini.wstring()).c_str());
    std::ofstream f(js, std::ios::binary | std::ios::trunc);
    f << uiStateJson(app).dump(2);
    PS_INFO("Layout gravado: %s", name.c_str());
}

void drawLayoutMenu(App& app) {
    auto& pn = app.ui.panel;
    static bool askName = false;
    if (ImGui::BeginMenu(T("Layout"))) {
        ImGui::SeparatorText(T("Painéis"));
        ImGui::MenuItem(T("Stack"), nullptr, &pn.clips);
        ImGui::MenuItem(T("Preview / Programa"), nullptr, &pn.monitors);
        ImGui::MenuItem(T("Monitor de saídas"), nullptr, &pn.outputs);
        ImGui::MenuItem(T("Efeitos"), nullptr, &pn.effects);
        ImGui::MenuItem(T("Clip Time"), nullptr, &pn.clipTime);
        ImGui::MenuItem(T("Registo"), nullptr, &pn.log);
        ImGui::SeparatorText(T("Propriedades"));
        ImGui::MenuItem(T("Composição"), nullptr, &pn.propComp);
        ImGui::MenuItem(T("Layer"), nullptr, &pn.propLayer);
        ImGui::MenuItem(T("Coluna"), nullptr, &pn.propColumn);
        ImGui::MenuItem(T("Clip"), nullptr, &pn.propClip);
        ImGui::SeparatorText(T("Monitores extra"));
        if (ImGui::MenuItem(T("+ Monitor de programa"))) app.ui.extraMonitors.push_back({app.ui.nextMonitorId++, false});
        if (ImGui::MenuItem(T("+ Monitor de preview"))) app.ui.extraMonitors.push_back({app.ui.nextMonitorId++, true});
        for (auto& m : app.ui.extraMonitors) {
            std::string l = std::string(m.preview ? T("Preview") : T("Programa")) + " " + std::to_string(m.id) + "##em" + std::to_string(m.id);
            ImGui::MenuItem(l.c_str(), nullptr, &m.open);
        }
        ImGui::SeparatorText(T("Layouts"));
        if (ImGui::MenuItem(T("Gravar layout atual..."))) askName = true;
        std::error_code ec;
        std::vector<fs::path> files;
        for (auto& e : fs::directory_iterator(layoutsDir(), ec)) if (e.path().extension() == L".json") files.push_back(e.path());
        if (!files.empty() && ImGui::BeginMenu(T("Carregar layout"))) {
            for (auto& f : files) {
                std::string nm = toUtf8(f.stem().wstring());
                if (ImGui::MenuItem(nm.c_str())) { fs::path b = f; b.replace_extension(); app.ui.pendingLayout = toUtf8(b.wstring()); }
            }
            ImGui::EndMenu();
        }
        if (!files.empty() && ImGui::BeginMenu(T("Apagar layout"))) {
            for (auto& f : files) {
                std::string nm = toUtf8(f.stem().wstring());
                if (ImGui::MenuItem(nm.c_str())) { fs::path b = f; fs::remove(f, ec); b.replace_extension(L".ini"); fs::remove(b, ec); }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(T("Repor layout de origem"))) {
            pn = UIState::Panels{};
            app.ui.extraMonitors.clear();
            app.ui.layoutBuilt = false;
        }
        ImGui::EndMenu();
    }
    // limpar monitores fechados
    app.ui.extraMonitors.erase(std::remove_if(app.ui.extraMonitors.begin(), app.ui.extraMonitors.end(),
                                              [](const UIState::ExtraMonitor& m) { return !m.open; }), app.ui.extraMonitors.end());
    if (askName) { ImGui::OpenPopup("###savelayout"); askName = false; }
    std::string t = winTitle("Gravar layout", "savelayout");
    if (ImGui::BeginPopupModal(t.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char name[64] = "";
        if (ImGui::IsWindowAppearing()) { name[0] = 0; ImGui::SetKeyboardFocusHere(); }
        ImGui::TextUnformatted(T("Nome do layout:"));
        bool enter = ImGui::InputText("##nm", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue);
        std::string n = name;
        for (char& ch : n) if (strchr("\\/:*?\"<>|", ch)) ch = '_';
        ImGui::BeginDisabled(n.empty());
        if (ImGui::Button(T("Gravar"), ImVec2(120, 0)) || (enter && !n.empty())) { saveLayout(app, n); ImGui::CloseCurrentPopup(); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace ps
