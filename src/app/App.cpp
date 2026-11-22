#include "app/App.h"
#include "core/Log.h"
#include "core/CrashHandler.h"
#include "core/I18n.h"
#include "core/Settings.h"
#include "core/Updater.h"
#include "core/Osc.h"
#include "core/WebServer.h"
#include "ui/UI.h"
#include "video/VideoPlayer.h"
#include "video/Thumbnail.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <shellapi.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <fstream>
#include <filesystem>
#include <mmsystem.h>
#include <sstream>
#include <atomic>
#include <mutex>
#include <thread>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ps {

static App* g_app = nullptr;

// ---------------------------------------------------------------------------
// impedir o rato de entrar nos ecrãs de saída (hook de rato de baixo nível numa
// thread própria, para o rato nunca ficar lento enquanto a interface desenha)
// ---------------------------------------------------------------------------
static std::mutex g_blockMtx;
static std::vector<RECT> g_blockRects;      // protegido por g_blockMtx
static std::vector<RECT> g_blockApplied;    // thread principal
static HHOOK g_mouseHook = nullptr;
static std::thread g_hookThread;
static DWORD g_hookThreadId = 0;

static LRESULT CALLBACK mouseHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEMOVE) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lp;
        std::lock_guard<std::mutex> lk(g_blockMtx);
        if (!g_blockRects.empty()) {
            POINT cur; GetCursorPos(&cur);
            bool curInside = false;
            for (auto& r : g_blockRects) if (PtInRect(&r, cur)) curInside = true;
            if (!curInside)   // se já estiver lá dentro, deixa sair
                for (auto& r : g_blockRects) if (PtInRect(&r, m->pt)) return 1;
        }
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

static void stopMouseHook() {
    if (!g_hookThread.joinable()) return;
    PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    g_hookThread.join();
    g_hookThreadId = 0;
}

static void startMouseHook() {
    if (g_hookThread.joinable()) return;
    std::atomic<bool> ready{false};
    g_hookThread = std::thread([&ready] {
        g_hookThreadId = GetCurrentThreadId();
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);   // criar a fila de mensagens
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseHookProc, GetModuleHandleW(nullptr), 0);
        ready = true;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
        if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    });
    while (!ready.load()) Sleep(1);
}

static void updateMouseLock(const std::vector<RECT>& rects, HWND mainWnd) {
    // nunca bloquear o monitor da interface (senão o rato ficava preso)
    HMONITOR mainMon = MonitorFromWindow(mainWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mainMon, &mi);
    std::vector<RECT> b;
    for (auto& r : rects) { RECT x; if (!IntersectRect(&x, &r, &mi.rcMonitor)) b.push_back(r); }
    bool changed = b.size() != g_blockApplied.size();
    for (size_t i = 0; !changed && i < b.size(); ++i) changed = memcmp(&b[i], &g_blockApplied[i], sizeof(RECT)) != 0;
    if (!changed) return;
    g_blockApplied = b;
    { std::lock_guard<std::mutex> lk(g_blockMtx); g_blockRects = b; }
    if (!b.empty()) {
        startMouseHook();
        POINT cur; GetCursorPos(&cur);   // tirar o rato de dentro de uma saída
        for (auto& r : b) if (PtInRect(&r, cur)) SetCursorPos((mi.rcWork.left + mi.rcWork.right) / 2, (mi.rcWork.top + mi.rcWork.bottom) / 2);
    } else {
        stopMouseHook();
    }
}

static std::wstring autosavePath() { return appDataDir() + L"\\autosave.psproj"; }

