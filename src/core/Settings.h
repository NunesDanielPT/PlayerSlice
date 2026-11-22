#pragma once
#include <string>
#include <vector>

// Definições da aplicação (não do projeto): %LOCALAPPDATA%\PLAYERSLICE\settings.json
namespace ps {

enum class Action : int {
    ToggleOutputs = 0,   // ligar/desligar saídas marcadas
    OutputsOn,
    OutputsOff,
    StopAll,
    TestPattern,
    Save,
    SaveAs,
    Open,
    Undo,
    Redo,
    Settings,
    OutputEditor,
    TakePreview,
    ClearPreview,
    Column1, Column2, Column3, Column4, Column5, Column6, Column7, Column8, Column9,
    PrevStack, NextStack, NewStack,
    CueGo, CueBack,
    Count
};

struct KeyBinding {
    int  key = 0;        // ImGuiKey (0 = sem atalho)
    bool ctrl = false, shift = false, alt = false;
    bool operator==(const KeyBinding&) const = default;
};

struct Settings {
    std::string language = "pt";
    std::string gpu;             // descrição da placa escolhida ("" = automática, a mais potente)
    std::string network;         // nome do adaptador de rede (NDI / OSC / Art-Net)
    std::string audioDevice;     // ID do dispositivo de som ("" = predefinido do Windows)
    std::string audioName;
    bool        outputsOffBlack = true;   // ao desligar: true = preto, false = fechar janela
    bool        startOutputsLive = false; // ligar saídas ao abrir um projeto
    bool        lockMouseOutputs = true;  // o rato não entra nos ecrãs que estão a ser usados como saída
    bool        checkUpdates = true;      // procurar atualizações ao abrir
    // performance
    int         priority = 2;             // 0 normal, 1 acima do normal, 2 alta
    bool        preventSleep = true;      // impedir suspensão / ecrãs a desligar
    bool        hwDecode = true;          // descodificação H.264/H.265 pela placa gráfica
    int         decodeBuffer = 6;         // frames em fila por clip (3..16)
    bool        matchOutputHz = true;     // ao ligar uma saída, pôr o monitor à frequência dos fps do projeto
    bool        limitUiWhenLive = true;   // interface a 30 fps com saídas no ar (mais GPU para as saídas)
    // OSC (Companion, TouchOSC...)
    bool        oscIn = false;
    int         oscInPort = 7000;
    bool        oscOut = false;
    std::string oscOutAddr = "127.0.0.1";
    int         oscOutPort = 7001;
    // link na rede (página Clip Time)
    bool        webEnabled = false;
    int         webPort = 8090;
    // timecode SMPTE
    int         tcSource = 0;             // 0 desligado, 1 LTC (áudio), 2 MTC (MIDI), 3 gerador interno
    std::string tcAudioId;                // entrada de áudio (LTC)
    std::string tcAudioName;
    int         tcChannel = 0;            // 0 esquerdo, 1 direito
    std::string tcMidi;                   // entrada MIDI (MTC)
    float       tcFps = 0;                // 0 = automático
    double      tcOffset = 0;             // segundos
    int         tcFreewheel = 10;         // frames
    KeyBinding  keys[(int)Action::Count];

    void resetKeys();
    bool load();                 // devolve false se ainda não existia
    bool save() const;
};

Settings& settings();
const char* actionName(Action a);           // texto PT (passar por T())
std::string bindingLabel(const KeyBinding& k);

// listas do sistema
struct GpuInfo  { std::string name; size_t vramMB = 0; bool software = false; };
struct NetInfo  { std::string name, description, ipv4; bool up = false; };
struct AudioInfo{ std::string id, name; bool isDefault = false; };
std::vector<GpuInfo>   listGpus();
std::vector<NetInfo>   listNetworkAdapters();
std::vector<AudioInfo> listAudioOutputs();

} // namespace ps
