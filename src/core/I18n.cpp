#include "core/I18n.h"
#include <mutex>
#include <unordered_map>

namespace ps {

static Lang g_lang = Lang::PT;

// dicionário PT → EN
static const std::unordered_map<std::string, std::string>& dict() {
    static const std::unordered_map<std::string, std::string> d = {
#include "core/I18nStrings.inc"
    };
    return d;
}

void setLanguage(Lang l) { g_lang = l; }
Lang language() { return g_lang; }
std::string langCode(Lang l) { return l == Lang::EN ? "en" : "pt"; }
Lang langFromCode(const std::string& c) {
    if (c.size() >= 2 && (c[0] == 'e' || c[0] == 'E') && (c[1] == 'n' || c[1] == 'N')) return Lang::EN;
    if (c == "english" || c == "en") return Lang::EN;
    return Lang::PT;
}

const char* T(const char* pt) {
    if (g_lang == Lang::PT || !pt) return pt;
    // cache de resultados compostos (texto + "##id") com ponteiros estáveis
    static std::unordered_map<const char*, std::string> cache;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    auto c = cache.find(pt);
    if (c != cache.end()) return c->second.c_str();
    std::string s(pt);
    std::string head = s, tail;
    auto pos = s.find("##");
    if (pos != std::string::npos) { head = s.substr(0, pos); tail = s.substr(pos); }
    auto it = dict().find(head);
    std::string out = (it != dict().end() ? it->second : head) + tail;
    return cache.emplace(pt, std::move(out)).first->second.c_str();
}

std::vector<std::string> missingTranslations(const std::vector<std::string>& keys) {
    std::vector<std::string> out;
    for (auto& k : keys) {
        std::string head = k.substr(0, k.find("##"));
        if (head.empty()) continue;
        if (!dict().count(head)) out.push_back(head);
    }
    return out;
}

} // namespace ps