// ---------------------------------------------------------------------------
// ficheiros
// ---------------------------------------------------------------------------
static bool readFile(const std::wstring& path, std::string& out) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool writeFileAtomic(const std::wstring& path, const std::string& data) {
    // escrever para .tmp e substituir → nunca fica um projeto meio gravado
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void App::newProject() {
    comp.stopAll(project);
    comp.clearPreview();
    liveScreens.clear();
    project = Project::makeDefault();
    projectPath.clear();
    dirty = false;
    m_undo.clear(); m_redo.clear();
    ui.selScreen = project.screens.empty() ? 0 : project.screens[0].id;
    ui.selSlice = 0;
}

bool App::openProject(const std::wstring& path) {
    std::string text, err;
    if (!readFile(path, text)) { PS_ERROR("Não foi possível ler %s", toUtf8(path).c_str()); return false; }
    Project p;
    if (!p.fromJson(text, &err)) { PS_ERROR("Projeto inválido: %s", err.c_str()); return false; }
    comp.stopAll(project);
    comp.clearPreview();
    liveScreens.clear();
    project = std::move(p);
    projectPath = path;
    if (settings().startOutputsLive) setAllOutputs(true);
    dirty = false;
    m_undo.clear(); m_redo.clear();
    ui.selScreen = project.screens.empty() ? 0 : project.screens[0].id;
    ui.selSlice = 0;
    PS_INFO("Projeto aberto: %s", toUtf8(path).c_str());
    return true;
}

bool App::saveProject(const std::wstring& path) {
    if (!writeFileAtomic(path, project.toJson())) { PS_ERROR("Erro a gravar %s", toUtf8(path).c_str()); return false; }
    projectPath = path;
    dirty = false;
    PS_INFO("Projeto gravado: %s", toUtf8(path).c_str());
    return true;
}

void App::saveOrSaveAs() {
    if (projectPath.empty()) saveAsDialog();
    else saveProject(projectPath);
}

void App::saveAsDialog() {
    wchar_t file[MAX_PATH] = L"projeto.psproj";
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = m_hwnd;
    std::wstring filter = fromUtf8(T("Projeto PLAYERSLICE")) + L" (*.psproj)";
    filter.push_back(0); filter += L"*.psproj"; filter.push_back(0); filter.push_back(0);
    of.lpstrFilter = filter.c_str();
    of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = L"psproj";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&of)) saveProject(file);
}

void App::openDialog() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = m_hwnd;
    std::wstring filter = fromUtf8(T("Projeto PLAYERSLICE")) + L" (*.psproj)";
    filter.push_back(0); filter += L"*.psproj"; filter.push_back(0); filter.push_back(0);
    of.lpstrFilter = filter.c_str();
    of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&of)) openProject(file);
}

std::vector<std::string> App::pickVideoFiles() {
    std::vector<wchar_t> buf(32768, 0);
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = m_hwnd;
    std::wstring filter = fromUtf8(T("Vídeo"));
    filter.push_back(0); filter += L"*.mov;*.mp4;*.mkv;*.avi;*.mxf;*.m4v;*.webm;*.ts"; filter.push_back(0);
    filter += fromUtf8(T("Todos")); filter.push_back(0); filter += L"*.*"; filter.push_back(0); filter.push_back(0);
    of.lpstrFilter = filter.c_str();
    of.lpstrFile = buf.data(); of.nMaxFile = (DWORD)buf.size();
    of.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;
    std::vector<std::string> out;
    if (!GetOpenFileNameW(&of)) return out;
    std::wstring dir = buf.data();
    const wchar_t* p = buf.data() + dir.size() + 1;
    if (!*p) { out.push_back(toUtf8(dir)); return out; }
    while (*p) { std::wstring name = p; out.push_back(toUtf8(dir + L"\\" + name)); p += name.size() + 1; }
    return out;
}

void App::refreshDisplays() {
    displays = enumerateDisplays();
    PS_INFO("%d saídas de vídeo detetadas", (int)displays.size());
    for (auto& d : displays) PS_INFO("  %s — %s", d.label().c_str(), d.adapter.c_str());
}

// ---------------------------------------------------------------------------
// saídas ligadas / desligadas
// ---------------------------------------------------------------------------
void App::setOutput(uint32_t id, bool on) {
    if (on) liveScreens.insert(id); else liveScreens.erase(id);
}

