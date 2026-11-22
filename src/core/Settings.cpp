#include "core/Settings.h"
#include "core/Log.h"
#include <nlohmann/json.hpp>
#include <imgui.h>
#include <windows.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <mmdeviceapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

using nlohmann::json;
using Microsoft::WRL::ComPtr;

namespace ps {

static Settings g_settings;
Settings& settings() { return g_settings; }

static const char* kActionNames[] = {
    "Ligar/desligar saídas marcadas",
    "Ligar todas as saídas",
    "Desligar todas as saídas",
    "Parar todos os clips",
    "Test pattern (ecrã selecionado)",
    "Gravar",
    "Gravar como",
    "Abrir projeto",
    "Desfazer",
    "Refazer",
    "Definições",
    "Editor de saídas e slices",
    "Enviar preview para programa",
    "Limpar preview",
    "Disparar coluna 1", "Disparar coluna 2", "Disparar coluna 3", "Disparar coluna 4", "Disparar coluna 5",
    "Disparar coluna 6", "Disparar coluna 7", "Disparar coluna 8", "Disparar coluna 9",
};
static_assert(sizeof(kActionNames) / sizeof(kActionNames[0]) == (int)Action::Count, "nomes das ações");

// chave estável no settings.json
static const char* kActionIds[] = {
    "toggleOutputs", "outputsOn", "outputsOff", "stopAll", "testPattern", "save", "saveAs", "open",
    "undo", "redo", "settings", "outputEditor", "takePreview", "clearPreview",
    "column1", "column2", "column3", "column4", "column5", "column6", "column7", "column8", "column9",
};
static_assert(sizeof(kActionIds) / sizeof(kActionIds[0]) == (int)Action::Count, "ids das ações");

const char* actionName(Action a) { return kActionNames[(int)a]; }

void Settings::resetKeys() {
    auto k = [](int key, bool c = false, bool s = false, bool a = false) { KeyBinding b; b.key = key; b.ctrl = c; b.shift = s; b.alt = a; return b; };
    keys[(int)Action::ToggleOutputs] = k(ImGuiKey_F12);
    keys[(int)Action::OutputsOn]     = k(ImGuiKey_F12, true);
    keys[(int)Action::OutputsOff]    = k(ImGuiKey_F12, false, true);
    keys[(int)Action::StopAll]       = k(ImGuiKey_Escape, false, true);
    keys[(int)Action::TestPattern]   = k(ImGuiKey_T, true);
    keys[(int)Action::Save]          = k(ImGuiKey_S, true);
    keys[(int)Action::SaveAs]        = k(ImGuiKey_S, true, true);
    keys[(int)Action::Open]          = k(ImGuiKey_O, true);
    keys[(int)Action::Undo]          = k(ImGuiKey_Z, true);
    keys[(int)Action::Redo]          = k(ImGuiKey_Y, true);
    keys[(int)Action::Settings]      = k(ImGuiKey_Comma, true);
    keys[(int)Action::OutputEditor]  = k(ImGuiKey_E, true);
    keys[(int)Action::TakePreview]   = k(ImGuiKey_Enter);
    keys[(int)Action::ClearPreview]  = k(ImGuiKey_Backspace, true);
    for (int i = 0; i < 9; ++i) keys[(int)Action::Column1 + i] = k(ImGuiKey_F1 + i);
}

std::string bindingLabel(const KeyBinding& k) {
    if (!k.key) return "—";
    std::string s;
    if (k.ctrl) s += "Ctrl+";
    if (k.shift) s += "Shift+";
    if (k.alt) s += "Alt+";
    const char* n = ImGui::GetKeyName((ImGuiKey)k.key);
    s += n ? n : "?";
    return s;
}

static std::filesystem::path settingsPath() { return std::filesystem::path(appDataDir() + L"\\settings.json"); }

// idioma escolhido no instalador: <pasta do exe>\playerslice.ini
static std::string installerLanguage() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path ini = std::filesystem::path(exe).parent_path() / L"playerslice.ini";
    wchar_t buf[32] = L"";
    GetPrivateProfileStringW(L"General", L"Language", L"", buf, 32, ini.c_str());
    return toUtf8(buf);
}

