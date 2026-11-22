#include "gfx/Displays.h"
#include "core/Log.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <map>

using Microsoft::WRL::ComPtr;

namespace ps {

std::string DisplayInfo::label() const {
    char buf[512];
    std::string k = key;
    auto p = k.find("DISPLAY");
    std::string num = p != std::string::npos ? k.substr(p + 7) : k;
    snprintf(buf, sizeof(buf), "%s: %s  %dx%d @%dHz%s", num.c_str(),
             friendly.empty() ? "Monitor" : friendly.c_str(), width(), height(), refreshHz,
             primary ? "  (principal)" : "");
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

} // namespace ps