void App::setAllOutputs(bool on) {
    liveScreens.clear();
    if (on) for (auto& s : project.screens) if (s.device != ScreenDevice::Disabled) liveScreens.insert(s.id);
    PS_INFO("%s", on ? "Saídas ligadas (todas)" : "Saídas desligadas (todas)");
}

void App::toggleOutputs() {
    std::vector<uint32_t> targets;
    for (auto& s : project.screens) if (s.device != ScreenDevice::Disabled && s.inToggle) targets.push_back(s.id);
    if (targets.empty()) for (auto& s : project.screens) if (s.device != ScreenDevice::Disabled) targets.push_back(s.id);
    if (targets.empty()) { PS_WARN("Nenhum ecrã tem saída configurada"); return; }
    bool anyOn = false;
    for (auto id : targets) if (isLive(id)) anyOn = true;
    for (auto id : targets) setOutput(id, !anyOn);
    PS_INFO("Saídas %s (%d)", anyOn ? "desligadas" : "ligadas", (int)targets.size());
}

void App::runAction(int a) {
    switch ((Action)a) {
    case Action::ToggleOutputs: toggleOutputs(); break;
    case Action::OutputsOn:     setAllOutputs(true); break;
    case Action::OutputsOff:    setAllOutputs(false); break;
    case Action::StopAll:       comp.stopAll(project); break;
    case Action::TestPattern:
        if (Screen* s = project.findScreen(ui.selScreen)) { s->testPattern = !s->testPattern; dirty = true; }
        break;
    case Action::Save:          saveOrSaveAs(); break;
    case Action::SaveAs:        saveAsDialog(); break;
    case Action::Open:          openDialog(); break;
    case Action::Undo:          undo(); break;
    case Action::Redo:          redo(); break;
    case Action::Settings:      ui.showSettings = true; break;
    case Action::OutputEditor:  ui.focusEditor = true; break;
    case Action::TakePreview:   comp.takePreview(project); break;
    case Action::ClearPreview:  comp.clearPreview(); break;
    default:
        if (a >= (int)Action::Column1 && a <= (int)Action::Column9) comp.triggerColumn(project, a - (int)Action::Column1);
        break;
    }
}

void App::applyPerformance() {
    const Settings& s = settings();
    DWORD cls = s.priority >= 2 ? HIGH_PRIORITY_CLASS : s.priority == 1 ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
    SetPriorityClass(GetCurrentProcess(), cls);
    SetThreadExecutionState(s.preventSleep ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS);
}

void App::applyOsc() {
    const Settings& s = settings();
    Osc::get().configure(s.oscIn, s.oscInPort, s.oscOut, s.oscOutAddr, s.oscOutPort);
    m_oscSentColumn.clear();
    m_oscSentLive = -1;
}

