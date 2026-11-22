#pragma once
#include "core/Model.h"
#include "gfx/Gfx.h"
#include "gfx/Displays.h"
#include <functional>
#include <map>
#include <vector>

namespace ps {

class Compositor;

// Janelas de saída: uma por Ecrã ativo. Em modo "Monitor" a janela ocupa a
// saída inteira da gráfica (sem bordas, por cima de tudo, sem cursor).
class OutputWindows {
public:
    using LiveFn = std::function<bool(uint32_t screenId)>;
    // offBlack: saídas desligadas mantêm a janela a preto (em vez de fechar)
    void sync(const Project& p, const std::vector<DisplayInfo>& displays, const LiveFn& live, bool offBlack);
    void present(const Compositor& comp, const LiveFn& live, bool& didVsync);
    void closeAll();
    int  count() const { return (int)m_wins.size(); }
    bool coversMonitor(HMONITOR m) const;

    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);

private:
    struct Win {
        HWND hwnd = nullptr;
        ComPtr<IDXGISwapChain1> swap;
        ComPtr<ID3D11RenderTargetView> rtv;
        int w = 0, h = 0;
        ScreenDevice device = ScreenDevice::Disabled;
        std::string monitorKey;
        HMONITOR monitor = nullptr;
        RECT rect{};
        bool needResize = false;
    };
    bool create(Win& w, const Screen& s, const DisplayInfo* d);
    void destroy(Win& w);
    bool resize(Win& w);

    std::map<uint32_t, Win> m_wins;   // por Screen::id
};

} // namespace ps
