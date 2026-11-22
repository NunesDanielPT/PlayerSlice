#include "core/Osc.h"
#include "core/Log.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>

namespace ps {

Osc& Osc::get() { static Osc o; return o; }

// ---------------------------------------------------------------------------
// codificação OSC 1.0
// ---------------------------------------------------------------------------
static void padString(std::vector<uint8_t>& b, const std::string& s) {
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
    while (b.size() % 4) b.push_back(0);
}
static void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((v >> 24) & 0xFF); b.push_back((v >> 16) & 0xFF); b.push_back((v >> 8) & 0xFF); b.push_back(v & 0xFF);
}
static uint32_t getU32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

std::vector<uint8_t> Osc::encode(const std::string& address, const std::vector<OscArg>& args) {
    std::vector<uint8_t> b;
    padString(b, address);
    std::string tags = ",";
    for (auto& a : args) tags += a.type;
    padString(b, tags);
    for (auto& a : args) {
        if (a.type == 'i') putU32(b, (uint32_t)a.i);
        else if (a.type == 'f') { uint32_t u; memcpy(&u, &a.f, 4); putU32(b, u); }
        else if (a.type == 's') padString(b, a.s);
    }
    return b;
}

static bool readString(const uint8_t* d, size_t n, size_t& pos, std::string& out) {
    size_t start = pos;
    while (pos < n && d[pos]) ++pos;
    if (pos >= n) return false;
    out.assign((const char*)d + start, pos - start);
    ++pos;
    while (pos % 4) ++pos;
    return pos <= n;
}

bool Osc::decode(const uint8_t* d, size_t n, std::vector<OscMessage>& out) {
    if (n < 4) return false;
    if (n >= 16 && memcmp(d, "#bundle", 8) == 0) {
        size_t pos = 16;                         // "#bundle\0" + timetag
        while (pos + 4 <= n) {
            uint32_t sz = getU32(d + pos); pos += 4;
            if (sz == 0 || pos + sz > n) return false;
            decode(d + pos, sz, out);
            pos += sz;
        }
        return true;
    }
    size_t pos = 0;
    OscMessage m;
    if (!readString(d, n, pos, m.address) || m.address.empty() || m.address[0] != '/') return false;
    std::string tags;
    if (pos < n && !readString(d, n, pos, tags)) return false;
    for (size_t t = 1; t < tags.size(); ++t) {
        OscArg a; a.type = tags[t];
        if (a.type == 'i') { if (pos + 4 > n) return false; a.i = (int32_t)getU32(d + pos); pos += 4; }
        else if (a.type == 'f') { if (pos + 4 > n) return false; uint32_t u = getU32(d + pos); memcpy(&a.f, &u, 4); pos += 4; }
        else if (a.type == 's') { if (!readString(d, n, pos, a.s)) return false; }
        else if (a.type == 'd') { if (pos + 8 > n) return false; uint64_t u = (uint64_t(getU32(d + pos)) << 32) | getU32(d + pos + 4); double v; memcpy(&v, &u, 8); a.type = 'f'; a.f = (float)v; pos += 8; }
        else if (a.type == 'h') { if (pos + 8 > n) return false; a.type = 'i'; a.i = (int32_t)getU32(d + pos + 4); pos += 8; }
        else if (a.type == 'T' || a.type == 'F' || a.type == 'N') {}
        else return false;
        m.args.push_back(a);
    }
    out.push_back(std::move(m));
    return true;
}

// ---------------------------------------------------------------------------
// rede
// ---------------------------------------------------------------------------
void Osc::log(const std::string& line) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_monitor.push_back(line);
    while (m_monitor.size() > 200) m_monitor.pop_front();
}

std::vector<std::string> Osc::monitor() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return std::vector<std::string>(m_monitor.begin(), m_monitor.end());
}

std::string Osc::lastError() const { std::lock_guard<std::mutex> lk(m_mtx); return m_error; }

