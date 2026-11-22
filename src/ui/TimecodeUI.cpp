// Timecode SMPTE: definições, painel (menu Layout) e propriedades do clip
#include "ui/UI.h"
#include "app/App.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Timecode.h"
#include "video/Thumbnail.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdio>

namespace ps {

namespace {
// mensagens do módulo Timecode (traduzidas quando mostradas)
[[maybe_unused]] void i18nKeys() {
    (void)T("Captura de áudio indisponível"); (void)T("Formato de áudio desconhecido");
    (void)T("Não foi possível abrir a entrada MIDI (está a ser usada por outro programa?)");
    (void)T("Não foi possível abrir a entrada de áudio"); (void)T("Não foi possível iniciar a captura de áudio");
    (void)T("Sem entrada de áudio"); (void)T("Áudio indisponível");
    (void)T("Entrada MIDI não encontrada"); (void)T("Não há entradas MIDI"); (void)T("Gerador interno");
}
std::vector<Timecode::Device> g_tcAudio;
std::vector<std::string> g_tcMidi;
bool g_tcListed = false;

void listDevices() {
    g_tcAudio = Timecode::audioInputs();
    g_tcMidi = Timecode::midiInputs();
    g_tcListed = true;
}

const float kFpsVals[] = {0, 24, 25, 29.97f, 30};
const char* sourceName(int s) {
    switch (s) {
    case 1: return T("LTC (entrada de áudio)");
    case 2: return T("MTC (MIDI)");
    case 3: return T("Gerador interno (para testar)");
    default: return T("Desligado");
    }
}

// campo HH:MM:SS:FF editável
bool tcField(const char* label, double& sec, float fps, float width = 130) {
    ImGui::PushID(label);
    static char buf[32];
    static ImGuiID editing = 0;
    ImGuiID id = ImGui::GetID("##tc");
    if (editing != id) snprintf(buf, sizeof(buf), "%s", tcFormat(sec, fps).c_str());
    ImGui::SetNextItemWidth(width);
    bool ch = false;
    if (ImGui::InputText("##tc", buf, sizeof(buf), ImGuiInputTextFlags_CharsNoBlank)) {}
    if (ImGui::IsItemActivated()) editing = id;
    if (ImGui::IsItemDeactivated()) {
        double v;
        if (tcParse(buf, fps, v)) { sec = v; ch = true; }
        editing = 0;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("HH:MM:SS:FF (horas:minutos:segundos:frames). Enter para confirmar."));
    ImGui::SameLine(0, 6);
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return ch;
}

ImVec4 statusColor(const TcStatus& st) {
    if (!st.valid) return ImVec4(0.55f, 0.56f, 0.6f, 1);
    if (st.running) return ImVec4(0.35f, 0.9f, 0.5f, 1);
    if (st.signal) return ImVec4(0.95f, 0.75f, 0.25f, 1);
    return ImVec4(0.95f, 0.4f, 0.35f, 1);
}
const char* statusText(const TcStatus& st, int source) {
    if (source == 0) return T("Desligado");
    if (!st.error.empty() && !st.valid) return T("Erro");
    if (!st.valid) return T("À espera de sinal");
    if (st.running) return T("A correr");
    if (st.signal) return T("Parado");
    return T("Sem sinal (a segurar o último)");
}

void levelMeter(float level, float w) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = 8;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(40, 42, 48, 255), 2);
    float db = level > 1e-5f ? 20.f * std::log10(level) : -100.f;
    float t = std::clamp((db + 60.f) / 60.f, 0.f, 1.f);
    ImU32 c = db > -3 ? IM_COL32(230, 80, 70, 255) : db > -30 ? IM_COL32(64, 224, 150, 255) : IM_COL32(200, 170, 60, 255);
    dl->AddRectFilled(p, ImVec2(p.x + w * t, p.y + h), c, 2);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::SameLine();
    if (level <= 1e-5f) ImGui::TextDisabled("%s", T("sem áudio"));
    else ImGui::TextDisabled("%.0f dB", db);
}
} // namespace

