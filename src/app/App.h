#pragma once
#include "core/Model.h"
#include "gfx/Displays.h"
#include "gfx/Gfx.h"
#include "render/Compositor.h"
#include "render/OutputWindows.h"
#include <deque>
#include <string>
#include <vector>

namespace ps {

struct UIState {
    int      selLayer = 0;
    int      selColumn = -1;
    uint32_t selScreen = 0;
    uint32_t selSlice = 0;
    int      editorTab = 1;          // 0 = Input Selection, 1 = Output Transformation
    float    zoom[2] = {0, 0};       // 0 = ajustar automaticamente
    float    pan[2][2] = {{0, 0}, {0, 0}};
    bool     snap = true;
    bool     showGrid = false;
    int      gridSize = 16;
    bool     showAbout = false;
    bool     showCompSettings = false;
    bool     showLog = false;
    bool     focusEditor = false;
    bool     layoutBuilt = false;
    struct CellRect { int layer, column; float x0, y0, x1, y1; };
    std::vector<CellRect> cells;     // para largar ficheiros do Explorador
};

class App {
public:
    int run(HINSTANCE inst);

    Project      project;
    Compositor   comp;
    OutputWindows outputs;
    std::vector<DisplayInfo> displays;
    UIState      ui;
    std::wstring projectPath;
    bool         dirty = false;
    double       fps = 0;
    double       frameMs = 0;

    // ficheiros
    void newProject();
    bool openProject(const std::wstring& path);
    bool saveProject(const std::wstring& path);
    void saveOrSaveAs();
    void saveAsDialog();
    void openDialog();
    std::vector<std::string> pickVideoFiles();
    void refreshDisplays();

    // undo/redo (instantâneos do projeto)
    void checkpoint();
    void undo();
    void redo();
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    void markDirty() { dirty = true; }

    // ficheiros largados na janela (UTF-8) + ponto em coordenadas cliente
    std::vector<std::string> pendingDrops;
    float dropX = 0, dropY = 0;

    HWND hwnd() const { return m_hwnd; }
    static App* instance();
    void emergencySave();

private:
    bool initWindow(HINSTANCE inst);
    bool initSwapchain();
    void resizeSwapchain();
    void frame();
    void autosave(double now);
    LRESULT handleMessage(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);

    HWND m_hwnd = nullptr;
    ComPtr<IDXGISwapChain1> m_swap;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    bool m_running = true;
    bool m_needResize = false;
    bool m_displaysChanged = false;
    double m_lastAutosave = 0;
    double m_lastFrame = 0;
    std::deque<std::string> m_undo, m_redo;
};

} // namespace ps
