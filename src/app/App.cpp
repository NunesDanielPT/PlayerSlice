#include "app/App.h"
#include "core/Log.h"
#include "core/CrashHandler.h"
#include "core/I18n.h"
#include "core/Timecode.h"
#include "render/NdiOutput.h"
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
    case Action::TestPattern:   toggleTestPattern(); break;
    case Action::Save:          saveOrSaveAs(); break;
    case Action::SaveAs:        saveAsDialog(); break;
    case Action::Open:          openDialog(); break;
    case Action::Undo:          undo(); break;
    case Action::Redo:          redo(); break;
    case Action::Settings:      ui.showSettings = true; break;
    case Action::OutputEditor:  ui.focusEditor = true; break;
    case Action::TakePreview:   comp.takePreview(project); break;
    case Action::ClearPreview:  comp.clearPreview(); break;
    case Action::PrevStack:     selectStack(project.comp.activeStack - 1); break;
    case Action::NextStack:     selectStack(project.comp.activeStack + 1); break;
    case Action::NewStack:      newStack(false); break;
    case Action::CueGo:         cueGo(); break;
    case Action::CueBack:       cueBack(); break;
    default:
        if (a >= (int)Action::Column1 && a <= (int)Action::Column9) comp.triggerColumn(project, a - (int)Action::Column1);
        break;
    }
}

// nomes automáticos ("Coluna 3") acompanham o idioma da interface ("Column 3")
void App::localizeDefaultNames() {
    const bool en = language() == Lang::EN;
    const char* from = en ? "Coluna " : "Column ";
    const char* to   = en ? "Column " : "Coluna ";
    const size_t fl = strlen(from);
    auto fix = [&](std::vector<Column>& cols) {
        for (auto& c : cols) {
            if (c.name.size() <= fl || c.name.compare(0, fl, from) != 0) continue;
            bool digits = true;
            for (size_t k = fl; k < c.name.size(); ++k) if (!isdigit((unsigned char)c.name[k])) { digits = false; break; }
            if (digits) c.name = to + c.name.substr(fl);
        }
    };
    fix(project.comp.columns);
    for (auto& s : project.comp.stacks) fix(s.columns);
}

void App::matchOutputRefresh() {
    if (!settings().matchOutputHz) return;
    const float fps = project.comp.fps;
    if (fps <= 0) return;                         // automático: não mexe nos monitores
    static double lastTry = 0;
    double now = nowSeconds();
    if (now - lastTry < 0.5) return;              // no máximo 2 verificações/s
    lastTry = now;
    for (uint32_t id : liveScreens) {
        const Screen* s = project.findScreen(id);
        if (!s || s->device != ScreenDevice::Monitor || s->monitorKey.empty()) continue;
        auto it = m_hzApplied.find(s->monitorKey);
        if (it != m_hzApplied.end() && std::fabs(it->second - fps) < 0.001f) continue;
        m_hzApplied[s->monitorKey] = fps;         // tenta uma vez por valor de fps (mantém-se ao desligar)
        int hz = setDisplayRefresh(s->monitorKey, fps);
        if (hz > 0) m_displaysChanged = true;
    }
}

void App::selectStack(int i) {
    if (i < 0 || i >= (int)project.comp.stacks.size() || i == project.comp.activeStack) return;
    project.switchStack(i);
    ui.selCells.clear();
    ui.selColumn = std::min(ui.selColumn, (int)project.comp.columns.size() - 1);
    ui.selColumnProp = std::min(ui.selColumnProp, (int)project.comp.columns.size() - 1);
    dirty = true;
}

void App::newStack(bool duplicate) {
    checkpoint();
    std::string name = duplicate ? project.comp.stacks[project.comp.activeStack].name + T(" (cópia)")
                                 : "Stack " + std::to_string(project.comp.stacks.size() + 1);
    project.addStack(name, duplicate);
    ui.selCells.clear();
    dirty = true;
}

void App::applyPerformance() {
    const Settings& s = settings();
    DWORD cls = s.priority >= 2 ? HIGH_PRIORITY_CLASS : s.priority == 1 ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
    SetPriorityClass(GetCurrentProcess(), cls);
    SetThreadExecutionState(s.preventSleep ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS);
}

void App::applyTimecode() {
    const Settings& s = settings();
    TcConfig c;
    c.source = (TcSource)s.tcSource;
    c.audioId = s.tcAudioId; c.channel = s.tcChannel; c.midiName = s.tcMidi;
    c.fps = s.tcFps; c.offset = s.tcOffset; c.freewheel = s.tcFreewheel;
    Timecode::get().configure(c);
}

