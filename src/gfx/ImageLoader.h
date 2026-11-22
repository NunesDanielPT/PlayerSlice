#pragma once
#include "gfx/Gfx.h"
#include <string>

namespace ps {

struct ImageTexture {
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
    bool valid() const { return srv != nullptr; }
};

// Carrega PNG/JPG/BMP/TIFF (WIC). Resultado em cache por caminho; falhas também (não repete).
const ImageTexture* loadImageCached(const std::string& pathUtf8);

} // namespace ps
