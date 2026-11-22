#include "core/Effects.h"
#include <algorithm>
#include <cmath>

namespace ps {

// Layout de FxParams::v (igual ao shader):
//  0 brilho   1 contraste   2 saturação   3 matiz (rad)
//  4 tint r   5 tint g      6 tint b      7 quantidade tint
//  8 inverter 9 limiar(-1)  10 vinheta    11 separação RGB (px)
// 12 espelho H 13 espelho V 14 pixelizar (px) 15 livre
FxParams::FxParams() {
    for (float& x : v) x = 0;
    v[2] = 1;      // saturação neutra
    v[4] = v[5] = v[6] = 1;
    v[9] = -1;     // limiar desligado
}

const std::vector<EffectDef>& effectCatalog() {
    static const std::vector<EffectDef> cat = {
        {"color", "Cor", "Brilho, contraste, saturação e matiz.",
         {{"Brilho", -1, 1, 0}, {"Contraste", -1, 1, 0}, {"Saturação", 0, 2, 1}, {"Matiz", -180, 180, 0}}},
        {"invert", "Inverter", "Inverte as cores (negativo).", {{"Quantidade", 0, 1, 1}}},
        {"tint", "Tingir", "Pinta a imagem com uma cor, mantendo a luminosidade.",
         {{"Cor", 0, 1, 1, false, true}, {"G", 0, 1, 0.4f}, {"B", 0, 1, 0.1f}, {"Quantidade", 0, 1, 1}}},
        {"bw", "Preto e branco", "Remove a cor.", {{"Quantidade", 0, 1, 1}}},
        {"threshold", "Limiar", "Só preto e branco puros, a partir de um nível de luz.", {{"Nível", 0, 1, 0.5f}}},
        {"mirror", "Espelho", "Espelha metade da imagem para o outro lado.",
         {{"Horizontal", 0, 1, 1, true}, {"Vertical", 0, 1, 0, true}}},
        {"pixelate", "Pixelizar", "Blocos grandes de píxeis (mosaico).", {{"Tamanho (px)", 1, 128, 16}}},
        {"rgbsplit", "Separação RGB", "Desloca os canais vermelho e azul (efeito glitch).", {{"Distância (px)", 0, 60, 8}}},
        {"vignette", "Vinheta", "Escurece os cantos.", {{"Quantidade", 0, 1, 0.6f}}},
        {"strobe", "Strobe", "Pisca a imagem ligada/desligada.", {{"Velocidade (Hz)", 0.5f, 30, 8}, {"Ligado (%)", 5, 95, 50}}},
    };
    return cat;
}

const EffectDef* findEffect(const std::string& id) {
    for (auto& d : effectCatalog()) if (id == d.id) return &d;
    return nullptr;
}

Effect makeEffect(const std::string& id) {
    Effect e;
    e.type = id;
    if (const EffectDef* d = findEffect(id)) for (auto& p : d->params) e.p.push_back(p.def);
    return e;
}

FxParams aggregateFx(const std::vector<Effect>& fx, double now) {
    FxParams r;
    for (const Effect& e : fx) {
        if (!e.enabled) continue;
        const EffectDef* d = findEffect(e.type);
        if (!d || e.p.size() < d->params.size()) continue;
        auto P = [&](int i) { return e.p[i]; };
        r.identity = false;
        if (e.type == "color") {
            r.v[0] += P(0); r.v[1] += P(1); r.v[2] *= P(2); r.v[3] += P(3) * 3.14159265f / 180.f;
        } else if (e.type == "invert") {
            r.v[8] = std::max(r.v[8], P(0));
        } else if (e.type == "tint") {
            r.v[4] = P(0); r.v[5] = P(1); r.v[6] = P(2); r.v[7] = std::max(r.v[7], P(3));
        } else if (e.type == "bw") {
            r.v[2] *= 1.f - P(0);
        } else if (e.type == "threshold") {
            r.v[9] = P(0);
        } else if (e.type == "mirror") {
            if (P(0) > 0.5f) r.v[12] = 1;
            if (P(1) > 0.5f) r.v[13] = 1;
        } else if (e.type == "pixelate") {
            r.v[14] = std::max(r.v[14], P(0));
        } else if (e.type == "rgbsplit") {
            r.v[11] += P(0);
        } else if (e.type == "vignette") {
            r.v[10] = std::max(r.v[10], P(0));
        } else if (e.type == "strobe") {
            double period = 1.0 / std::max(0.1f, P(0));
            double ph = std::fmod(now, period) / period;
            if (ph > P(1) / 100.0) r.opacityMul = 0.f;
        }
    }
    return r;
}

} // namespace ps