// --------------------------------------------------------------------------- Definições → Timecode
void sectionTimecode(App& app) {
    Settings& s = settings();
    if (!g_tcListed) listDevices();
    bool ch = false;
    ImGui::TextWrapped("%s", T("Recebe timecode SMPTE de uma mesa de luz, de som ou de um sistema de playback, como no Resolume. Os clips podem seguir o timecode (a posição do vídeo vem do relógio) ou disparar a uma hora marcada. Configura cada clip no painel Clip → Timecode."));
    ImGui::SeparatorText(T("Entrada"));
    ImGui::SetNextItemWidth(300);
    if (ImGui::BeginCombo(T("Fonte"), sourceName(s.tcSource))) {
        for (int i = 0; i < 4; ++i) if (ImGui::Selectable(sourceName(i), s.tcSource == i)) { s.tcSource = i; ch = true; }
        ImGui::EndCombo();
    }
    if (s.tcSource == 1) {
        std::string cur = s.tcAudioId.empty() ? std::string(T("Predefinido do Windows")) : (s.tcAudioName.empty() ? s.tcAudioId : s.tcAudioName);
        ImGui::SetNextItemWidth(300);
        if (ImGui::BeginCombo(T("Entrada de áudio"), cur.c_str())) {
            if (ImGui::Selectable(T("Predefinido do Windows"), s.tcAudioId.empty())) { s.tcAudioId.clear(); s.tcAudioName.clear(); ch = true; }
            for (auto& d : g_tcAudio)
                if (ImGui::Selectable((d.name + "##" + d.id).c_str(), d.id == s.tcAudioId)) { s.tcAudioId = d.id; s.tcAudioName = d.name; ch = true; }
            ImGui::EndCombo();
        }
        const char* chans[] = {T("Esquerdo (1)"), T("Direito (2)")};
        ImGui::SetNextItemWidth(300);
        if (ImGui::Combo(T("Canal"), &s.tcChannel, chans, 2)) ch = true;
        ImGui::TextDisabled("%s", T("Liga o cabo de LTC à entrada de linha da placa de som (não à de microfone, se possível)."));
    } else if (s.tcSource == 2) {
        std::string cur = s.tcMidi.empty() ? std::string(T("Primeira disponível")) : s.tcMidi;
        ImGui::SetNextItemWidth(300);
        if (ImGui::BeginCombo(T("Entrada MIDI"), cur.c_str())) {
            if (ImGui::Selectable(T("Primeira disponível"), s.tcMidi.empty())) { s.tcMidi.clear(); ch = true; }
            for (auto& m : g_tcMidi) if (ImGui::Selectable(m.c_str(), m == s.tcMidi)) { s.tcMidi = m; ch = true; }
            ImGui::EndCombo();
        }
        if (g_tcMidi.empty()) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1), "%s", T("Não foram encontradas entradas MIDI."));
    } else if (s.tcSource == 3) {
        ImGui::TextDisabled("%s", T("Relógio interno com Play/Pausa no painel Timecode — serve para preparar o espetáculo sem a fonte real."));
    }
    if (ImGui::SmallButton(T("Procurar dispositivos"))) listDevices();

    ImGui::SeparatorText(T("Formato"));
    const char* fpsNames[] = {T("Automático (detetar)"), "24", "25", "29.97 (drop frame)", "30"};
    int fi = 0;
    for (int i = 0; i < 5; ++i) if (std::fabs(kFpsVals[i] - s.tcFps) < 0.01f) fi = i;
    ImGui::SetNextItemWidth(300);
    if (ImGui::Combo(T("Frame rate do timecode"), &fi, fpsNames, 5)) { s.tcFps = kFpsVals[fi]; ch = true; }
    float fpsShown = s.tcFps > 0 ? s.tcFps : 25;
    double off = std::fabs(s.tcOffset);
    bool neg = s.tcOffset < 0;
    if (ImGui::Checkbox(T("Negativo##tcoffneg"), &neg)) { s.tcOffset = neg ? -off : off; ch = true; }
    ImGui::SameLine();
    if (tcField(T("Offset (somado ao timecode recebido)"), off, fpsShown, 130)) { s.tcOffset = neg ? -off : off; ch = true; }
    ImGui::SetNextItemWidth(300);
    if (ImGui::SliderInt(T("Freewheel (frames)"), &s.tcFreewheel, 0, 50)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Se o sinal falhar, o relógio continua a contar durante estes frames antes de parar (evita paragens por pequenas falhas no cabo)."));

    ImGui::SeparatorText(T("Estado"));
    TcStatus st = Timecode::get().status();
    ImGui::TextColored(statusColor(st), "● %s", statusText(st, s.tcSource));
    ImGui::SameLine();
    ImGui::Text("  %s  ·  %.2f fps", tcFormat(st.seconds, st.fps, st.drop).c_str(), st.fps);
    if (!st.device.empty()) ImGui::TextDisabled("%s", st.device.c_str());
    if (!st.error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.35f, 1), "%s", T(st.error.c_str()));
    if (s.tcSource == 1) levelMeter(st.level, 300);
    if (ch) { app.applyTimecode(); if (!settings().save()) PS_ERROR("%s", T("Não foi possível gravar as definições")); }
}