// Endereços compatíveis com o Resolume (1 = layer de baixo, colunas a partir de 1)
void App::handleOsc() {
    auto msgs = Osc::get().poll();
    Project& p = project;
    const int nL = (int)p.comp.layers.size();
    auto layerIdx = [&](int n) { return (n >= 1 && n <= nL) ? nL - n : -1; };
    for (auto& m : msgs) {
        std::vector<std::string> a;
        size_t i = 1;
        while (i <= m.address.size()) {
            size_t j = m.address.find('/', i);
            if (j == std::string::npos) j = m.address.size();
            a.push_back(m.address.substr(i, j - i));
            i = j + 1;
        }
        auto num = [&](size_t k) { return k < a.size() ? atoi(a[k].c_str()) : 0; };
        const float v = m.value(1.f);
        const bool press = v > 0.5f;
        if (a.size() >= 2 && a[0] == "composition") {
            if (a[1] == "disconnectall") { if (press) comp.stopAll(p); }
            else if (a[1] == "columns" && a.size() >= 4) {
                int c = num(2) - 1;
                if (c >= 0 && c < (int)p.comp.columns.size()) {
                    if (a[3] == "connect" && press) comp.triggerColumn(p, c);
                    else if (a[3] == "select" && press) ui.selColumnProp = c;
                }
            } else if (a[1] == "layers" && a.size() >= 4) {
                int li = layerIdx(num(2));
                if (li < 0) continue;
                Layer& l = p.comp.layers[li];
                if (a[3] == "clear" && press) comp.stopLayer(p, li);
                else if (a[3] == "clips" && a.size() >= 6) {
                    int c = num(4) - 1;
                    if (c < 0 || c >= (int)l.clips.size()) continue;
                    if (a[5] == "connect" && press) comp.trigger(p, li, c);
                    else if (a[5] == "select" && press) comp.previewClip(p, li, c);
                }
                else if (a[3] == "video" && a.size() >= 5 && a[4] == "opacity") l.opacity = std::clamp(v, 0.f, 1.f);
                else if (a[3] == "bypassed") l.bypass = press;
                else if (a[3] == "solo") l.solo = press;
            } else if (a[1] == "video" && a.size() >= 3 && a[2] == "opacity") p.comp.master = std::clamp(v, 0.f, 1.f);
        } else if (a.size() >= 2 && a[0] == "playerslice") {
            if (a[1] == "outputs" && a.size() >= 3) {
                if (a[2] == "toggle" && press) toggleOutputs();
                else if (a[2] == "on" && press) setAllOutputs(true);
                else if (a[2] == "off" && press) setAllOutputs(false);
            } else if (a[1] == "preview" && a.size() >= 3) {
                if (a[2] == "take" && press) comp.takePreview(p);
                else if (a[2] == "clear" && press) comp.clearPreview();
            } else if (a[1] == "screens" && a.size() >= 4 && a[3] == "output") {
                int si = num(2) - 1;
                if (si >= 0 && si < (int)p.screens.size()) setOutput(p.screens[si].id, press);
            } else if (a[1] == "testpattern" && press) {
                runAction((int)Action::TestPattern);
            } else if (a[1] == "stopall" && press) comp.stopAll(p);
        }
    }

    // feedback para o Companion (só quando muda)
    if (!settings().oscOut) return;
    if ((int)m_oscSentColumn.size() != nL) m_oscSentColumn.assign(nL, -2);
    for (int li = 0; li < nL; ++li) {
        int cur = p.comp.layers[li].activeColumn;
        int& sent = m_oscSentColumn[li];
        if (cur == sent) continue;
        std::string base = "/composition/layers/" + std::to_string(nL - li) + "/clips/";
        if (sent >= 0) Osc::get().send(base + std::to_string(sent + 1) + "/connected", 0);
        if (cur >= 0) Osc::get().send(base + std::to_string(cur + 1) + "/connected", 1);
        sent = cur;
    }
    int live = anyLive() ? 1 : 0;
    if (live != m_oscSentLive) { Osc::get().send("/playerslice/outputs/live", live); m_oscSentLive = live; }
}

void App::applyLanguage() {
    setLanguage(langFromCode(settings().language));
}

// ---------------------------------------------------------------------------
// undo
// ---------------------------------------------------------------------------
void App::checkpoint() {
    std::string s = project.toJson();
    if (!m_undo.empty() && m_undo.back() == s) return;
    m_undo.push_back(std::move(s));
    while (m_undo.size() > 200) m_undo.pop_front();
    m_redo.clear();
    dirty = true;
}

void App::undo() {
    if (m_undo.empty()) return;
    std::string cur = project.toJson();
    std::string prev = m_undo.back(); m_undo.pop_back();
    if (prev == cur && !m_undo.empty()) { prev = m_undo.back(); m_undo.pop_back(); }
    m_redo.push_back(cur);
    project.fromJson(prev, nullptr);
    dirty = true;
}

