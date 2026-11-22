#pragma once
#include "gfx/Gfx.h"
#include "video/VideoPlayer.h"
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <string>

namespace ps {

// Miniaturas + info dos ficheiros (resolução, codec, duração), geradas em
// segundo plano. A textura é criada na thread de render (poll()).
class ThumbnailCache {
public:
    struct Entry {
        enum class State { Pending, Ready, Failed };
        std::atomic<State> state{State::Pending};
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        MediaInfo info;
        ComPtr<ID3D11ShaderResourceView> srv;
        bool uploaded = false;
    };

    static ThumbnailCache& get();
    // devolve sempre uma entrada (pode estar Pending)
    std::shared_ptr<Entry> request(const std::string& path);
    void poll();   // render thread
    void clear();

private:
    std::mutex m_mtx;
    std::map<std::string, std::shared_ptr<Entry>> m_map;
};

} // namespace ps
