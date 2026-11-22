#include "gfx/ImageLoader.h"
#include "core/Log.h"
#include <wincodec.h>
#include <map>
#include <vector>

namespace ps {

static bool loadWic(const std::wstring& path, std::vector<uint8_t>& rgba, int& w, int& h) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    UINT uw = 0, uh = 0;
    conv->GetSize(&uw, &uh);
    if (!uw || !uh || uw > 16384 || uh > 16384) return false;
    rgba.resize((size_t)uw * uh * 4);
    if (FAILED(conv->CopyPixels(nullptr, uw * 4, (UINT)rgba.size(), rgba.data()))) return false;
    w = (int)uw; h = (int)uh;
    return true;
}

const ImageTexture* loadImageCached(const std::string& path) {
    static std::map<std::string, ImageTexture> cache;
    if (path.empty()) return nullptr;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second.valid() ? &it->second : nullptr;
    ImageTexture& t = cache[path];
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (!loadWic(fromUtf8(path), px, w, h)) { PS_WARN("Não foi possível abrir a imagem %s", path.c_str()); return nullptr; }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), (UINT)w * 4, 0};
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(gfx().dev()->CreateTexture2D(&td, &sd, &tex))) return nullptr;
    gfx().dev()->CreateShaderResourceView(tex.Get(), nullptr, &t.srv);
    t.w = w; t.h = h;
    return t.valid() ? &t : nullptr;
}

} // namespace ps