void App::redo() {
    if (m_redo.empty()) return;
    m_undo.push_back(project.toJson());
    std::string next = m_redo.back(); m_redo.pop_back();
    project.fromJson(next, nullptr);
    dirty = true;
}

void App::autosave(double now) {
    if (!dirty || now - m_lastAutosave < 30.0) return;
    m_lastAutosave = now;
    writeFileAtomic(autosavePath(), project.toJson());
}

void App::emergencySave() {
    std::string s = project.toJson();
    std::ofstream f(std::filesystem::path(appDataDir() + L"\\crash_recovery.psproj"), std::ios::binary | std::ios::trunc);
    f.write(s.data(), (std::streamsize)s.size());
}

static void emergencySaveThunk() { if (g_app) g_app->emergencySave(); }

// ---------------------------------------------------------------------------
// janela principal
// ---------------------------------------------------------------------------
LRESULT CALLBACK App::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return TRUE;
    if (g_app) return g_app->handleMessage(h, msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT App::handleMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) m_needResize = true;
        return 0;
    case WM_DISPLAYCHANGE:
        m_displaysChanged = true;
        break;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        POINT pt; DragQueryPoint(drop, &pt);
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        pendingDrops.clear();
        for (UINT i = 0; i < n; ++i) {
            wchar_t path[MAX_PATH * 4];
            if (DragQueryFileW(drop, i, path, (UINT)std::size(path))) pendingDrops.push_back(toUtf8(path));
        }
        DragFinish(drop);
        // com painéis em vários monitores, o ImGui usa coordenadas do ecrã
        if (ImGui::GetCurrentContext() && (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) ClientToScreen(h, &pt);
        dropX = (float)pt.x; dropY = (float)pt.y;
        return 0;
    }
    case WM_CLOSE:
        if (dirty) {
            int r = MessageBoxW(h, fromUtf8(T("Gravar alterações ao projeto antes de sair?")).c_str(), L"PLAYERSLICE", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES) { saveOrSaveAs(); if (dirty) return 0; }
        }
        m_running = false;
        return 0;
    case WM_TIMER:
        // Só chega aqui a partir de ciclos modais do Windows (mover/redimensionar a janela,
        // diálogos de ficheiros, MessageBox): o ciclo principal deita fora os WM_TIMER.
        // Assim as saídas nunca congelam.
        if (wp == 1) {
            static bool inOutputsOnly = false;
            if (m_inUi) {                          // diálogo aberto a partir da interface
                if (!inOutputsOnly) { inOutputsOnly = true; outputsOnlyFrame(); inOutputsOnly = false; }
            } else if (!m_inTimerFrame) {          // a mover/redimensionar a janela
                m_inTimerFrame = true; frame(); m_inTimerFrame = false;
            }
        }
        return 0;
    case WM_MOVING:
    case WM_SIZING: {
        // a janela principal não entra nos ecrãs que estão a ser usados como saída
        RECT* r = (RECT*)lp;
        for (const RECT& m : outputs.monitorRects()) {
            RECT x;
            if (!IntersectRect(&x, r, &m)) continue;
            if (msg == WM_MOVING) {
                LONG w = r->right - r->left, h = r->bottom - r->top;
                LONG cx = (r->left + r->right) / 2, mcx = (m.left + m.right) / 2;
                if (cx < mcx) { r->right = m.left; r->left = r->right - w; }
                else { r->left = m.right; r->right = r->left + w; }
                (void)h;
            } else {
                if (r->right > m.left && r->left < m.left) r->right = m.left;
                else if (r->left < m.right && r->right > m.right) r->left = m.right;
                if (r->bottom > m.top && r->top < m.top) r->bottom = m.top;
                else if (r->top < m.bottom && r->bottom > m.bottom) r->top = m.bottom;
            }
        }
        return TRUE;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize = {960, 600};
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool App::initWindow(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, L"IDI_APP");
    wc.lpszClassName = L"PLAYERSLICE_MAIN";
    RegisterClassExW(&wc);
    m_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"PLAYERSLICE", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1600, 950, nullptr, nullptr, inst, nullptr);
    if (!m_hwnd) return false;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    DragAcceptFiles(m_hwnd, TRUE);
    // se a app correr como administrador, o Windows bloqueia o arrastar a partir do Explorador
    ChangeWindowMessageFilterEx(m_hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(m_hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(m_hwnd, 0x0049 /*WM_COPYGLOBALDATA*/, MSGFLT_ALLOW, nullptr);
    ShowWindow(m_hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(m_hwnd);
    return true;
}

bool App::initSwapchain() {
    Gfx& G = gfx();
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(G.factory()->CreateSwapChainForHwnd(G.dev(), m_hwnd, &sd, nullptr, nullptr, &m_swap))) return false;
    G.factory()->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
    ComPtr<ID3D11Texture2D> bb;
    m_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    G.dev()->CreateRenderTargetView(bb.Get(), nullptr, &m_rtv);
    return true;
}

void App::resizeSwapchain() {
    m_needResize = false;
    m_rtv.Reset();
    gfx().ctx()->OMSetRenderTargets(0, nullptr, nullptr);
    m_swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    ComPtr<ID3D11Texture2D> bb;
    m_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    gfx().dev()->CreateRenderTargetView(bb.Get(), nullptr, &m_rtv);
}

void App::outputsOnlyFrame() {
    double now = nowSeconds();
    comp.update(project, now);
    comp.render(project);
    auto live = [this](uint32_t id) { return isLive(id); };
    bool vsynced = false;
    outputs.present(comp, live, vsynced);
    if (!vsynced) Sleep(1);
}

void App::frame() {
    double now = nowSeconds();
    if (m_lastFrame > 0) {
        double dt = now - m_lastFrame;
        frameMs = frameMs * 0.9 + dt * 1000.0 * 0.1;
        fps = frameMs > 0 ? 1000.0 / frameMs : 0;
    }
    m_lastFrame = now;

    if (m_displaysChanged) { m_displaysChanged = false; refreshDisplays(); }
    if (m_needResize) resizeSwapchain();

    ThumbnailCache::get().poll();
    comp.update(project, now);
    comp.render(project);
    handleOsc();
    applyPendingLayout(*this);
    // página Clip Time na rede (~20 atualizações/s)
    static double lastWeb = 0;
    if (WebServer::get().running() && now - lastWeb > 0.05) { lastWeb = now; WebServer::get().setClipTimeJson(clipTimeJson(*this)); }

    // limpar ids de ecrãs apagados ou sem dispositivo
    for (auto it = liveScreens.begin(); it != liveScreens.end();) {
        const Screen* s = project.findScreen(*it);
        it = (!s || s->device == ScreenDevice::Disabled) ? liveScreens.erase(it) : std::next(it);
    }
    auto live = [this](uint32_t id) { return isLive(id); };
    outputs.sync(project, displays, live, settings().outputsOffBlack);
    updateMouseLock(settings().lockMouseOutputs ? outputs.monitorRects() : std::vector<RECT>{}, m_hwnd);

    // com saídas no ar a interface pode ir a 30 fps: as saídas continuam ao ritmo do vsync
    bool drawUi = !(settings().limitUiWhenLive && anyLive() && outputs.count() > 0) || now - m_lastUi >= 1.0 / 30.0 - 0.002;
    if (drawUi) {
        m_lastUi = now;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        m_inUi = true;
        drawUI(*this);
        m_inUi = false;
        ImGui::Render();
        pendingDrops.clear();

        Gfx& G = gfx();
        RECT cr; GetClientRect(m_hwnd, &cr);
        G.setTarget(m_rtv.Get(), cr.right - cr.left, cr.bottom - cr.top);
        G.clear(m_rtv.Get(), 0.08f, 0.08f, 0.09f, 1);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // painéis arrastados para fora da janela principal (outros monitores)
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
    }

    bool vsynced = false;
    outputs.present(comp, live, vsynced);
    if (drawUi) m_swap->Present(vsynced ? 0 : 1, 0);
    else if (!vsynced) Sleep(1);

    autosave(now);
}

int App::run(HINSTANCE inst) {
    g_app = this;
    std::wstring dir = appDataDir();
    Log::init(dir);
    PS_INFO("PLAYERSLICE %s — por Daniel Nunes", PLAYERSLICE_VERSION);
    CrashHandler::install(dir, emergencySaveThunk);

    // desempenho: prioridade alta, timer de 1 ms, sem throttling de energia
    timeBeginPeriod(1);
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));

    settings().load();
    applyLanguage();
    applyPerformance();

    std::string err;
    if (!gfx().init(settings().gpu, &err)) {
        MessageBoxW(nullptr, fromUtf8(std::string(T("Erro gráfico: ")) + err).c_str(), L"PLAYERSLICE", MB_ICONERROR);
        return 1;
    }
    VideoSystem::init();
    if (!initWindow(inst) || !initSwapchain()) {
        MessageBoxW(nullptr, fromUtf8(T("Não foi possível criar a janela principal")).c_str(), L"PLAYERSLICE", MB_ICONERROR);
        return 1;
    }
    SetTimer(m_hwnd, 1, 10, nullptr);   // mantém as saídas vivas durante ciclos modais do Windows

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;   // arrastar dentro dos monitores não move a janela
    io.ConfigDebugHighlightIdConflicts = false;
    static std::string iniPath = toUtf8(dir + L"\\layout-v2.ini");
    io.IniFilename = iniPath.c_str();
    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(gfx().dev(), gfx().ctx());
    setupTheme(GetDpiForWindow(m_hwnd) / 96.f);

    refreshDisplays();
    newProject();
    loadUiState(*this);
    applyOsc();
    WebServer::get().configure(settings().webEnabled, settings().webPort);

    // recuperar trabalho depois de um crash
    std::wstring rec = dir + L"\\crash_recovery.psproj";
    if (GetFileAttributesW(rec.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (MessageBoxW(m_hwnd, fromUtf8(T("O PLAYERSLICE fechou inesperadamente.\nRecuperar o projeto?")).c_str(), L"PLAYERSLICE",
                        MB_YESNO | MB_ICONWARNING) == IDYES) {
            openProject(rec);
            projectPath.clear();
            dirty = true;
        }
        DeleteFileW(rec.c_str());
    } else {
        // abrir projeto passado na linha de comandos (duplo clique num .psproj)
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc > 1) openProject(argv[1]);
        if (argv) LocalFree(argv);
    }

    if (settings().checkUpdates) Updater::get().check(false);

    while (m_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            if (msg.message == WM_TIMER && msg.hwnd == m_hwnd && msg.wParam == 1) continue;   // ver WM_TIMER
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) m_running = false;
        }
        if (!m_running) break;
        if (IsIconic(m_hwnd) && outputs.count() == 0) { Sleep(10); }
        frame();
    }

    saveUiState(*this);
    updateMouseLock({}, m_hwnd);
    Osc::get().shutdown();
    WebServer::get().shutdown();
    outputs.closeAll();
    comp.stopAll(project);
    comp.shutdown();
    Sleep(50); // dar tempo aos descodificadores em destruição assíncrona
    ThumbnailCache::get().clear();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_rtv.Reset(); m_swap.Reset();
    VideoSystem::shutdown();
    gfx().shutdown();
    timeEndPeriod(1);
    SetThreadExecutionState(ES_CONTINUOUS);
    PS_INFO("Fim");
    Log::shutdown();
    DestroyWindow(m_hwnd);
    return 0;
}

} // namespace ps
