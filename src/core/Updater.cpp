#include "core/Updater.h"
#include "core/Log.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <thread>
#include <vector>
#include <cstdio>

#ifndef PLAYERSLICE_UPDATE_REPO
#define PLAYERSLICE_UPDATE_REPO "NunesDanielPT/PlayerSlice"
#endif

namespace ps {

Updater& Updater::get() { static Updater u; return u; }

std::string Updater::latestVersion() const { std::lock_guard<std::mutex> lk(m_mtx); return m_latest; }
std::string Updater::notes() const { std::lock_guard<std::mutex> lk(m_mtx); return m_notes; }
std::string Updater::error() const { std::lock_guard<std::mutex> lk(m_mtx); return m_error; }

int Updater::compareVersions(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& s) {
        std::vector<int> v;
        size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
        int cur = 0; bool any = false;
        for (; i < s.size(); ++i) {
            if (s[i] >= '0' && s[i] <= '9') { cur = cur * 10 + (s[i] - '0'); any = true; }
            else if (s[i] == '.') { v.push_back(cur); cur = 0; any = false; }
            else break;
        }
        if (any || !v.empty()) v.push_back(cur);
        while (v.size() < 3) v.push_back(0);
        return v;
    };
    auto x = parse(a), y = parse(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

// GET simples com WinHTTP (segue redirecionamentos; HTTPS com validação de certificado)
static bool httpGet(const std::wstring& url, std::string* body, FILE* file, std::atomic<float>* progress, std::string& err) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    wchar_t extra[2048]; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "URL inválido"; return false; }
    std::wstring fullPath = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);

    HINTERNET s = WinHttpOpen(L"PLAYERSLICE-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { err = "WinHttpOpen"; return false; }
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 60000);
    HINTERNET c = WinHttpConnect(s, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    if (r) {
        const wchar_t* hdr = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
        if (WinHttpSendRequest(r, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
            DWORD total = 0; sz = sizeof(total);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &total, &sz, nullptr);
            if (status == 200) {
                ok = true;
                size_t got = 0;
                std::vector<char> buf(64 * 1024);
                for (;;) {
                    DWORD n = 0;
                    if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &n)) { ok = false; err = "Ligação interrompida"; break; }
                    if (n == 0) break;
                    if (body) body->append(buf.data(), n);
                    if (file && fwrite(buf.data(), 1, n, file) != n) { ok = false; err = "Erro a escrever no disco"; break; }
                    got += n;
                    if (progress && total) progress->store((float)got / total);
                }
            } else {
                err = status == 404 ? "Sem releases publicadas (ou repositório privado)" : "HTTP " + std::to_string(status);
            }
        } else {
            err = "Sem ligação à internet";
        }
    } else if (err.empty()) {
        err = "Sem ligação à internet";
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

void Updater::check(bool user) {
    State st = m_state.load();
    if (st == State::Checking || st == State::Downloading) return;
    m_user = user;
    m_state = State::Checking;
    std::thread([this] { runCheck(); }).detach();
}

void Updater::runCheck() {
    std::string body, err;
    std::wstring url = L"https://api.github.com/repos/" + fromUtf8(PLAYERSLICE_UPDATE_REPO) + L"/releases/latest";
    if (!httpGet(url, &body, nullptr, nullptr, err)) {
        { std::lock_guard<std::mutex> lk(m_mtx); m_error = err; }
        m_state = State::Error;
        PS_WARN("Atualizações: %s", err.c_str());
        return;
    }
    try {
        auto j = nlohmann::json::parse(body);
        std::string tag = j.value("tag_name", "");
        std::string assetUrl;
        for (auto& a : j.value("assets", nlohmann::json::array()))
            if (a.value("name", "") == "PLAYERSLICE-Setup.exe") assetUrl = a.value("browser_download_url", "");
        std::lock_guard<std::mutex> lk(m_mtx);
        m_latest = tag;
        m_notes = j.value("body", "");
        m_url = assetUrl;
        if (compareVersions(tag, PLAYERSLICE_VERSION) > 0 && !assetUrl.empty()) {
            m_state = State::Available;
            popupPending = true;
            PS_INFO("Nova versão disponível: %s", tag.c_str());
        } else {
            m_state = State::UpToDate;
        }
    } catch (const std::exception& e) {
        { std::lock_guard<std::mutex> lk(m_mtx); m_error = e.what(); }
        m_state = State::Error;
    }
}

void Updater::download() {
    if (m_state.load() != State::Available) return;
    m_state = State::Downloading;
    m_progress = 0;
    std::thread([this] { runDownload(); }).detach();
}

void Updater::runDownload() {
    std::string url;
    { std::lock_guard<std::mutex> lk(m_mtx); url = m_url; }
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"PLAYERSLICE-Setup.exe";
    FILE* f = _wfopen(path.c_str(), L"wb");
    std::string err;
    bool ok = f && httpGet(fromUtf8(url), nullptr, f, &m_progress, err);
    if (f) fclose(f);
    if (!ok) {
        { std::lock_guard<std::mutex> lk(m_mtx); m_error = err.empty() ? "Erro no download" : err; }
        DeleteFileW(path.c_str());
        m_state = State::Error;
        return;
    }
    { std::lock_guard<std::mutex> lk(m_mtx); m_file = toUtf8(path); }
    m_progress = 1;
    m_state = State::Ready;
}

bool Updater::install() {
    std::string file;
    { std::lock_guard<std::mutex> lk(m_mtx); file = m_file; }
    if (m_state.load() != State::Ready || file.empty()) return false;
    // /SILENT mostra só a barra de progresso; o instalador fecha a app e volta a abri-la
    HINSTANCE h = ShellExecuteW(nullptr, L"open", fromUtf8(file).c_str(), L"/SILENT /SP- /NOCANCEL /CLOSEAPPLICATIONS", nullptr, SW_SHOWNORMAL);
    return (INT_PTR)h > 32;
}

} // namespace ps
