#pragma once
#include <string>
#include <vector>
#include <cstdint>

// ============================================================================
// Modelo de dados do PLAYERSLICE
//
//   Composição (ex. 7680x2160)
//     └─ Layers (linhas)  × Colunas  →  células com Clips
//          cada Layer tem uma "Região" na composição onde os seus vídeos são
//          encaixados (ex. a zona do ledwall CENTER T, 720x648)
//
//   Ecrãs (= saídas da gráfica / monitores / processadores LED)
//     └─ Slices: recortam um retângulo da composição (ou de uma Layer só)
//                e colocam-no numa posição/tamanho/rotação da saída.
// ============================================================================

namespace ps {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Rect&) const = default;
};

enum class FitMode : int { Stretch = 0, Fit, Fill, Native };
enum class BlendMode : int { Normal = 0, Add, Multiply, Screen };
enum class LoopMode : int { Loop = 0, PlayOnceHold, PlayOnceClear };

struct Clip {
    uint32_t    id = 0;
    std::string path;          // UTF-8
    std::string name;
    LoopMode    loop = LoopMode::Loop;
    float       speed = 1.0f;
    float       opacity = 1.0f;
    FitMode     fit = FitMode::Stretch;
    // ajuste fino dentro da região da layer (em píxeis da composição)
    float       offsetX = 0, offsetY = 0;
    float       scale = 1.0f;
    bool empty() const { return path.empty(); }
};

struct Layer {
    uint32_t    id = 0;
    std::string name;
    float       opacity = 1.0f;
    BlendMode   blend = BlendMode::Normal;
    bool        bypass = false;     // B — escondida
    bool        solo = false;       // S — só esta
    bool        locked = false;
    float       fadeTime = 0.0f;    // crossfade entre clips (s)
    bool        useRegion = false;  // false = composição inteira
    Rect        region;             // em píxeis da composição
    std::vector<Clip> clips;        // uma por coluna
    int         activeColumn = -1;  // estado em execução (não gravado)
};

struct Composition {
    int width = 1920;
    int height = 1080;
    float fps = 50.0f;
    std::vector<std::string> columns;
    std::vector<Layer> layers;      // índice 0 = layer do topo (desenhada por último)
};

struct Slice {
    uint32_t    id = 0;
    std::string name;
    bool        enabled = true;
    uint32_t    sourceLayer = 0;    // 0 = composição, senão ID da layer (só essa layer)
    Rect        input;              // recorte (px da composição)
    // saída: centro, tamanho e rotação (px do ecrã)
    float       outX = 0, outY = 0, outW = 0, outH = 0;
    float       rotation = 0.0f;    // graus
    bool        flipH = false, flipV = false;
    float       opacity = 1.0f;
    float       brightness = 0.0f;  // -1..1
    uint32_t    color = 0;          // cor no editor/test pattern (RGBA)
};

enum class ScreenDevice : int { Disabled = 0, Window = 1, Monitor = 2 };

struct Screen {
    uint32_t    id = 0;
    std::string name;
    ScreenDevice device = ScreenDevice::Disabled;
    std::string monitorKey;         // ex. \\.\DISPLAY2 — identifica a saída da gráfica
    int         width = 1920;
    int         height = 1080;
    float       brightness = 0.0f;
    bool        testPattern = false;
    std::vector<Slice> slices;
};

struct Project {
    Composition comp;
    std::vector<Screen> screens;
    uint32_t nextId = 1;

    uint32_t newId() { return nextId++; }

    Layer&  addLayer(const std::string& name);
    void    ensureColumns(int n);
    Screen& addScreen(const std::string& name);
    Slice&  addSlice(Screen& s, const std::string& name);

    Screen* findScreen(uint32_t id);
    Slice*  findSlice(uint32_t id, Screen** owner = nullptr);
    int     layerIndex(uint32_t layerId) const;

    static Project makeDefault();

    std::string toJson() const;
    bool fromJson(const std::string& text, std::string* err);
};

uint32_t paletteColor(int i);

} // namespace ps
