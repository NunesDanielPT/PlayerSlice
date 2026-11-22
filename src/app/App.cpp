#include "app/App.h"
#include "core/Log.h"
#include "core/CrashHandler.h"
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

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ps {

static App* g_app = nullptr;
App* App::instance() { return g_app; }

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
    project = std::move(p);
    projectPath = path;
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
    of.lpstrFilter = L"Projeto PLAYERSLICE (*.psproj)\0*.psproj\0";
    of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = L"psproj";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&of)) saveProject(file);
}

void App::openDialog() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = m_hwnd;
    of.lpstrFilter = L"Projeto PLAYERSLICE (*.psproj)\0*.psproj\0";
    of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&of)) openProject(file);
}

std::vector<std::string> App::pickVideoFiles() {
    std::vector<wchar_t> buf(32768, 0);
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = m_hwnd;
    of.lpstrFilter = L"Vídeo\0*.mov;*.mp4;*.mkv;*.avi;*.mxf;*.m4v;*.webm;*.ts\0Todos\0*.*\0";
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
        dropX = (float)pt.x; dropY = (float)pt.y;
        return 0;
    }
    case WM_CLOSE:
        if (dirty) {
            int r = MessageBoxW(h, L"Gravar alterações ao projeto antes de sair?", L"PLAYERSLICE", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES) { saveOrSaveAs(); if (dirty) return 0; }
        }
        m_running = false;
        return 0;
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
    outputs.sync(project, displays, m_hwnd);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    drawUI(*this);
    ImGui::Render();

    Gfx& G = gfx();
    RECT cr; GetClientRect(m_hwnd, &cr);
    G.setTarget(m_rtv.Get(), cr.right - cr.left, cr.bottom - cr.top);
    G.clear(m_rtv.Get(), 0.08f, 0.08f, 0.09f, 1);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    bool vsynced = false;
    outputs.present(project, comp, vsynced);
    m_swap->Present(vsynced ? 0 : 1, 0);

    autosave(now);
}

int App::run(HINSTANCE inst) {
    g_app = this;
    std::wstring dir = appDataDir();
    Log::init(dir);
    PS_INFO("PLAYERSLICE %s — por Daniel Nunes", PLAYERSLICE_VERSION);
    CrashHandler::install(dir, emergencySaveThunk);

    // desempenho: prioridade alta, timer de 1 ms, sem throttling de energia
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    timeBeginPeriod(1);
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED); // não desligar ecrãs

    std::string err;
    if (!gfx().init(&err)) {
        MessageBoxW(nullptr, fromUtf8("Erro gráfico: " + err).c_str(), L"PLAYERSLICE", MB_ICONERROR);
        return 1;
    }
    VideoSystem::init();
    if (!initWindow(inst) || !initSwapchain()) {
        MessageBoxW(nullptr, L"Não foi possível criar a janela principal", L"PLAYERSLICE", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    static std::string iniPath = toUtf8(dir + L"\\layout.ini");
    io.IniFilename = iniPath.c_str();
    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(gfx().dev(), gfx().ctx());
    setupTheme(GetDpiForWindow(m_hwnd) / 96.f);

    refreshDisplays();
    newProject();

    // recuperar trabalho depois de um crash
    std::wstring rec = dir + L"\\crash_recovery.psproj";
    if (GetFileAttributesW(rec.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (MessageBoxW(m_hwnd, L"O PLAYERSLICE fechou inesperadamente.\nRecuperar o projeto?", L"PLAYERSLICE",
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

    while (m_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) m_running = false;
        }
        if (!m_running) break;
        if (IsIconic(m_hwnd) && outputs.count() == 0) { Sleep(10); }
        frame();
    }

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