void Osc::configure(bool inEnabled, int inPort, bool outEnabled, const std::string& outAddr, int outPort) {
    if (!m_wsa) { WSADATA w; m_wsa = WSAStartup(MAKEWORD(2, 2), &w) == 0; }
    // entrada
    bool restartIn = inEnabled != m_inEnabled || inPort != m_inPort || (inEnabled && !m_running.load());
    if (restartIn) {
        if (m_thread.joinable()) {
            m_stop = true;
            if (m_recvSock != ~(uintptr_t)0) closesocket((SOCKET)m_recvSock);
            m_thread.join();
            m_recvSock = ~(uintptr_t)0;
        }
        m_stop = false;
        m_inEnabled = inEnabled; m_inPort = inPort;
        { std::lock_guard<std::mutex> lk(m_mtx); m_error.clear(); }
        if (inEnabled) m_thread = std::thread([this, inPort] { recvLoop(inPort); });
    }
    // saída
    m_outEnabled = outEnabled; m_outAddr = outAddr; m_outPort = outPort;
    if (outEnabled && m_sendSock == ~(uintptr_t)0) m_sendSock = (uintptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

void Osc::shutdown() {
    m_stop = true;
    if (m_recvSock != ~(uintptr_t)0) closesocket((SOCKET)m_recvSock);
    if (m_thread.joinable()) m_thread.join();
    if (m_sendSock != ~(uintptr_t)0) closesocket((SOCKET)m_sendSock);
    m_recvSock = m_sendSock = ~(uintptr_t)0;
    if (m_wsa) WSACleanup();
    m_wsa = false;
}

void Osc::recvLoop(int port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { std::lock_guard<std::mutex> lk(m_mtx); m_error = "socket()"; return; }
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
        { std::lock_guard<std::mutex> lk(m_mtx); m_error = "Porta " + std::to_string(port) + " ocupada por outro programa"; }
        PS_ERROR("OSC: não foi possível abrir a porta %d", port);
        closesocket(s);
        return;
    }
    m_recvSock = (uintptr_t)s;
    m_running = true;
    PS_INFO("OSC: a ouvir na porta %d", port);
    std::vector<uint8_t> buf(65536);
    while (!m_stop.load()) {
        sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(s, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fl);
        if (n <= 0) { if (m_stop.load()) break; continue; }
        std::vector<OscMessage> msgs;
        if (!decode(buf.data(), (size_t)n, msgs)) continue;
        char ip[64] = ""; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        std::string src = std::string(ip) + ":" + std::to_string(ntohs(from.sin_port));
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto& m : msgs) {
            m.from = src;
            std::string line = "← " + m.address;
            for (auto& ar : m.args) line += ar.type == 's' ? " \"" + ar.s + "\"" : " " + std::to_string(ar.asFloat());
            m_monitor.push_back(line);
            if (m_queue.size() < 1000) m_queue.push_back(std::move(m));
        }
        while (m_monitor.size() > 200) m_monitor.pop_front();
    }
    m_running = false;
    closesocket(s);
}

std::vector<OscMessage> Osc::poll() {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<OscMessage> out(std::make_move_iterator(m_queue.begin()), std::make_move_iterator(m_queue.end()));
    m_queue.clear();
    return out;
}

void Osc::sendRaw(const std::vector<uint8_t>& pkt) {
    if (!m_outEnabled || m_sendSock == ~(uintptr_t)0 || m_outPort <= 0) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)m_outPort);
    if (inet_pton(AF_INET, m_outAddr.c_str(), &a.sin_addr) != 1) return;
    sendto((SOCKET)m_sendSock, (const char*)pkt.data(), (int)pkt.size(), 0, (sockaddr*)&a, sizeof(a));
}

void Osc::send(const std::string& address, float v) {
    OscArg a; a.type = 'f'; a.f = v;
    sendRaw(encode(address, {a}));
    if (m_outEnabled) log("→ " + address + " " + std::to_string(v));
}
void Osc::send(const std::string& address, int v) {
    OscArg a; a.type = 'i'; a.i = v;
    sendRaw(encode(address, {a}));
    if (m_outEnabled) log("→ " + address + " " + std::to_string(v));
}
void Osc::send(const std::string& address, const std::string& v) {
    OscArg a; a.type = 's'; a.s = v;
    sendRaw(encode(address, {a}));
    if (m_outEnabled) log("→ " + address + " \"" + v + "\"");
}

} // namespace ps
