// Cues: número → coluna de uma stack. Outros programas disparam por OSC (/playerslice/cue/...).
#include "ui/UI.h"
#include "app/App.h"
#include "core/I18n.h"
#include "core/Settings.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ps {

static std::string nextCueNumber(const std::vector<Cue>& q) {
    double mx = 0;
    for (auto& c : q) { char* e = nullptr; double v = strtod(c.number.c_str(), &e); if (e && *e == 0 && !c.number.empty()) mx = std::max(mx, v); }
    char b[32]; snprintf(b, sizeof(b), "%d", (int)std::floor(mx) + 1);
    return b;
}

void drawCuesPanel(App& app) {
    if (!app.ui.panel.cues) return;
    ImGui::SetNextWindowSize(ImVec2(560, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(winTitle("Cues", "cues").c_str(), &app.ui.panel.cues)) { ImGui::End(); return; }
    Project& p = app.project;
    auto& q = p.comp.cues;
    if (app.cueIndex >= (int)q.size()) app.cueIndex = (int)q.size() - 1;

    // GO / anterior / cue atual
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.55f, 0.3f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.65f, 0.36f, 1));
    ImGui::BeginDisabled(q.empty());
    if (ImGui::Button(T("GO"), ImVec2(90, 34))) app.cueGo();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button(T("Anterior"), ImVec2(0, 34))) app.cueBack();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (app.cueIndex >= 0) ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.5f, 1), T("Atual: %s  %s"), q[app.cueIndex].number.c_str(), q[app.cueIndex].name.c_str());
    else ImGui::TextDisabled("%s", T("Nenhum cue disparado"));
    int nx = app.cueIndex + 1;
    if (nx < (int)q.size()) ImGui::TextDisabled(T("Seguinte: %s  %s"), q[nx].number.c_str(), q[nx].name.c_str());
    ImGui::EndGroup();

    if (ImGui::Button(T("+ Cue da coluna selecionada"))) {
        app.checkpoint();
        Cue c;
        c.number = nextCueNumber(q);
        c.stack = p.comp.activeStack;
        c.column = std::clamp(app.ui.selColumnProp >= 0 ? app.ui.selColumnProp : std::max(0, app.ui.selColumn), 0, std::max(0, (int)p.comp.columns.size() - 1));
        if (c.column < (int)p.comp.columns.size()) c.name = p.comp.columns[c.column].name;
        q.push_back(c);
        app.markDirty();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Cria um cue para a coluna escolhida (clica no cabeçalho de uma coluna) na stack visível."));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("OSC: /playerslice/cue/go · /playerslice/cue/N"));

    int remove = -1, moveFrom = -1, moveTo = -1;
    if (ImGui::BeginTable("cues", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(T("Cue"), ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn(T("Nome"));
        ImGui::TableSetupColumn(T("Stack"), ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn(T("Coluna"), ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 46);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 78);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)q.size(); ++i) {
            Cue& c = q[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            if (i == app.cueIndex) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(30, 90, 60, 160));
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-1);
            if (inputTextStd("##num", c.number)) app.markDirty();
            undoOnActivate(app);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            if (inputTextStd("##name", c.name)) app.markDirty();
            undoOnActivate(app);
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);
            const char* sn = c.stack >= 0 && c.stack < (int)p.comp.stacks.size() ? p.comp.stacks[c.stack].name.c_str() : "?";
            if (ImGui::BeginCombo("##stack", sn)) {
                for (int s = 0; s < (int)p.comp.stacks.size(); ++s)
                    if (ImGui::Selectable((p.comp.stacks[s].name + "##s" + std::to_string(s)).c_str(), s == c.stack)) { app.checkpoint(); c.stack = s; app.markDirty(); }
                ImGui::EndCombo();
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(-1);
            const std::vector<Column>& cols = c.stack == p.comp.activeStack ? p.comp.columns
                                            : (c.stack >= 0 && c.stack < (int)p.comp.stacks.size() ? p.comp.stacks[c.stack].columns : p.comp.columns);
            std::string cn = c.column >= 0 && c.column < (int)cols.size() ? cols[c.column].name : std::string("?");
            if (ImGui::BeginCombo("##col", cn.c_str())) {
                for (int k = 0; k < (int)cols.size(); ++k)
                    if (ImGui::Selectable((cols[k].name + "##c" + std::to_string(k)).c_str(), k == c.column)) { app.checkpoint(); c.column = k; app.markDirty(); }
                ImGui::EndCombo();
            }
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton(T("GO##row"))) app.fireCue(i);
            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton("^") && i > 0) { moveFrom = i; moveTo = i - 1; }
            ImGui::SameLine(0, 2);
            if (ImGui::SmallButton("v") && i + 1 < (int)q.size()) { moveFrom = i; moveTo = i + 1; }
            ImGui::SameLine(0, 2);
            if (ImGui::SmallButton("x")) remove = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (q.empty()) ImGui::TextWrapped("%s", T("Sem cues. Clica no cabeçalho de uma coluna e depois em «+ Cue da coluna selecionada». Outros programas podem disparar cada cue por OSC (ativa a entrada OSC em Definições → OSC)."));
    if (remove >= 0) { app.checkpoint(); q.erase(q.begin() + remove); if (app.cueIndex >= remove) app.cueIndex--; app.markDirty(); }
    if (moveFrom >= 0) { app.checkpoint(); std::swap(q[moveFrom], q[moveTo]); if (app.cueIndex == moveFrom) app.cueIndex = moveTo; else if (app.cueIndex == moveTo) app.cueIndex = moveFrom; app.markDirty(); }
    ImGui::End();
}

} // namespace ps
