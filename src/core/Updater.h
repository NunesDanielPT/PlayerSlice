#pragma once
#include <atomic>
#include <mutex>
#include <string>

// Atualizações automáticas via GitHub Releases:
//   GET https://api.github.com/repos/<repo>/releases/latest
//   compara a tag (vX.Y.Z) com a versão atual, descarrega PLAYERSLICE-Setup.exe
//   e corre o instalador em modo silencioso (que volta a abrir a app).
namespace ps {

class Updater {
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Ready, Error };

    static Updater& get();
    void check(bool userInitiated);   // assíncrono
    void download();                   // assíncrono (depois de Available)
    bool install();                    // lança o instalador; a app deve fechar a seguir

    State state() const { return m_state.load(); }
    float progress() const { return m_progress.load(); }
    std::string latestVersion() const;
    std::string notes() const;
    std::string error() const;
    bool userInitiated() const { return m_user; }
    std::atomic<bool> popupPending{false};   // mostrar aviso quando há versão nova

    static int compareVersions(const std::string& a, const std::string& b); // <0, 0, >0

private:
    void runCheck();
    void runDownload();
    std::atomic<State> m_state{State::Idle};
    std::atomic<float> m_progress{0};
    mutable std::mutex m_mtx;
    std::string m_latest, m_notes, m_url, m_error, m_file;
    bool m_user = false;
};

} // namespace ps
