#include "gfx/Displays.h"
#include "core/Log.h"
#include "core/I18n.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <map>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace ps {

std::string DisplayInfo::label() const {
    char buf[512];
    std::string k = key;
    auto p = k.find("DISPLAY");
    std::string num = p != std::string::npos ? k.substr(p + 7) : k;
    snprintf(buf, sizeof(buf), "%s: %s  %dx%d @%dHz%s", num.c_str(),
             friendly.empty() ? T("Monitor") : friendly.c_str(), width(), height(), refreshHz,
             primary ? T("  (principal)") : "");
    return buf;
}

// nomes "amigáveis" via DisplayConfig (o que aparece nas Definições do Windows)
static std::map<std::wstring, std::wstring> friendlyNames() {
    std::map<std::wstring, std::wstring> out;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return out;
    for (UINT32 i = 0; i < np; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = paths[i].targetInfo.adapterId;
        tgt.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;
        out[src.viewGdiDeviceName] = tgt.monitorFriendlyDeviceName;
    }
    return out;
}

std::vector<DisplayInfo> enumerateDisplays() {
    std::vector<DisplayInfo> list;
    auto names = friendlyNames();
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return list;
    ComPtr<IDXGIAdapter1> a;
    for (UINT ai = 0; f->EnumAdapters1(ai, &a) != DXGI_ERROR_NOT_FOUND; ++ai) {
        DXGI_ADAPTER_DESC1 ad; a->GetDesc1(&ad);
        ComPtr<IDXGIOutput> o;
        for (UINT oi = 0; a->EnumOutputs(oi, &o) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC od; o->GetDesc(&od);
            if (!od.AttachedToDesktop) continue;
            DisplayInfo d;
            d.key = toUtf8(od.DeviceName);
            d.adapter = toUtf8(ad.Description);
            d.rect = od.DesktopCoordinates;
            d.monitor = od.Monitor;
            auto it = names.find(od.DeviceName);
            if (it != names.end()) d.friendly = toUtf8(it->second);
            MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(od.Monitor, &mi)) d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            DEVMODEW dm{}; dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) d.refreshHz = (int)dm.dmDisplayFrequency;
            list.push_back(d);
        }
    }
    return list;
}

static std::wstring wkey(const std::string& k) { return std::wstring(k.begin(), k.end()); }

int currentRefresh(const std::string& key) {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    std::wstring dev = wkey(key);
    if (!EnumDisplaySettingsW(dev.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return (int)dm.dmDisplayFrequency;
}

int setDisplayRefresh(const std::string& key, float fps, std::string* msg) {
    if (fps <= 1.f) return 0;
    std::wstring dev = wkey(key);
    DEVMODEW cur{}; cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW(dev.c_str(), ENUM_CURRENT_SETTINGS, &cur)) return 0;
    // 23.976 / 29.97 / 59.94 aparecem no Windows como 23 / 29 / 59
    const bool ntsc = std::fabs(fps - std::round(fps)) > 0.01f;
    const int base = ntsc ? (int)std::floor(fps) : (int)std::lround(fps);
    // candidatos por ordem: exato, depois múltiplos (25 → 50 → 75 → 100, 24 → 48 ...)
    std::vector<int> want;
    for (int m = 1; m <= 5; ++m) {
        int hz = base * m;
        if (ntsc && m > 1) hz = (int)std::floor(fps * m);
        if (hz > 0 && hz <= 500) want.push_back(hz);
    }
    if ((int)cur.dmDisplayFrequency == want[0]) return want[0];
    std::vector<DEVMODEW> modes;
    for (DWORD i = 0;; ++i) {
        DEVMODEW m{}; m.dmSize = sizeof(m);
        if (!EnumDisplaySettingsW(dev.c_str(), i, &m)) break;
        if (m.dmPelsWidth != cur.dmPelsWidth || m.dmPelsHeight != cur.dmPelsHeight) continue;
        if (m.dmDisplayFlags & DM_INTERLACED) continue;
        modes.push_back(m);
    }
    for (int hz : want) {
        if ((int)cur.dmDisplayFrequency == hz) return hz;   // já está num múltiplo compatível
        for (auto& m : modes) {
            if ((int)m.dmDisplayFrequency != hz) continue;
            DEVMODEW set = cur;
            set.dmDisplayFrequency = hz;
            set.dmFields = DM_DISPLAYFREQUENCY;
            LONG r = ChangeDisplaySettingsExW(dev.c_str(), &set, nullptr, CDS_UPDATEREGISTRY, nullptr);
            if (r == DISP_CHANGE_SUCCESSFUL) {
                PS_INFO("%s: %d Hz -> %d Hz (projeto %.3f fps)", key.c_str(), (int)cur.dmDisplayFrequency, hz, fps);
                if (msg) *msg = std::to_string(hz) + " Hz";
                return hz;
            }
            PS_WARN("%s: não foi possível mudar para %d Hz (código %ld)", key.c_str(), hz, r);
        }
    }
    PS_WARN("%s: o monitor não tem um modo compatível com %.3f fps a %lux%lu (fica a %lu Hz)", key.c_str(), fps,
            cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency);
    return 0;
}

} // namespace ps