// clips com "Disparar": entram no ar quando o timecode passa pela hora de início
// (ou quando salta para dentro da duração do clip). Só a stack visível.
void App::timecodeTriggers() {
    static double prev = -1;
    TcStatus st = Timecode::get().status();
    if (!st.valid || !st.running) { prev = st.valid ? st.seconds : -1; return; }
    const double t = st.seconds;
    if (prev < 0) { prev = t; return; }
    const bool jump = t < prev - 0.001 || t - prev > 0.5;
    Project& p = project;
    // colunas com hora marcada (a mais recente que foi atravessada ganha)
    {
        int best = -1; double bestT = -1;
        for (int c = 0; c < (int)p.comp.columns.size(); ++c) {
            const Column& col = p.comp.columns[c];
            if (!col.tcTrigger) continue;
            bool hit = !jump ? (prev < col.tcStart && t >= col.tcStart) : (t >= col.tcStart && t < col.tcStart + 0.5);
            if (hit && col.tcStart >= bestT) { best = c; bestT = col.tcStart; }
        }
        if (best >= 0) {
            comp.triggerColumn(p, best);
            PS_INFO("Timecode %s: %s", tcFormat(t, st.fps).c_str(), p.comp.columns[best].name.c_str());
        }
    }
    for (int li = 0; li < (int)p.comp.layers.size(); ++li) {
        Layer& l = p.comp.layers[li];
        int best = -1; double bestStart = -1;
        for (int c = 0; c < (int)l.clips.size(); ++c) {
            const Clip& cl = l.clips[c];
            if (cl.empty() || !tcTriggers(cl.tcMode)) continue;
            bool hit = false;
            if (!jump) hit = prev < cl.tcStart && t >= cl.tcStart;
            else {
                double dur = 0;
                auto th = ThumbnailCache::get().request(cl.path);
                if (th->state == ThumbnailCache::Entry::State::Ready) dur = th->info.duration / std::max(0.01f, cl.speed);
                hit = t >= cl.tcStart && t < cl.tcStart + (dur > 0 ? dur : 0.5);
            }
            if (hit && cl.tcStart >= bestStart) { best = c; bestStart = cl.tcStart; }   // o mais recente ganha
        }
        if (best >= 0 && !(l.activeColumn == best && l.playing)) {
            comp.trigger(p, li, best);
            PS_INFO("Timecode %s: %s → %s", tcFormat(t, st.fps).c_str(), l.name.c_str(), l.clips[best].name.c_str());
        }
    }
    prev = t;
}

int App::findCue(const std::string& number) const {
    const auto& q = project.comp.cues;
    for (int i = 0; i < (int)q.size(); ++i) if (q[i].number == number) return i;
    // "3" também encontra "3.0" e vice-versa
    char* e = nullptr;
    double v = strtod(number.c_str(), &e);
    if (e && *e == 0 && !number.empty())
        for (int i = 0; i < (int)q.size(); ++i) {
            char* e2 = nullptr; double w = strtod(q[i].number.c_str(), &e2);
            if (e2 && *e2 == 0 && !q[i].number.empty() && std::fabs(v - w) < 1e-6) return i;
        }
    return -1;
}

void App::fireCue(int i) {
    auto& q = project.comp.cues;
    if (i < 0 || i >= (int)q.size()) return;
    const Cue& c = q[i];
    if (c.stack < 0 || c.stack >= (int)project.comp.stacks.size()) { PS_WARN("Cue %s: stack inexistente", c.number.c_str()); return; }
    if (c.stack != project.comp.activeStack) selectStack(c.stack);
    if (c.column < 0 || c.column >= (int)project.comp.columns.size()) { PS_WARN("Cue %s: coluna inexistente", c.number.c_str()); return; }
    comp.triggerColumn(project, c.column);
    cueIndex = i;
    PS_INFO("Cue %s %s → %s / %s", c.number.c_str(), c.name.c_str(), project.comp.stacks[c.stack].name.c_str(), project.comp.columns[c.column].name.c_str());
    if (settings().oscOut) {
        Osc::get().send("/playerslice/cue/current", c.number);
        Osc::get().send("/playerslice/cue/name", c.name);
    }
}

void App::cueGo() { fireCue(cueIndex + 1 < (int)project.comp.cues.size() ? cueIndex + 1 : cueIndex); }
void App::cueBack() { fireCue(std::max(0, cueIndex - 1)); }