// --------------------------------------------------------------------------- painel Timecode
void drawTimecodePanel(App& app) {
    if (!app.ui.panel.timecode) return;
    ImGui::SetNextWindowSize(ImVec2(420, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(winTitle("Timecode", "timecode").c_str(), &app.ui.panel.timecode)) { ImGui::End(); return; }
    Settings& s = settings();
    TcStatus st = Timecode::get().status();
    ImGui::TextColored(statusColor(st), "●");
    ImGui::SameLine();
    ImGui::TextUnformatted(statusText(st, s.tcSource));
    ImGui::SameLine();
    ImGui::TextDisabled("·  %s", sourceName(s.tcSource));
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 90);
    if (ImGui::SmallButton(T("Definições..."))) { app.ui.showSettings = true; app.ui.settingsTab = 5; }

    // relógio grande
    std::string tc = tcFormat(st.seconds, st.fps, st.drop);
    float avail = ImGui::GetContentRegionAvail().x;
    ImFont* f = ImGui::GetFont();
    float base = ImGui::CalcTextSize("00:00:00:00").x;
    float scale = std::clamp(avail * 0.9f / std::max(1.f, base), 1.f, 6.f);
    ImGui::SetWindowFontScale(scale);
    ImVec4 col = s.tcSource == 0 ? ImVec4(0.45f, 0.46f, 0.5f, 1) : st.running ? ImVec4(0.92f, 0.95f, 0.98f, 1) : ImVec4(0.7f, 0.72f, 0.76f, 1);
    ImGui::TextColored(col, "%s", tc.c_str());
    ImGui::SetWindowFontScale(1.f);
    (void)f;
    ImGui::TextDisabled("%.2f fps%s", st.fps, st.drop ? " DF" : "");
    if (!st.device.empty()) { ImGui::SameLine(); ImGui::TextDisabled("·  %s", st.device.c_str()); }
    if (!st.error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.35f, 1), "%s", T(st.error.c_str()));
    if (s.tcSource == 1) levelMeter(st.level, std::min(300.f, avail * 0.6f));

    if (s.tcSource == 0) {
        ImGui::TextWrapped("%s", T("O timecode está desligado. Escolhe a fonte (LTC, MTC ou gerador interno) em Definições → Timecode."));
    }
    if (s.tcSource == 3) {
        bool play = Timecode::get().genPlaying();
        if (ImGui::Button(play ? T("Pausa") : T("Play"), ImVec2(80, 0))) Timecode::get().genPlay(!play);
        ImGui::SameLine();
        if (ImGui::Button(T("Ao início"))) Timecode::get().genSet(0);
        ImGui::SameLine();
        static double jump = 0;
        tcField(T("Ir para"), jump, st.fps, 120);
        ImGui::SameLine();
        if (ImGui::Button(T("Ir"))) Timecode::get().genSet(jump - s.tcOffset);
    }

    // clips com timecode na stack visível
    ImGui::SeparatorText(T("Clips com timecode (stack visível)"));
    Project& p = app.project;
    int n = 0;
    if (ImGui::BeginTable("tcclips", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn(T("Início"), ImGuiTableColumnFlags_WidthFixed, 95);
        ImGui::TableSetupColumn(T("Layer"));
        ImGui::TableSetupColumn(T("Clip"));
        ImGui::TableSetupColumn(T("Modo"), ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableHeadersRow();
        struct Row { double t; int li, c; };
        std::vector<Row> rows;
        for (int li = 0; li < (int)p.comp.layers.size(); ++li)
            for (int c = 0; c < (int)p.comp.layers[li].clips.size(); ++c) {
                const Clip& cl = p.comp.layers[li].clips[c];
                if (!cl.empty() && cl.tcMode != TcMode::Off) rows.push_back({cl.tcStart, li, c});
            }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.t < b.t; });
        const char* modes[] = {"", T("Seguir"), T("Disparar e seguir"), T("Só disparar")};
        for (auto& r : rows) {
            const Layer& l = p.comp.layers[r.li];
            const Clip& cl = l.clips[r.c];
            bool live = l.activeColumn == r.c;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImVec4 tcCol = live ? ImVec4(0.35f, 0.9f, 0.5f, 1) : (st.valid && st.seconds < r.t) ? ImVec4(0.85f, 0.87f, 0.9f, 1) : ImVec4(0.55f, 0.56f, 0.6f, 1);
            ImGui::TextColored(tcCol, "%s", tcFormat(r.t, st.fps).c_str());
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(l.name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(n);
            if (ImGui::Selectable(cl.name.c_str(), app.ui.selLayer == r.li && app.ui.selColumn == r.c)) { app.ui.selLayer = r.li; app.ui.selColumn = r.c; }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(3); ImGui::TextDisabled("%s", modes[(int)cl.tcMode]);
            n++;
        }
        ImGui::EndTable();
    }
    if (!n) ImGui::TextDisabled("%s", T("Nenhum clip com timecode. Seleciona um clip e, no painel Clip, escolhe o modo de timecode."));
    ImGui::End();
}

