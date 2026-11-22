#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// Pequeno servidor HTTP só de leitura para a rede local:
//   /               → página Clip Time (atualiza sozinha)
//   /cliptime.json  → dados atuais (JSON)
namespace ps {

class WebServer {
public:
    static WebServer& get();
    void configure(bool enabled, int port);
    void shutdown();
    void setClipTimeJson(std::string json);     // thread principal, a cada frame
    bool running() const { return m_running.load(); }
    std::string lastError() const;
    int port() const { return m_port; }

private:
    void loop(int port);
    void handle(uintptr_t client);
    std::thread m_thread;
    std::atomic<bool> m_running{false}, m_stop{false};
    uintptr_t m_sock = ~(uintptr_t)0;
    mutable std::mutex m_mtx;
    std::string m_json = "{}", m_error;
    bool m_enabled = false;
    int m_port = 0;
};

} // namespace ps