bool Settings::load() {
    resetKeys();
    std::ifstream f(settingsPath(), std::ios::binary);
    if (!f) {
        std::string il = installerLanguage();
        if (!il.empty()) language = il;
        return false;
    }
    try {
        std::stringstream ss; ss << f.rdbuf();
        json j = json::parse(ss.str());
        language = j.value("language", language);
        gpu = j.value("gpu", gpu);
        network = j.value("network", network);
        audioDevice = j.value("audioDevice", audioDevice);
        audioName = j.value("audioName", audioName);
        outputsOffBlack = j.value("outputsOffBlack", outputsOffBlack);
        startOutputsLive = j.value("startOutputsLive", startOutputsLive);
        checkUpdates = j.value("checkUpdates", checkUpdates);
        priority = j.value("priority", priority);
        preventSleep = j.value("preventSleep", preventSleep);
        hwDecode = j.value("hwDecode", hwDecode);
        decodeBuffer = std::clamp(j.value("decodeBuffer", decodeBuffer), 3, 16);
        limitUiWhenLive = j.value("limitUiWhenLive", limitUiWhenLive);
        if (j.contains("keys")) {
            for (int i = 0; i < (int)Action::Count; ++i) {
                auto it = j["keys"].find(kActionIds[i]);
                if (it == j["keys"].end()) continue;
                KeyBinding b;
                b.key = it->value("key", 0); b.ctrl = it->value("ctrl", false);
                b.shift = it->value("shift", false); b.alt = it->value("alt", false);
                keys[i] = b;
            }
        }
    } catch (const std::exception& e) {
        PS_WARN("settings.json inválido (%s) — a usar predefinições", e.what());
    }
    return true;
}

bool Settings::save() const {
    json j;
    j["language"] = language; j["gpu"] = gpu; j["network"] = network;
    j["audioDevice"] = audioDevice; j["audioName"] = audioName;
    j["outputsOffBlack"] = outputsOffBlack; j["startOutputsLive"] = startOutputsLive; j["checkUpdates"] = checkUpdates;
    j["priority"] = priority; j["preventSleep"] = preventSleep; j["hwDecode"] = hwDecode;
    j["decodeBuffer"] = decodeBuffer; j["limitUiWhenLive"] = limitUiWhenLive;
    for (int i = 0; i < (int)Action::Count; ++i) {
        const KeyBinding& b = keys[i];
        j["keys"][kActionIds[i]] = {{"key", b.key}, {"ctrl", b.ctrl}, {"shift", b.shift}, {"alt", b.alt}};
    }
    std::ofstream f(settingsPath(), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(2);
    return (bool)f;
}

// ---------------------------------------------------------------------------
// listas do sistema
// ---------------------------------------------------------------------------
std::vector<GpuInfo> listGpus() {
    std::vector<GpuInfo> out;
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return out;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        GpuInfo g;
        g.name = toUtf8(d.Description);
        g.vramMB = d.DedicatedVideoMemory / (1024 * 1024);
        g.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        bool dup = false;
        for (auto& x : out) if (x.name == g.name) dup = true;
        if (!dup) out.push_back(g);
    }
    return out;
}

std::vector<NetInfo> listNetworkAdapters() {
    std::vector<NetInfo> out;
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, (PIP_ADAPTER_ADDRESSES)buf.data(), &size);
    if (r == ERROR_BUFFER_OVERFLOW) { buf.resize(size); r = GetAdaptersAddresses(AF_INET, flags, nullptr, (PIP_ADAPTER_ADDRESSES)buf.data(), &size); }
    if (r != NO_ERROR) return out;
    for (auto* a = (PIP_ADAPTER_ADDRESSES)buf.data(); a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        NetInfo n;
        n.name = toUtf8(a->FriendlyName);
        n.description = toUtf8(a->Description);
        n.up = a->OperStatus == IfOperStatusUp;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            char ip[64] = "";
            auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            n.ipv4 = ip;
            break;
        }
        out.push_back(n);
    }
    return out;
}

std::vector<AudioInfo> listAudioOutputs() {
    std::vector<AudioInfo> out;
    // {a45c254e-df1c-4efd-8020-67d146a850e0}, 14 = PKEY_Device_FriendlyName
    static const PROPERTYKEY kFriendly = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return out;
    std::wstring defId;
    ComPtr<IMMDevice> def;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) { defId = id; CoTaskMemFree(id); }
    }
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(col->Item(i, &d))) continue;
        AudioInfo ai;
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id))) { ai.id = toUtf8(id); ai.isDefault = defId == id; CoTaskMemFree(id); }
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kFriendly, &v)) && v.vt == VT_LPWSTR) ai.name = toUtf8(v.pwszVal);
            PropVariantClear(&v);
        }
        out.push_back(ai);
    }
    return out;
}

} // namespace ps
