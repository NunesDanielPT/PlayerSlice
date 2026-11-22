#pragma once
#include "core/Model.h"
#include "gfx/Displays.h"
#include <map>
#include <cmath>
#include "gfx/Gfx.h"
#include "render/Compositor.h"
#include "render/OutputWindows.h"
#include <deque>
#include <set>
#include <utility>
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
    bool     showSettings = false;
    int      viewMode[2] = {0, 0};    // monitores preview/programa: 0 composição, 1 ecrã, 2 slice
    uint32_t viewId[2] = {0, 0};
    int      editorBg = 0;           // fundo do editor: 0 programa, 1 preview, 2 nenhum
    float    editorBgAlpha = 0.7f;
    int      settingsTab = 0;
    bool     showCompSettings = false;
    bool     showLog = false;
    bool     focusEditor = false;
    bool     showAdvOutput = false;   // janela "Saída avançada"
    bool     editPoints = false;      // editor: modo Editar pontos
    bool     handTool = false;        // editor: ferramenta mão
    bool     monitorGizmo = true;     // moldura de transformação nos monitores
    float    monSplit = 0.5f;         // divisão preview | programa
    bool     showBigTransform = false;// janela grande de transformação
    bool     bigPrev = true;          // janela grande: preview (true) ou programa
    float    sliceFill = 0.15f;       // editor: opacidade do preenchimento das slices
    uint32_t renameSlice = 0;         // editor: slice a renomear (duplo clique)
    bool     openRenameSlice = false;
    bool     layoutBuilt = false;
    // seleção múltipla de clips (id da layer, coluna) + âncora para Shift+clique
    std::set<std::pair<uint32_t, int>> selCells;
    int      anchorLayer = -1, anchorCol = -1;
    int      selColumnProp = 0;               // coluna mostrada nas propriedades
    struct ClipCopy { int dl, dc; Clip clip; };
    std::vector<ClipCopy> clipboard;          // Ctrl+C / Ctrl+V
    std::vector<Effect> fxClipboard;          // copiar/colar efeitos
    uint32_t renameLayer = 0;                 // layer a ser renomeada (0 = nenhuma)
    int      renameColumn = -1;
    struct CellRect { int layer, column; float x0, y0, x1, y1; };
    // painéis visíveis (menu Layout)
    struct Panels {
        bool clips = true, monitors = true, outputs = true, log = true, effects = true;
        bool propComp = true, propLayer = true, propColumn = true, propClip = true;
        bool clipTime = true;
        bool timecode = false;
    } panel;
    // Clip Time: o que mostrar
    bool ctSystem = true, ctSelClip = true, ctSelLayer = true;
    std::set<uint32_t> ctHiddenLayers;
    // monitores extra (menu Layout → + monitor)
    struct ExtraMonitor { uint32_t id; bool preview; int mode = 0; uint32_t viewId = 0; bool open = true; };
    std::vector<ExtraMonitor> extraMonitors;
    uint32_t nextMonitorId = 1;
    std::string pendingLayout;     // layout a carregar no início do próximo frame
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

    // saídas ligadas (estado em execução, não gravado no projeto)
    std::set<uint32_t> liveScreens;
    bool isLive(uint32_t id) const { return liveScreens.count(id) > 0; }
    bool anyLive() const { return !liveScreens.empty(); }
    void toggleOutputs();          // ecrãs marcados "no atalho" (ou todos se nenhum)
    void setAllOutputs(bool on);
    void setOutput(uint32_t id, bool on);
    void runAction(int action);    // Action (Settings.h)
    void selectStack(int i);
    void localizeDefaultNames();
    void matchOutputRefresh();
    void applyTimecode();          // Settings → módulo Timecode
    void timecodeTriggers();       // disparar clips à hora marcada
    void toggleTestPattern();      // todas as saídas
    bool anyTestPattern() const;     // saídas no ar → frequência do monitor = fps do projeto
    std::map<std::string, float> m_hzApplied;   // monitor → fps já aplicado       // muda a stack visível (os clips no ar continuam)
    void newStack(bool duplicate);
    void applyLanguage();
    void applyPerformance();
    void applyOsc();

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
    void quitForUpdate() { dirty = false; m_running = false; }

private:
    bool initWindow(HINSTANCE inst);
    bool initSwapchain();
    void resizeSwapchain();
    void frame();
    void outputsOnlyFrame();        // só vídeo + saídas (durante diálogos / mover a janela)
    bool m_inUi = false;            // a meio do desenho da interface
    bool m_inTimerFrame = false;
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
    double m_lastUi = 0;
    std::deque<std::string> m_undo, m_redo;
    void handleOsc();
    std::vector<int> m_oscSentColumn;     // feedback: coluna ativa já enviada por layer
    int m_oscSentLive = -1;
};

} // namespace ps
