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
void drawSettings(App& app);
void drawUpdateStatus(App& app, bool compact);
void drawUpdatePopup(App& app);
void compositionSettingsBody(App& app, bool popup);
void drawLayoutMenu(App& app);
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

namespace col {
    constexpr ImU32 Accent     = IM_COL32(64, 224, 192, 255);   // turquesa
    constexpr ImU32 AccentDim  = IM_COL32(40, 140, 120, 255);
    constexpr ImU32 Playing    = IM_COL32(90, 230, 110, 255);
    constexpr ImU32 Warn       = IM_COL32(255, 180, 60, 255);
    constexpr ImU32 Error      = IM_COL32(255, 80, 80, 255);
    constexpr ImU32 TextDim    = IM_COL32(150, 150, 155, 255);
}

} // namespace ps
