#pragma once
#include <string>
#include <vector>

// Efeitos de vídeo (num só passo de GPU, sem custo de render targets extra).
// Podem estar no clip, na layer e na composição. Cada tipo tem parâmetros fixos.
namespace ps {

struct Effect {
    std::string type;          // id do catálogo (ex. "color")
    bool enabled = true;
    std::vector<float> p;      // valores pela ordem de EffectDef::params
};

struct ParamDef {
    const char* name;          // texto PT (passar por T())
    float min, max, def;
    bool toggle = false;       // mostrado como caixa
    bool color = false;        // 3 valores seguidos = cor RGB
};

struct EffectDef {
    const char* id;
    const char* name;          // texto PT
    const char* help;          // texto PT
    std::vector<ParamDef> params;
};

const std::vector<EffectDef>& effectCatalog();
const EffectDef* findEffect(const std::string& id);
Effect makeEffect(const std::string& id);

// parâmetros agregados para o shader (4 x float4)
struct FxParams {
    float v[16];
    float opacityMul = 1.f;    // strobe
    bool  identity = true;
    FxParams();
};
FxParams aggregateFx(const std::vector<Effect>& fx, double now);

} // namespace ps
