// Janela de Definições: Geral · Placa gráfica · Rede · Som · Atalhos · Sobre
#include "ui/UI.h"
#include "app/App.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Updater.h"
#include <imgui.h>
#include <shellapi.h>
#include <algorithm>

namespace ps {

namespace {

std::vector<GpuInfo>   g_gpus;
std::vector<NetInfo>   g_nets;
std::vector<AudioInfo> g_audio;
bool g_listed = false;
int  g_capture = -1;            // ação à espera de uma tecla
std::string g_startGpu;         // GPU em uso (para avisar que é preciso reiniciar)

void refreshLists() {
    g_gpus = listGpus();
    g_nets = listNetworkAdapters();
    g_audio = listAudioOutputs();
    g_listed = true;
}

void save() {
    if (!settings().save()) PS_ERROR("%s", T("Não foi possível gravar as definições"));
}

void sectionGeneral(App& app) {
    Settings& s = settings();
    ImGui::SeparatorText(T("Idioma"));
    int lang = langFromCode(s.language) == Lang::EN ? 1 : 0;
    if (ImGui::RadioButton("Português", lang == 0)) { s.language = "pt"; app.applyLanguage(); save(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("English", lang == 1)) { s.language = "en"; app.applyLanguage(); save(); }

    ImGui::SeparatorText(T("Saídas"));
    ImGui::TextUnformatted(T("Quando uma saída é desligada:"));
    if (ImGui::RadioButton(T("Mostrar preto (recomendado em espetáculo)"), s.outputsOffBlack)) { s.outputsOffBlack = true; save(); }
    if (ImGui::RadioButton(T("Fechar a janela (vê-se o ambiente de trabalho)"), !s.outputsOffBlack)) { s.outputsOffBlack = false; save(); }
    if (ImGui::Checkbox(T("Ligar as saídas automaticamente ao abrir um projeto"), &s.startOutputsLive)) save();
}

void sectionGpu() {
    Settings& s = settings();
    ImGui::SeparatorText(T("Placa gráfica"));
    ImGui::TextWrapped("%s", T("Placa usada para descodificar e desenhar os vídeos. As saídas ligadas a outra placa também funcionam, mas com mais carga."));
    ImGui::Spacing();
    ImGui::Text("%s %s (%zu MB)", T("Em uso:"), toUtf8(gfx().adapterName()).c_str(), gfx().vramMB());
    ImGui::Spacing();
    if (ImGui::RadioButton(T("Automática (a mais potente)"), s.gpu.empty())) { s.gpu.clear(); save(); }
    for (auto& g : g_gpus) {
        if (g.software) continue;
        std::string lbl = g.name + "  (" + std::to_string(g.vramMB) + " MB)##" + g.name;
        if (ImGui::RadioButton(lbl.c_str(), s.gpu == g.name)) { s.gpu = g.name; save(); }
    }
    std::string want = s.gpu.empty() ? g_startGpu : s.gpu;
    if (want != g_startGpu && !s.gpu.empty())
        ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s", T("Reinicia o PLAYERSLICE para mudar de placa."));
}

void sectionNetwork() {
    Settings& s = settings();
    ImGui::SeparatorText(T("Placa de rede"));
    ImGui::TextWrapped("%s", T("Rede usada para controlo e vídeo por rede (OSC, Art-Net, NDI). Estas funções estão em desenvolvimento; a escolha fica já guardada."));
    ImGui::Spacing();
    if (ImGui::RadioButton(T("Todas as placas"), s.network.empty())) { s.network.clear(); save(); }
    for (auto& n : g_nets) {
        std::string lbl = n.name + "  —  " + (n.ipv4.empty() ? std::string(T("sem IP")) : n.ipv4) + (n.up ? "" : std::string("  (") + T("desligada") + ")") + "##" + n.name;
        if (ImGui::RadioButton(lbl.c_str(), s.network == n.name)) { s.network = n.name; save(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", n.description.c_str());
    }
}

void sectionAudio() {
    Settings& s = settings();
    ImGui::SeparatorText(T("Placa de som"));
    ImGui::TextWrapped("%s", T("Saída de áudio dos clips. A reprodução de som está em desenvolvimento; a escolha fica já guardada."));
    ImGui::Spacing();
    if (ImGui::RadioButton(T("Predefinida do Windows"), s.audioDevice.empty())) { s.audioDevice.clear(); s.audioName.clear(); save(); }
    for (auto& a : g_audio) {
        std::string lbl = a.name + (a.isDefault ? std::string("  (") + T("predefinida") + ")" : "") + "##" + a.id;
        if (ImGui::RadioButton(lbl.c_str(), s.audioDevice == a.id)) { s.audioDevice = a.id; s.audioName = a.name; save(); }
    }
}

void sectionShortcuts() {
    Settings& s = settings();
    ImGui::SeparatorText(T("Atalhos de teclado"));
    ImGui::TextDisabled("%s", T("Clica num atalho e carrega na nova tecla. Esc cancela, Backspace apaga."));
    ImGui::Spacing();

    // captura
    if (g_capture >= 0) {
        ImGuiIO& io = ImGui::GetIO();
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            ImGuiKey key = (ImGuiKey)k;
            if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
                key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt || key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper ||
                key == ImGuiKey_ReservedForModCtrl || key == ImGuiKey_ReservedForModShift || key == ImGuiKey_ReservedForModAlt ||
                key == ImGuiKey_ReservedForModSuper || (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY))
                continue;
            if (!ImGui::IsKeyPressed(key, false)) continue;
            bool mods = io.KeyCtrl || io.KeyShift || io.KeyAlt;
            if (key == ImGuiKey_Escape && !mods) { g_capture = -1; break; }
            if (key == ImGuiKey_Backspace && !mods) { s.keys[g_capture] = KeyBinding{}; g_capture = -1; save(); break; }
            KeyBinding b; b.key = key; b.ctrl = io.KeyCtrl; b.shift = io.KeyShift; b.alt = io.KeyAlt;
            s.keys[g_capture] = b;
            g_capture = -1;
            save();
            break;
        }
    }

    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn(T("Ação"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Atalho"), ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)Action::Count; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(T(actionName((Action)i)));
            ImGui::TableSetColumnIndex(1);
            bool dup = false;
            if (s.keys[i].key)
                for (int j = 0; j < (int)Action::Count; ++j) if (j != i && s.keys[j] == s.keys[i]) dup = true;
            std::string lbl = g_capture == i ? std::string(T("carrega numa tecla...")) : bindingLabel(s.keys[i]);
            lbl += "##k" + std::to_string(i);
            if (dup) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1));
            if (ImGui::Button(lbl.c_str(), ImVec2(-1, 0))) g_capture = i;
            if (dup) { ImGui::PopStyleColor(); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Este atalho está repetido")); }
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(T("Repor atalhos de origem"))) { s.resetKeys(); save(); }
}

void sectionUpdates(App& app) {
    Settings& s = settings();
    ImGui::SeparatorText(T("Atualizações"));
    ImGui::Text("%s %s", T("Versão instalada:"), PLAYERSLICE_VERSION);
    if (ImGui::Checkbox(T("Procurar atualizações ao abrir o PLAYERSLICE"), &s.checkUpdates)) save();
    ImGui::Spacing();
    drawUpdateStatus(app, false);
}

void sectionAbout() {
    ImGui::SeparatorText(T("Sobre"));
    ImGui::TextColored(ImVec4(0.25f, 0.88f, 0.75f, 1), "PLAYERSLICE %s", PLAYERSLICE_VERSION);
    ImGui::TextUnformatted(T("Player de vídeo para ecrãs LED fora de formato."));
    ImGui::Spacing();
    ImGui::Text("%s Daniel Nunes", T("Software criado por"));
    if (ImGui::Button("github.com/NunesDanielPT"))
        ShellExecuteW(nullptr, L"open", L"https://github.com/NunesDanielPT", nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::Spacing();
    ImGui::SeparatorText(T("Sistema"));
    ImGui::Text("%s %s (%zu MB)", T("Placa gráfica:"), toUtf8(gfx().adapterName()).c_str(), gfx().vramMB());
    ImGui::Text("%s %s", T("Pasta de dados:"), toUtf8(appDataDir()).c_str());
    if (ImGui::SmallButton(T("Abrir pasta de registos"))) ShellExecuteW(nullptr, L"open", appDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::Spacing();
    ImGui::SeparatorText(T("Licenças"));
    ImGui::TextDisabled("%s", T("Usa FFmpeg (LGPL), Dear ImGui (MIT), HAP (BSD, Vidvox), Snappy (BSD), nlohmann/json (MIT)."));
}

} // namespace

// estado + botões (usado nas Definições e no aviso ao arrancar)
void drawUpdateStatus(App& app, bool compact) {
    Updater& u = Updater::get();
    switch (u.state()) {
    case Updater::State::Idle:
    case Updater::State::UpToDate:
    case Updater::State::Error:
        if (u.state() == Updater::State::UpToDate) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "%s", T("Tens a versão mais recente."));
        if (u.state() == Updater::State::Error) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s %s", T("Não foi possível verificar:"), u.error().c_str());
        if (!compact && ImGui::Button(T("Procurar atualizações"))) u.check(true);
        break;
    case Updater::State::Checking:
        ImGui::TextDisabled("%s", T("A procurar atualizações..."));
        break;
    case Updater::State::Available:
        ImGui::TextColored(ImVec4(0.25f, 0.88f, 0.75f, 1), "%s %s", T("Nova versão disponível:"), u.latestVersion().c_str());
        if (!compact && !u.notes().empty()) {
            ImGui::BeginChild("notes", ImVec2(0, 120), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", u.notes().c_str());
            ImGui::EndChild();
        }
        if (ImGui::Button(T("Atualizar agora"))) u.download();
        break;
    case Updater::State::Downloading:
        ImGui::TextUnformatted(T("A descarregar..."));
        ImGui::ProgressBar(u.progress(), ImVec2(-1, 0));
        break;
    case Updater::State::Ready:
        ImGui::TextUnformatted(T("Pronto a instalar. O PLAYERSLICE vai fechar e voltar a abrir."));
        if (ImGui::Button(T("Instalar e reiniciar"))) {
            if (app.anyLive() && MessageBoxW(app.hwnd(), fromUtf8(T("Há saídas no ar. Instalar mesmo assim?")).c_str(), L"PLAYERSLICE", MB_YESNO | MB_ICONWARNING) != IDYES) break;
            if (app.dirty) app.saveOrSaveAs();
            if (u.install()) app.quitForUpdate();
        }
        break;
    }
}

void drawUpdatePopup(App& app) {
    Updater& u = Updater::get();
    std::string title = winTitle("Atualização disponível", "updpopup");
    if (u.popupPending.exchange(false) && !app.anyLive()) ImGui::OpenPopup(title.c_str()); // nunca interromper com saídas no ar
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("PLAYERSLICE %s → %s", PLAYERSLICE_VERSION, u.latestVersion().c_str());
        ImGui::Spacing();
        drawUpdateStatus(app, true);
        ImGui::Spacing();
        if (ImGui::Button(T("Mais tarde"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void drawSettings(App& app) {
    if (g_startGpu.empty()) g_startGpu = toUtf8(gfx().adapterName());
    if (app.ui.showSettings) {
        ImGui::SetNextWindowSize(ImVec2(820, 560), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    } else {
        g_listed = false;
        g_capture = -1;
        return;
    }
    std::string title = std::string(T("Definições")) + "###settings";
    if (!ImGui::Begin(title.c_str(), &app.ui.showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (!g_listed) refreshLists();

    const char* tabs[] = {"Geral", "Placa gráfica", "Placa de rede", "Placa de som", "Atalhos", "Atualizações", "Sobre"};
    ImGui::BeginChild("nav", ImVec2(180, 0), ImGuiChildFlags_Borders);
    for (int i = 0; i < 7; ++i) {
        std::string l = std::string(T(tabs[i])) + "##tab" + std::to_string(i);
        if (ImGui::Selectable(l.c_str(), app.ui.settingsTab == i, 0, ImVec2(0, 26))) app.ui.settingsTab = i;
    }
    ImGui::Spacing();
    if (ImGui::SmallButton(T("Atualizar listas"))) refreshLists();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_Borders);
    switch (app.ui.settingsTab) {
    case 0: sectionGeneral(app); break;
    case 1: sectionGpu(); break;
    case 2: sectionNetwork(); break;
    case 3: sectionAudio(); break;
    case 4: sectionShortcuts(); break;
    case 5: sectionUpdates(app); break;
    default: sectionAbout(); break;
    }
    ImGui::EndChild();
    ImGui::End();
}

bool shortcutCaptureActive() { return g_capture >= 0; }

} // namespace ps
