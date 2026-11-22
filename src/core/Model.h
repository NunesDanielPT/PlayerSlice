#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include "core/Effects.h"

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
    FitMode     fit = FitMode::Native;   // entra no tamanho e proporção nativos
    // transformação dentro da região da layer (em píxeis da composição)
    float       offsetX = 0, offsetY = 0;
    float       scale = 1.0f;
    float       scaleX = 1.0f, scaleY = 1.0f;   // esticar (não uniforme)
    float       rotation = 0.0f;   // graus
    bool        flipH = false, flipV = false;
    std::vector<Effect> effects;
    bool empty() const { return path.empty(); }
};

struct Column {
    std::string name;
    uint32_t    color = 0;          // 0 = sem cor
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
    uint32_t    color = 0;          // cor da layer na grelha (0 = sem cor)
    bool        ignoreColumn = false; // não reage ao disparo de colunas
    // transformação da layer inteira
    float       posX = 0, posY = 0, scale = 1.0f, rotation = 0.0f;
    std::vector<Effect> effects;
    bool        useRegion = false;  // false = composição inteira
    Rect        region;             // em píxeis da composição
    std::vector<Clip> clips;        // uma por coluna
    int         activeColumn = -1;  // coluna no ar NA STACK VISÍVEL (não gravado)
    bool        playing = false;    // tem clip no ar (de qualquer stack)
};

// Stack: conjunto de colunas + clips (como os Decks do Resolume). As layers são partilhadas.
// A stack ativa vive em Composition::columns e Layer::clips; as outras ficam aqui guardadas.
struct Stack {
    std::string name;
    std::vector<Column> columns;
    std::map<uint32_t, std::vector<Clip>> clips;   // por ID de layer
};

struct Composition {
    std::string name = "Sem título";
    std::string description;
    int width = 1920;
    int height = 1080;
    float fps = 0.0f;               // 0 = automático
    int colorDepth = 8;             // 8 ou 16 bits por canal
    float master = 1.0f;            // opacidade geral
    std::vector<Effect> effects;
    std::vector<Column> columns;
    std::vector<Layer> layers;      // índice 0 = layer do topo (desenhada por último)
    std::vector<Stack> stacks{Stack{"Stack 1"}};
    int activeStack = 0;
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
    bool        locked = false;     // trancada no editor (não se mexe sem querer)
    // warp de 4 pontos (perspetiva). Cantos: 0 topo-esq, 1 topo-dir, 2 baixo-dir, 3 baixo-esq
    bool        inWarp = false;     // entrada: quadrilátero livre na composição
    float       inPts[4][2] = {};
    bool        outWarp = false;    // saída: quadrilátero livre no ecrã (encaixar em paredes)
    float       outPts[4][2] = {};

    void inputQuad(float q[4][2]) const;   // cantos da entrada (com espelhos aplicados)
    void outputQuad(float q[4][2]) const;  // cantos da saída (rotação ou pontos)
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
    float       opacity = 1.0f;
    float       contrast = 0.0f;    // -1..1
    float       red = 0, green = 0, blue = 0; // -1..1 (ganho por canal)
    std::string guidePath;          // Output Guide: imagem de referência no editor
    float       guideOpacity = 0.5f;
    bool        testPattern = false;
    bool        inToggle = true;    // incluído no atalho ligar/desligar saídas
    bool        locked = false;     // trancado no editor (slices não se mexem)
    std::vector<Slice> slices;
};

struct Project {
    Composition comp;
    std::vector<Screen> screens;
    uint32_t nextId = 1;

    uint32_t newId() { return nextId++; }

    Layer&  addLayer(const std::string& name);
    // stacks
    void storeActiveStack();            // copia colunas/clips em uso para stacks[activeStack]
    void switchStack(int i);
    int  addStack(const std::string& name, bool duplicateActive = false);
    void removeStack(int i);
    void moveStack(int from, int to);
    Layer&  insertLayer(int index, const std::string& name);
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