bool App::anyTestPattern() const {
    for (auto& s : project.screens) if (s.testPattern) return true;
    return false;
}

void App::toggleTestPattern() {
    bool on = !anyTestPattern();
    for (auto& s : project.screens) s.testPattern = on;
    dirty = true;
}

void App::applyOsc() {
    const Settings& s = settings();
    Osc::get().configure(s.oscIn, s.oscInPort, s.oscOut, s.oscOutAddr, s.oscOutPort);
    m_oscSentColumn.clear();
    m_oscSentLive = -1;
}

// ---------------------------------------------------------------------------
// estado no formato do Resolume (respostas a "?" e feedback)
// connected: 0 = vazio/desligado, 1 = carregado, 2 = no ar (o Companion considera >= 2 "ligado")
// ---------------------------------------------------------------------------
static float clipLength(const Clip& c) {
    auto th = ThumbnailCache::get().request(c.path);
    double d = th->state == ThumbnailCache::Entry::State::Ready ? th->info.duration : 0;
    double end = c.trimOut > 0 ? (d > 0 ? std::min(c.trimOut, d) : c.trimOut) : d;
    return (float)std::max(0.0, end - c.trimIn);
}

static float layerPosition(App& app, const Layer& l) {
    VideoPlayer* v = app.comp.activePlayer(l.id);
    if (!v || l.activeColumn < 0) return 0.f;
    double len = v->trimLength();
    if (len <= 0) return 0.f;
    return (float)std::clamp(std::fmod(std::max(0.0, v->position()), len) / len, 0.0, 1.0);
}

static int columnConnected(App& app, int c) {
    const Project& p = app.project;
    bool any = false, live = false;
    for (auto& l : p.comp.layers) {
        if (c < (int)l.clips.size() && !l.clips[c].empty()) any = true;
        if (l.activeColumn == c) live = true;
    }
    if (live && app.comp.lastColumn() == c) return 2;
    return any ? 1 : 0;
}

