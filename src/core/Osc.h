#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// OSC (UDP) — controlo pelo Bitfocus Companion, TouchOSC, etc.
// Endereços compatíveis com o Resolume Arena (layers e colunas contam a partir de 1;
// a layer 1 é a de baixo, como no Resolume).
namespace ps {

struct OscArg {
    char type = 'i';       // i, f, s, T, F
    int i = 0;
    float f = 0;
    std::string s;
    float asFloat() const { return type == 'f' ? f : type == 'i' ? (float)i : type == 'T' ? 1.f : 0.f; }
};

struct OscMessage {
    std::string address;
    std::vector<OscArg> args;
    std::string from;       // ip:porta
    float value(float def = 1.f) const { return args.empty() ? def : args[0].asFloat(); }
};

class Osc {
public:
    static Osc& get();
    // aplica as definições (liga/desliga entrada e saída)
    void configure(bool inEnabled, int inPort, bool outEnabled, const std::string& outAddr, int outPort);
    void shutdown();

    std::vector<OscMessage> poll();                     // mensagens recebidas (thread principal)
    void send(const std::string& address, float value);
    void sendQuiet(const std::string& address, float value);   // sem registo no monitor (envios contínuos)
    void send(const std::string& address, int value);
    void send(const std::string& address, const std::string& value);
    // resposta a uma pergunta "?" (para o IP:porta de quem perguntou, como o Resolume)
    void reply(const std::string& to, const std::string& address, const OscArg& value);

    bool inputRunning() const { return m_running.load(); }
    std::string lastError() const;
    // monitor (últimas mensagens, entrada e saída)
    std::vector<std::string> monitor() const;

    static std::vector<uint8_t> encode(const std::string& address, const std::vector<OscArg>& args);
    static bool decode(const uint8_t* data, size_t size, std::vector<OscMessage>& out);

private:
    void recvLoop(int port);
    void sendRaw(const std::vector<uint8_t>& pkt);
    void log(const std::string& line);

    std::thread m_thread;
    std::atomic<bool> m_running{false}, m_stop{false};
    uintptr_t m_recvSock = ~(uintptr_t)0, m_sendSock = ~(uintptr_t)0;
    mutable std::mutex m_mtx;
    std::deque<OscMessage> m_queue;
    std::deque<std::string> m_monitor;
    std::string m_error;
    int m_inPort = 0;
    bool m_inEnabled = false, m_outEnabled = false;
    std::string m_outAddr;
    int m_outPort = 0;
    bool m_wsa = false;
};

} // namespace ps
