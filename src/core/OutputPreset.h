#pragma once
// Presets de saída (ecrãs + slices), como o Advanced Output do Resolume.
// Formato próprio (.psout, JSON) e importação/exportação do XML do Resolume Arena 7
// (o mesmo que o MapSim exporta).
#include "core/Model.h"
#include <string>
#include <vector>

namespace ps {

std::string exportScreensJson(const Project& p);
// substitui os ecrãs do projeto; ids novos; sourceLayer validado contra as layers do projeto
// createdLayers: layers dedicadas que não existiam e foram criadas (ligadas pelo nome)
bool importScreensJson(Project& p, const std::string& text, std::string* err, std::vector<std::string>* createdLayers = nullptr);

struct ResolumeImport {
    std::vector<Screen> screens;
    int compW = 0, compH = 0;       // CurrentCompositionTextureSize (0 = não indicado)
    int warpedSlices = 0;           // slices com warp bezier/homografia (não importado)
    int maskedSlices = 0;           // polígonos/máscaras (importado só o retângulo)
};
bool importResolumeXml(const std::string& xml, Project& idSource, ResolumeImport& out, std::string* err);
std::string exportResolumeXml(const Project& p, const std::string& presetName);

} // namespace ps
