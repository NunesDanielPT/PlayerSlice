#include "core/WebServer.h"
#include "core/Log.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>

namespace ps {

WebServer& WebServer::get() { static WebServer w; return w; }

std::string WebServer::lastError() const { std::lock_guard<std::mutex> lk(m_mtx); return m_error; }

void WebServer::setClipTimeJson(std::string json) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_json = std::move(json);
}

void WebServer::configure(bool enabled, int port) {
    if (enabled == m_enabled && port == m_port && (!enabled || m_running.load())) return;
    shutdown();
    m_enabled = enabled; m_port = port;
    if (!enabled) return;
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    m_stop = false;
    m_thread = std::thread([this, port] { loop(port); });
}

void WebServer::shutdown() {
    m_stop = true;
    if (m_sock != ~(uintptr_t)0) closesocket((SOCKET)m_sock);
    if (m_thread.joinable()) m_thread.join();
    m_sock = ~(uintptr_t)0;
    m_running = false;
}

// página Clip Time (autónoma, sem dependências externas)
static const char* kPage = R"HTML(<!DOCTYPE html>
<html lang="pt"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>PLAYERSLICE · Clip Time</title>
<style>
 *{box-sizing:border-box}
 body{margin:0;background:#151619;color:#e6e8eb;font-family:Segoe UI,Roboto,Arial,sans-serif}
 header{display:flex;justify-content:space-between;align-items:center;padding:10px 18px;background:#1c1d20;border-bottom:1px solid #2e3035}
 header b{color:#40e0c0;letter-spacing:.5px} #st{font-size:13px;color:#888}
 #grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(min(100%,340px),1fr));gap:14px;padding:16px}
 .it{background:#1c1d20;border:1px solid #2a2c31;border-radius:8px;padding:12px 14px 14px;container-type:inline-size;min-width:0;overflow:hidden}
 .lb{font-size:14px;color:#cfd2d6;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-bottom:4px}
 .tc{font-family:Consolas,"Cascadia Mono",Menlo,monospace;font-size:min(13cqw,88px);line-height:1.1;white-space:nowrap;font-variant-numeric:tabular-nums}
 .live{border-color:#4a6a2a} .live .lb,.live .tc{color:#b8f060}
 .warn .tc{color:#ffb040} .end .tc{color:#ff5050} .end{border-color:#6a2a2a}
 .bar{height:4px;background:#2e3035;margin-top:8px;border-radius:2px;overflow:hidden}.bar i{display:block;height:4px;background:#e6e8eb;width:0}
 .live .bar i{background:#b8f060} .warn .bar i{background:#ffb040} .end .bar i{background:#ff5050}
</style></head><body>
<header><b>PLAYERSLICE · Clip Time</b><span id="st">a ligar…</span></header>
<div id="grid"></div>
<script>
const g=document.getElementById('grid'),st=document.getElementById('st');
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
async function tick(){
 try{
  const r=await fetch('cliptime.json',{cache:'no-store'});const d=await r.json();
  let h='';for(const it of d.items){
   const cls=(it.live?'live ':'')+(it.remaining_s!=null&&it.remaining_s<5&&it.live?'end':(it.remaining_s!=null&&it.remaining_s<15&&it.live?'warn':''));
   h+=`<div class="it ${cls}"><div class="lb">${esc(it.label)}</div><div class="tc">${esc(it.time)}</div>`+
      (it.progress!=null?`<div class="bar"><i style="width:${Math.round(it.progress*100)}%"></i></div>`:'')+`</div>`;}
  g.innerHTML=h;st.textContent=d.project||'';
 }catch(e){st.textContent='sem ligação ao PLAYERSLICE';}
 setTimeout(tick,100);
}
tick();
</script></body></html>)HTML";

void WebServer::handle(uintptr_t c) {
    SOCKET s = (SOCKET)c;
    DWORD to = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    char buf[4096];
    int n = recv(s, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { closesocket(s); return; }
    buf[n] = 0;
    std::string req(buf);
    std::string path = "/";
    if (req.rfind("GET ", 0) == 0) {
        size_t e = req.find(' ', 4);
        path = req.substr(4, e == std::string::npos ? std::string::npos : e - 4);
    }
    std::string body, type = "text/html; charset=utf-8", status = "200 OK";
    if (path == "/" || path == "/cliptime" || path.rfind("/?", 0) == 0) body = kPage;
    else if (path.rfind("/cliptime.json", 0) == 0) { std::lock_guard<std::mutex> lk(m_mtx); body = m_json; type = "application/json; charset=utf-8"; }
    else { status = "404 Not Found"; body = "404"; type = "text/plain"; }
    std::string hdr = "HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                      "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
    send(s, hdr.data(), (int)hdr.size(), 0);
    send(s, body.data(), (int)body.size(), 0);
    closesocket(s);
}

void WebServer::loop(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return;
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 16) != 0) {
        { std::lock_guard<std::mutex> lk(m_mtx); m_error = "Porta " + std::to_string(port) + " ocupada"; }
        PS_ERROR("Servidor web: não foi possível abrir a porta %d", port);
        closesocket(s);
        return;
    }
    { std::lock_guard<std::mutex> lk(m_mtx); m_error.clear(); }
    m_sock = (uintptr_t)s;
    m_running = true;
    PS_INFO("Servidor web (Clip Time) na porta %d", port);
    while (!m_stop.load()) {
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{0, 200000};
        int r = select(0, &rd, nullptr, nullptr, &tv);
        if (r <= 0) continue;
        SOCKET c = accept(s, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        try { handle((uintptr_t)c); } catch (...) { closesocket(c); }
    }
    m_running = false;
    closesocket(s);
}

} // namespace ps
