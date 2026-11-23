#pragma once
#include <imgui.h>
#include <string>
#include <cstdint>

namespace ps {

class App;
struct Rect;

void setupTheme(float dpiScale);
void drawUI(App& app);
void drawClipGrid(App& app);
void drawSliceEditor(App& app);
void drawProperties(App& app);
void clipTransformControls(App& app, struct Clip& c);
void drawSettings(App& app);
void drawUpdateStatus(App& app, bool compact);
void drawUpdatePopup(App& app);
void compositionSettingsBody(App& app, bool popup);
void drawLayoutMenu(App& app);
void drawClipTime(App& app);
void drawTimecodePanel(App& app);
void drawCuesPanel(App& app);
void sectionTimecode(App& app);
void clipTimecodeControls(App& app, struct Clip& c);
void copyClipSettings(App& app, const struct Clip& c);
int  pasteClipSettings(App& app);     // cola nos clips selecionados; devolve quantos
void columnTimecodeControls(App& app, struct Column& col);
std::string clipTimeJson(App& app);
void applyPendingLayout(App& app);
void saveUiState(App& app);
void loadUiState(App& app);
bool shortcutCaptureActive();
// título traduzido com ID fixo (o layout não se perde ao mudar de idioma)
std::string winTitle(const char* pt, const char* id);

// helpers partilhados
ImTextureID texId(void* srv);
ImU32 sliceColorU32(uint32_t rgba, int alpha);
bool  inputTextStd(const char* label, std::string& s, float width = -1);
// editor de retângulo com campos x,y,w,h (devolve true se mudou)
bool  rectEditor(App& app, const char* id, Rect& r, float maxW, float maxH);
// chamar depois de um widget: grava undo quando a edição começa
void  undoOnActivate(App& app);
// valor numérico estilo Resolume: arrastar para mudar, duplo clique (ou Ctrl+clique) para escrever,
// botão direito para repor o valor por defeito. Barra fina em baixo mostra a posição.
bool  valueSlider(App& app, const char* label, float* v, float lo, float hi, float def, const char* fmt);
// VU (L/R) de -60 a +6 dB; desenha num retângulo (sem ocupar espaço) ou como item (vuItem)
void  vuDraw(ImDrawList* dl, ImVec2 p0, ImVec2 size, float l, float r, bool vertical);
void  vuItem(float l, float r, float width, float height = 10.f);
// volume em dB (−60 = mudo … +12) sobre um ganho linear
bool  volumeSlider(App& app, const char* label, float* gain);

namespace col {
    constexpr ImU32 Accent     = IM_COL32(64, 224, 192, 255);   // turquesa
    constexpr ImU32 AccentDim  = IM_COL32(40, 140, 120, 255);
    constexpr ImU32 Playing    = IM_COL32(90, 230, 110, 255);
    constexpr ImU32 Warn       = IM_COL32(255, 180, 60, 255);
    constexpr ImU32 Error      = IM_COL32(255, 80, 80, 255);
    constexpr ImU32 TextDim    = IM_COL32(150, 150, 155, 255);
}

} // namespace ps
