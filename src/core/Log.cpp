#include "core/Log.h"
#include <windows.h>
#include <shlobj.h>
#include <deque>
#include <cstdio>

namespace ps {

static std::mutex        g_mtx;
static std::deque<LogEntry> g_entries;
static FILE*             g_file = nullptr;

double nowSeconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring appDataDir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        dir = std::wstring(p) + L"\\PLAYERSLICE";
        CoTaskMemFree(p);
    } else {
        dir = L".\\PLAYERSLICE";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void Log::init(const std::wstring& dir) {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::wstring path = dir + L"\\playerslice.log";
    std::wstring old  = dir + L"\\playerslice.old.log";
    MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfopen(path.c_str(), L"w");
}

void Log::write(LogLevel lvl, const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(g_mtx);
    LogEntry e{lvl, nowSeconds(), buf};
    g_entries.push_back(e);
    while (g_entries.size() > 2000) g_entries.pop_front();
    if (g_file) {
        SYSTEMTIME st; GetLocalTime(&st);
        const char* tag = lvl == LogLevel::Info ? "INFO " : lvl == LogLevel::Warn ? "AVISO" : "ERRO ";
        fprintf(g_file, "%02d:%02d:%02d.%03d [%s] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tag, buf);
        fflush(g_file);
    }
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

std::vector<LogEntry> Log::recent(size_t max) {
    std::lock_guard<std::mutex> lk(g_mtx);
    size_t start = g_entries.size() > max ? g_entries.size() - max : 0;
    return std::vector<LogEntry>(g_entries.begin() + start, g_entries.end());
}

void Log::shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_file) { fclose(g_file); g_file = nullptr; }
}

} // namespace ps