void App::oscQuery(const OscMessage& m, const std::vector<std::string>& a) {
    Project& p = project;
    const int nL = (int)p.comp.layers.size(), nC = (int)p.comp.columns.size();
    auto F = [](float f) { OscArg x; x.type = 'f'; x.f = f; return x; };
    auto I = [](int i) { OscArg x; x.type = 'i'; x.i = i; return x; };
    auto S = [](const std::string& s) { OscArg x; x.type = 's'; x.s = s; return x; };
    auto R = [&](const std::string& addr, const OscArg& v) { Osc::get().reply(m.from, addr, v); };
    auto range = [](const std::string& tok, int n) {
        std::vector<int> r;
        if (tok == "*") { for (int i = 1; i <= n; ++i) r.push_back(i); }
        else { int k = atoi(tok.c_str()); if (k >= 1 && k <= n) r.push_back(k); }
        return r;
    };
    if (a.size() < 4 || a[0] != "composition") return;
    if (a[1] == "columns") {
        for (int c : range(a[2], nC)) {
            std::string base = "/composition/columns/" + std::to_string(c) + "/";
            if (a[3] == "connected") R(base + "connected", I(columnConnected(*this, c - 1)));
            else if (a[3] == "name") R(base + "name", S(p.comp.columns[c - 1].name));
        }
        return;
    }
    if (a[1] != "layers") return;
    for (int L : range(a[2], nL)) {
        Layer& l = p.comp.layers[nL - L];
        std::string lb = "/composition/layers/" + std::to_string(L) + "/";
        if (a.size() == 4) {
            if (a[3] == "position") R(lb + "position", F(layerPosition(*this, l)));
            else if (a[3] == "name") R(lb + "name", S(l.name));
            else if (a[3] == "bypassed") R(lb + "bypassed", I(l.bypass ? 1 : 0));
            else if (a[3] == "master") R(lb + "master", F(l.opacity));
            continue;
        }
        if (a.size() == 5 && a[3] == "video" && a[4] == "opacity") { R(lb + "video/opacity", F(l.opacity)); continue; }
        if (a[3] != "clips" || a.size() < 6) continue;
        std::string rest;
        for (size_t k = 5; k < a.size(); ++k) rest += (k > 5 ? "/" : "") + a[k];
        const bool wild = a[4] == "*";
        for (int c : range(a[4], (int)l.clips.size())) {
            const Clip& cl = l.clips[c - 1];
            if (wild && cl.empty() && rest != "connected") continue;
            std::string cb = lb + "clips/" + std::to_string(c) + "/";
            bool live = l.activeColumn == c - 1;
            if (rest == "connected") { if (!wild || !cl.empty() || live) R(cb + "connected", I(cl.empty() ? 0 : live ? 2 : 1)); }
            else if (rest == "name") R(cb + "name", S(cl.name));
            else if (rest == "transport/position") R(cb + "transport/position", F(live ? layerPosition(*this, l) : 0.f));
            else if (rest == "transport/position/behaviour/duration") R(cb + "transport/position/behaviour/duration", F(clipLength(cl)));
            else if (rest == "transport/position/behaviour/speed") R(cb + "transport/position/behaviour/speed", F(cl.speed));
        }
    }
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
        // pergunta "?" (Companion, como no Resolume): responder a quem perguntou e não executar nada
        if (!m.args.empty() && m.args[0].type == 's' && m.args[0].s == "?") { oscQuery(m, a); continue; }
        const float v = m.value(1.f);
        const bool press = v > 0.5f;
        const bool toggle = !m.args.empty() && m.args[0].type == 's' && m.args[0].s == "!";   // "!" = alternar (Companion)
        // coluna seguinte/anterior com clips (a partir da última posta no ar)
        auto stepColumn = [&](int dir) {
            int n = (int)p.comp.columns.size();
            if (n <= 0) return;
            int c = comp.lastColumn();
            for (int k = 0; k < n; ++k) {
                c = c < 0 ? (dir > 0 ? 0 : n - 1) : (c + dir + n) % n;
                bool any = false;
                for (auto& l : p.comp.layers) if (c < (int)l.clips.size() && !l.clips[c].empty()) any = true;
                if (any) { comp.triggerColumn(p, c); ui.selColumnProp = c; return; }
            }
        };
        if (a.size() >= 2 && a[0] == "composition") {
            if (a[1] == "disconnectall") { if (press) comp.stopAll(p); }
            else if (a[1] == "connectnextcolumn") { if (press) stepColumn(+1); }
            else if (a[1] == "connectprevcolumn") { if (press) stepColumn(-1); }
            else if (a[1] == "master") p.comp.master = std::clamp(v, 0.f, 1.f);
            else if (a[1] == "decks" && a.size() >= 4 && a[3] == "select") { if (press) selectStack(num(2) - 1); }
            else if (a[1] == "selectnextdeck") { if (press) selectStack(p.comp.activeStack + 1); }
            else if (a[1] == "selectprevdeck") { if (press) selectStack(p.comp.activeStack - 1); }
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
                else if ((a[3] == "connectnextclip" || a[3] == "connectprevclip") && press) {
                    int n = (int)l.clips.size(), dir = a[3] == "connectnextclip" ? 1 : -1;
                    int c = l.activeColumn;
                    for (int k = 0; k < n; ++k) {
                        c = c < 0 ? (dir > 0 ? 0 : n - 1) : (c + dir + n) % n;
                        if (!l.clips[c].empty()) { comp.trigger(p, li, c); break; }
                    }
                }
                else if (a[3] == "clips" && a.size() >= 6) {
                    int c = num(4) - 1;
                    if (c < 0 || c >= (int)l.clips.size()) continue;
                    Clip& cl = l.clips[c];
                    std::string rest;
                    for (size_t k = 5; k < a.size(); ++k) rest += (k > 5 ? "/" : "") + a[k];
                    if (rest == "connect" && press) comp.trigger(p, li, c);
                    else if (rest == "select" && press) comp.previewClip(p, li, c);
                    else if (rest == "video/opacity") cl.opacity = std::clamp(v, 0.f, 1.f);
                    else if (rest == "transport/position" && l.activeColumn == c) {
                        // saltar para uma posição (0..1) no clip que está no ar
                        if (VideoPlayer* vp = comp.activePlayer(l.id)) vp->seek(std::clamp(v, 0.f, 1.f) * vp->trimLength());
                    }
                }
                else if (a[3] == "direction") {
                    // Resolume: 0 para trás, 1 pausa, 2 para a frente
                    if (VideoPlayer* vp = comp.activePlayer(l.id)) vp->setPaused((int)std::lround(v) == 1);
                }
                else if (a[3] == "select" && press) ui.selLayer = li;
                else if ((a[3] == "video" && a.size() >= 5 && a[4] == "opacity") || a[3] == "master") l.opacity = std::clamp(v, 0.f, 1.f);
                else if (a[3] == "bypassed") l.bypass = toggle ? !l.bypass : press;
                else if (a[3] == "solo") l.solo = toggle ? !l.solo : press;
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
            else if (a[1] == "cue" && a.size() >= 3) {
                // /playerslice/cue/go | back | fire <número> | /playerslice/cue/<número>
                if (a[2] == "go") { if (press) cueGo(); }
                else if (a[2] == "back") { if (press) cueBack(); }
                else if (a[2] == "fire") {
                    std::string n;
                    if (!m.args.empty()) {
                        const OscArg& g = m.args[0];
                        if (g.type == 's') n = g.s;
                        else { char b[32]; float f = g.asFloat(); snprintf(b, sizeof(b), f == std::floor(f) ? "%.0f" : "%g", f); n = b; }
                    }
                    int i = findCue(n);
                    if (i >= 0) fireCue(i); else PS_WARN("OSC: cue %s não existe", n.c_str());
                } else if (press) {
                    std::string n = a[2];
                    for (size_t k = 3; k < a.size(); ++k) n += "/" + a[k];
                    int i = findCue(n);
                    if (i >= 0) fireCue(i); else PS_WARN("OSC: cue %s não existe", n.c_str());
                }
            }
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
        if (sent >= 0) Osc::get().send(base + std::to_string(sent + 1) + "/connected", 1);
        if (cur >= 0) {
            Osc::get().send(base + std::to_string(cur + 1) + "/connected", 2);
            const Clip& cl = p.comp.layers[li].clips[cur];
            Osc::get().send(base + std::to_string(cur + 1) + "/name", cl.name);
            Osc::get().send(base + std::to_string(cur + 1) + "/transport/position/behaviour/duration", clipLength(cl));
        }
        sent = cur;
    }
    // coluna ativa
    {
        int col = -1;
        for (int c = 0; c < (int)p.comp.columns.size(); ++c) if (columnConnected(*this, c) == 2) { col = c; break; }
        if (col != m_oscSentCol) {
            if (m_oscSentCol >= 0) Osc::get().send("/composition/columns/" + std::to_string(m_oscSentCol + 1) + "/connected", 1);
            if (col >= 0) Osc::get().send("/composition/columns/" + std::to_string(col + 1) + "/connected", 2);
            m_oscSentCol = col;
        }
    }
    // posição dos clips no ar (10 vezes por segundo, para os contadores de tempo do Companion)
    double nowT = nowSeconds();
    if (nowT - m_oscLastPos >= 0.1) {
        m_oscLastPos = nowT;
        for (int li = 0; li < nL; ++li) {
            const Layer& l = p.comp.layers[li];
            if (l.activeColumn < 0) continue;
            float pos = layerPosition(*this, l);
            std::string lb = "/composition/layers/" + std::to_string(nL - li) + "/";
            Osc::get().sendQuiet(lb + "position", pos);
            Osc::get().sendQuiet(lb + "clips/" + std::to_string(l.activeColumn + 1) + "/transport/position", pos);
        }
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
    NdiOutputs::get().present(project, comp, live, project.comp.fps > 0 ? project.comp.fps : (float)outputs.outputFps());
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
    timecodeTriggers();
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
    matchOutputRefresh();
    updateMouseLock(settings().lockMouseOutputs ? outputs.monitorRects() : std::vector<RECT>{}, m_hwnd);

    // com saídas no ar a interface pode ir a 30 fps: as saídas continuam ao ritmo do vsync
    bool drawUi = !(settings().limitUiWhenLive && anyLive() && outputs.count() > 0) || now - m_lastUi >= 1.0 / 30.0 - 0.002;
    if (drawUi) {
        m_lastUi = now;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        m_inUi = true;
        localizeDefaultNames();
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
    NdiOutputs::get().present(project, comp, live, project.comp.fps > 0 ? project.comp.fps : (float)outputs.outputFps());
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
    // painéis soltos pertencem à janela principal: sem ícone próprio na barra de tarefas,
    // minimizam e ficam sempre à frente da janela principal
    io.ConfigViewportsNoTaskBarIcon = true;
    io.ConfigViewportsNoDefaultParent = false;
    static std::string iniPath = toUtf8(dir + L"\\layout-v2.ini");
    io.IniFilename = iniPath.c_str();
    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(gfx().dev(), gfx().ctx());
    setupTheme(GetDpiForWindow(m_hwnd) / 96.f);

    refreshDisplays();
    newProject();
    loadUiState(*this);
    applyOsc();
    applyTimecode();
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
    Timecode::get().shutdown();
    NdiOutputs::get().shutdown();
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