// --------------------------------------------------------------------------- Clip → Timecode
void clipTimecodeControls(App& app, Clip& c) {
    TcStatus st = Timecode::get().status();
    float fps = st.fps > 0 ? st.fps : 25;
    const char* modes[] = {T("Desligado"), T("Seguir o timecode"), T("Disparar e seguir"), T("Só disparar")};
    int m = (int)c.tcMode;
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
    if (ImGui::Combo(T("Modo"), &m, modes, 4)) { app.checkpoint(); c.tcMode = (TcMode)m; app.markDirty(); }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Seguir: com o clip no ar, a posição do vídeo vem do timecode (salta, pára e anda com ele).\nDisparar e seguir: entra no ar sozinho à hora de início e depois segue o timecode.\nSó disparar: entra no ar à hora de início e toca normalmente."));
    if (c.tcMode == TcMode::Off) {
        ImGui::TextDisabled("%s", T("O clip toca pelo seu próprio relógio."));
        return;
    }
    double start = c.tcStart;
    if (tcField(T("Início"), start, fps, 130)) { app.checkpoint(); c.tcStart = start; app.markDirty(); }
    ImGui::SameLine();
    ImGui::BeginDisabled(!st.valid);
    if (ImGui::SmallButton(T("Agora"))) { app.checkpoint(); c.tcStart = std::floor(st.seconds * std::lround(fps)) / std::lround(fps); app.markDirty(); }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", T("Usa o timecode atual como início."));
    auto th = ThumbnailCache::get().request(c.path);
    if (th->state == ThumbnailCache::Entry::State::Ready && th->info.duration > 0) {
        double dur = th->info.duration / std::max(0.01f, c.speed);
        ImGui::TextDisabled(T("Fim: %s  (duração %s)"), tcFormat(c.tcStart + dur, fps).c_str(), tcFormat(dur, fps).c_str());
    }
    if (st.valid) {
        double rel = st.seconds - c.tcStart;
        if (rel < 0) ImGui::TextColored(ImVec4(0.85f, 0.87f, 0.9f, 1), T("Começa daqui a %s"), tcFormat(-rel, fps).c_str());
        else ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.5f, 1), T("Posição no clip: %s"), tcFormat(rel * c.speed, fps).c_str());
    } else ImGui::TextDisabled("%s", T("Sem timecode (Definições → Timecode)."));
    if (tcTriggers(c.tcMode)) ImGui::TextDisabled("%s", T("Dispara só quando este clip está na stack visível."));
}

} // namespace ps
