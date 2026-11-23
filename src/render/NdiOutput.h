#pragma once
// Saídas NDI: cada ecrã com dispositivo NDI é enviado pela rede como uma fonte NDI.
// Usa o NDI Runtime instalado no PC (Processing.NDI.Lib.x64.dll), carregado só quando é preciso.
#include "core/Model.h"
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <d3d11.h>
#include <wrl/client.h>

namespace ps {

class Compositor;

class NdiOutputs {
public:
    static NdiOutputs& get();
    using LiveFn = std::function<bool(uint32_t)>;
    // chamar uma vez por frame, depois de comp.render()
    void present(const Project& p, const Compositor& comp, const LiveFn& live, float fps);
    void shutdown();
    bool runtimeAvailable();              // tenta carregar a DLL (uma vez)
    const std::string& runtimeError() const { return m_err; }
    int  senders() const { return (int)m_send.size(); }

private:
    struct Sender {
        void* inst = nullptr;
        std::string name;
        int w = 0, h = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging[3];
        int frame = 0;
        std::vector<uint8_t> buf[2];
        int bufIdx = 0;
    };
    bool load();
    void destroy(Sender& s);

    bool m_tried = false, m_ok = false;
    std::string m_err;
    void* m_dll = nullptr;
    std::map<uint32_t, Sender> m_send;   // por Screen::id
};

} // namespace ps
