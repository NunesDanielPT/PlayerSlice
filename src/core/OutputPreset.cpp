#include "core/OutputPreset.h"
#include "core/Geometry.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>

using json = nlohmann::json;

namespace ps {

// ---------------------------------------------------------------------------
// formato próprio (.psout): o mesmo JSON dos ecrãs que vai no projeto
// ---------------------------------------------------------------------------
std::string exportScreensJson(const Project& p) {
    Project t;
    t.comp.width = p.comp.width; t.comp.height = p.comp.height;
    t.comp.layers = p.comp.layers;           // para as slices dedicadas manterem a layer
    for (auto& l : t.comp.layers) for (auto& c : l.clips) c = Clip{};
    t.screens = p.screens;
    t.nextId = p.nextId;
    json j = json::parse(t.toJson());
    json o;
    o["app"] = "PLAYERSLICE";
    o["type"] = "output-preset";
    o["format"] = 1;
    o["composition"] = {{"width", p.comp.width}, {"height", p.comp.height}};
    o["layers"] = json::array();
    for (auto& l : p.comp.layers) o["layers"].push_back({{"id", l.id}, {"name", l.name}});
    o["screens"] = j["screens"];
    return o.dump(2);
}

bool importScreensJson(Project& p, const std::string& text, std::string* err) {
    try {
        json o = json::parse(text);
        if (o.value("type", "") != "output-preset" || !o.contains("screens")) { if (err) *err = "Não é um preset de saída do PLAYERSLICE"; return false; }
        // reaproveitar o leitor do projeto: montar um projeto mínimo com estes ecrãs
        json j;
        j["app"] = "PLAYERSLICE";
        j["composition"] = {{"width", p.comp.width}, {"height", p.comp.height}, {"layers", json::array()}};
        for (auto& l : o.value("layers", json::array())) j["composition"]["layers"].push_back({{"id", l.value("id", 0u)}, {"name", l.value("name", "")}});
        j["screens"] = o["screens"];
        Project t;
        if (!t.fromJson(j.dump(), err)) return false;
        // ids novos e layers ligadas pelo nome (se o ID não existir neste projeto)
        std::map<uint32_t, std::string> names;
        for (auto& l : o.value("layers", json::array())) names[l.value("id", 0u)] = l.value("name", "");
        for (auto& s : t.screens) {
            s.id = p.newId();
            for (auto& sl : s.slices) {
                sl.id = p.newId();
                if (sl.sourceLayer && p.layerIndex(sl.sourceLayer) < 0) {
                    uint32_t found = 0;
                    auto it = names.find(sl.sourceLayer);
                    if (it != names.end()) for (auto& l : p.comp.layers) if (l.name == it->second) { found = l.id; break; }
                    sl.sourceLayer = found;
                }
            }
        }
        p.screens = std::move(t.screens);
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// XML mínimo (elementos + atributos; o texto é ignorado)
// ---------------------------------------------------------------------------
namespace {
struct XNode {
    std::string name;
    std::map<std::string, std::string> attr;
    std::vector<std::unique_ptr<XNode>> kids;
    const XNode* child(const char* n) const { for (auto& k : kids) if (k->name == n) return k.get(); return nullptr; }
    std::string a(const char* n, const std::string& def = "") const { auto it = attr.find(n); return it == attr.end() ? def : it->second; }
    double num(const char* n, double def = 0) const { auto it = attr.find(n); return it == attr.end() ? def : atof(it->second.c_str()); }
};

std::string unesc(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "amp") o += '&'; else if (ent == "lt") o += '<'; else if (ent == "gt") o += '>';
        else if (ent == "quot") o += '"'; else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long c = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16) : strtoul(ent.c_str() + 1, nullptr, 10);
            if (c < 0x80) o += (char)c;
            else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
            else { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
        } else { o += s.substr(i, e - i + 1); }
        i = e;
    }
    return o;
}

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break; case '<': o += "&lt;"; break; case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break; default: o += c;
        }
    }
    return o;
}

struct XParser {
    const std::string& s; size_t i = 0;
    explicit XParser(const std::string& t) : s(t) {}
    void ws() { while (i < s.size() && isspace((unsigned char)s[i])) ++i; }
    bool skipMisc() {   // <?xml ?>, <!-- -->, <!DOCTYPE>
        ws();
        if (s.compare(i, 4, "<!--") == 0) { size_t e = s.find("-->", i); i = e == std::string::npos ? s.size() : e + 3; return true; }
        if (s.compare(i, 2, "<?") == 0) { size_t e = s.find("?>", i); i = e == std::string::npos ? s.size() : e + 2; return true; }
        if (s.compare(i, 2, "<!") == 0) { size_t e = s.find('>', i); i = e == std::string::npos ? s.size() : e + 1; return true; }
        return false;
    }
    std::unique_ptr<XNode> element(int depth) {
        if (depth > 64) throw std::runtime_error("XML demasiado profundo");
        while (skipMisc()) {}
        if (i >= s.size() || s[i] != '<') throw std::runtime_error("XML inválido");
        ++i;
        auto n = std::make_unique<XNode>();
        while (i < s.size() && !isspace((unsigned char)s[i]) && s[i] != '>' && s[i] != '/') n->name += s[i++];
        for (;;) {
            ws();
            if (i >= s.size()) throw std::runtime_error("XML incompleto");
            if (s[i] == '/') { i += 2; return n; }            // />
            if (s[i] == '>') { ++i; break; }
            std::string k;
            while (i < s.size() && s[i] != '=' && !isspace((unsigned char)s[i]) && s[i] != '>' && s[i] != '/') k += s[i++];
            ws();
            if (i < s.size() && s[i] == '=') {
                ++i; ws();
                char q = s[i++];
                size_t e = s.find(q, i);
                if (e == std::string::npos) throw std::runtime_error("XML incompleto");
                n->attr[k] = unesc(s.substr(i, e - i));
                i = e + 1;
            }
        }
        // filhos até </nome>
        for (;;) {
            while (i < s.size() && s[i] != '<') ++i;              // ignora texto
            if (i >= s.size()) throw std::runtime_error("XML incompleto");
            if (s.compare(i, 2, "</") == 0) { size_t e = s.find('>', i); i = e == std::string::npos ? s.size() : e + 1; return n; }
            if (skipMisc()) continue;
            n->kids.push_back(element(depth + 1));
        }
    }
};

bool quadFrom(const XNode* r, float q[4][2]) {
    if (!r) return false;
    int k = 0;
    for (auto& v : r->kids) if (v->name == "v" && k < 4) { q[k][0] = (float)v->num("x"); q[k][1] = (float)v->num("y"); ++k; }
    return k == 4;
}
// <Params name="Common"> (o nome está no atributo, não no elemento)
const XNode* paramsNamed(const XNode* n, const char* name) {
    if (!n) return nullptr;
    for (auto& k : n->kids) if (k->name == "Params" && k->a("name") == name) return k.get();
    return nullptr;
}
std::string paramValue(const XNode* params, const char* name) {
    if (!params) return {};
    for (auto& p : params->kids) if (p->a("name") == name) return p->a("value");
    return {};
}
bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }
} // namespace

bool importResolumeXml(const std::string& xml, Project& ids, ResolumeImport& out, std::string* err) {
    try {
        XParser ps(xml);
        auto root = ps.element(0);
        const XNode* setup = root->name == "ScreenSetup" ? root.get() : root->child("ScreenSetup");
        if (!setup) { if (err) *err = "Não é um preset de Advanced Output do Resolume (falta ScreenSetup)"; return false; }
        if (const XNode* cs = setup->child("CurrentCompositionTextureSize")) { out.compW = (int)cs->num("width"); out.compH = (int)cs->num("height"); }
        const XNode* screens = setup->child("screens");
        if (!screens) { if (err) *err = "O preset não tem ecrãs"; return false; }
        for (auto& sn : screens->kids) {
            if (sn->name != "Screen") continue;
            Screen s;
            s.id = ids.newId();
            s.name = paramValue(paramsNamed(sn.get(), "Params"), "Name");
            if (s.name.empty()) s.name = sn->a("name", "Screen");
            s.device = ScreenDevice::Disabled;
            float maxX = 0, maxY = 0;
            if (const XNode* dev = sn->child("OutputDevice")) {
                for (auto& d : dev->kids) {
                    if (d->num("width") > 0 && d->num("height") > 0) { s.width = (int)d->num("width"); s.height = (int)d->num("height"); }
                    if (d->name == "OutputDeviceVirtual") s.device = ScreenDevice::Virtual;
                }
            }
            if (const XNode* layers = sn->child("layers")) {
                for (auto& ln : layers->kids) {
                    if (ln->name != "Slice" && ln->name != "Polygon" && ln->name != "Mask") continue;
                    if (ln->name != "Slice") { out.maskedSlices++; if (ln->name == "Mask") continue; }
                    Slice sl;
                    sl.id = ids.newId();
                    const XNode* common = paramsNamed(ln.get(), "Common");
                    sl.name = paramValue(common, "Name");
                    if (sl.name.empty()) sl.name = "Slice " + std::to_string(s.slices.size() + 1);
                    std::string en = paramValue(common, "Enabled");
                    sl.enabled = en.empty() || en != "0";
                    float qi[4][2], qo[4][2];
                    if (!quadFrom(ln->child("InputRect"), qi) || !quadFrom(ln->child("OutputRect"), qo)) continue;
                    if (const XNode* wp = ln->child("Warper")) {
                        float qw[4][2];
                        bool have = false;
                        if (const XNode* bz = wp->child("BezierWarper")) {
                            int cw = (int)bz->num("controlWidth"), ch = (int)bz->num("controlHeight");
                            std::vector<std::pair<float, float>> g;
                            if (const XNode* vs = bz->child("vertices"))
                                for (auto& v : vs->kids) if (v->name == "v") g.push_back({(float)v->num("x"), (float)v->num("y")});
                            if (cw >= 2 && ch >= 2 && (int)g.size() == cw * ch) {
                                const int idx[4] = {0, cw - 1, cw * ch - 1, cw * (ch - 1)};
                                for (int k = 0; k < 4; ++k) { qw[k][0] = g[idx[k]].first; qw[k][1] = g[idx[k]].second; }
                                have = true;
                                // pontos interiores fora do plano → curvas (só os cantos são usados)
                                float dev = 0;
                                for (int r = 0; r < ch; ++r) for (int c = 0; c < cw; ++c) {
                                    float u = (float)c / (cw - 1), w = (float)r / (ch - 1);
                                    float ex = (1 - u) * (1 - w) * qw[0][0] + u * (1 - w) * qw[1][0] + u * w * qw[2][0] + (1 - u) * w * qw[3][0];
                                    float ey = (1 - u) * (1 - w) * qw[0][1] + u * (1 - w) * qw[1][1] + u * w * qw[2][1] + (1 - u) * w * qw[3][1];
                                    dev = std::max(dev, std::hypot(g[r * cw + c].first - ex, g[r * cw + c].second - ey));
                                }
                                if (dev > 3.f) out.warpedSlices++;
                            }
                        }
                        if (!have) for (int k = 0; k < 4; ++k) { qw[k][0] = qo[k][0]; qw[k][1] = qo[k][1]; }
                        // homografia (perspetiva) aplicada por cima
                        if (const XNode* hg = wp->child("Homography")) {
                            float a[4][2], b[4][2];
                            if (quadFrom(hg->child("src"), a) && quadFrom(hg->child("dst"), b)) {
                                bool same = true;
                                for (int k = 0; k < 4; ++k) if (!near(a[k][0], b[k][0]) || !near(a[k][1], b[k][1])) same = false;
                                if (!same) {
                                    Mat3 hs = inverse(squareToQuad(a)), hd = squareToQuad(b);
                                    for (int k = 0; k < 4; ++k) {
                                        double ux, uy, ox, oy;
                                        apply(hs, qw[k][0], qw[k][1], ux, uy);
                                        apply(hd, ux, uy, ox, oy);
                                        qw[k][0] = (float)ox; qw[k][1] = (float)oy;
                                    }
                                }
                            }
                        }
                        for (int k = 0; k < 4; ++k) { qo[k][0] = qw[k][0]; qo[k][1] = qw[k][1]; }
                    }
                    // entrada: retângulo direito (com espelhos) ou quadrilátero livre
                    float x0 = std::min({qi[0][0], qi[1][0], qi[2][0], qi[3][0]}), x1 = std::max({qi[0][0], qi[1][0], qi[2][0], qi[3][0]});
                    float y0 = std::min({qi[0][1], qi[1][1], qi[2][1], qi[3][1]}), y1 = std::max({qi[0][1], qi[1][1], qi[2][1], qi[3][1]});
                    bool axis = near(qi[0][1], qi[1][1]) && near(qi[2][1], qi[3][1]) && near(qi[0][0], qi[3][0]) && near(qi[1][0], qi[2][0]);
                    if (axis && x1 - x0 > 0.5f && y1 - y0 > 0.5f) {
                        sl.input = {x0, y0, x1 - x0, y1 - y0};
                        sl.flipH = qi[0][0] > qi[1][0];
                        sl.flipV = qi[0][1] > qi[3][1];
                    } else {
                        sl.inWarp = true;
                        for (int k = 0; k < 4; ++k) { sl.inPts[k][0] = qi[k][0]; sl.inPts[k][1] = qi[k][1]; }
                        sl.input = {x0, y0, std::max(1.f, x1 - x0), std::max(1.f, y1 - y0)};
                    }
                    // saída: retângulo rodado (qualquer ângulo) ou quadrilátero livre
                    float ex = qo[1][0] - qo[0][0], ey = qo[1][1] - qo[0][1];      // aresta de cima
                    float fx = qo[3][0] - qo[0][0], fy = qo[3][1] - qo[0][1];      // aresta da esquerda
                    float w = std::hypot(ex, ey), h = std::hypot(fx, fy);
                    bool para = near(qo[2][0], qo[1][0] + fx) && near(qo[2][1], qo[1][1] + fy);
                    bool perp = w > 0.5f && h > 0.5f && std::fabs(ex * fx + ey * fy) < 0.001f * w * h + 0.01f;
                    bool rightHanded = ex * fy - ey * fx > 0;
                    if (para && perp && rightHanded) {
                        sl.outW = w; sl.outH = h;
                        sl.outX = (qo[0][0] + qo[2][0]) * 0.5f; sl.outY = (qo[0][1] + qo[2][1]) * 0.5f;
                        float deg = std::atan2(ey, ex) * 180.f / 3.14159265f;
                        if (std::fabs(deg - std::round(deg)) < 0.001f) deg = std::round(deg);
                        sl.rotation = deg;
                    } else {
                        sl.outWarp = true;
                        for (int k = 0; k < 4; ++k) { sl.outPts[k][0] = qo[k][0]; sl.outPts[k][1] = qo[k][1]; }
                        float ox0 = std::min({qo[0][0], qo[1][0], qo[2][0], qo[3][0]}), ox1 = std::max({qo[0][0], qo[1][0], qo[2][0], qo[3][0]});
                        float oy0 = std::min({qo[0][1], qo[1][1], qo[2][1], qo[3][1]}), oy1 = std::max({qo[0][1], qo[1][1], qo[2][1], qo[3][1]});
                        sl.outX = (ox0 + ox1) * 0.5f; sl.outY = (oy0 + oy1) * 0.5f; sl.outW = ox1 - ox0; sl.outH = oy1 - oy0;
                    }
                    for (int k = 0; k < 4; ++k) { maxX = std::max(maxX, qo[k][0]); maxY = std::max(maxY, qo[k][1]); }
                    s.slices.push_back(sl);
                }
            }
            if (s.width <= 16 && maxX > 0) { s.width = (int)std::ceil(maxX); s.height = (int)std::ceil(maxY); }
            s.width = std::clamp(s.width, 16, 16384);
            s.height = std::clamp(s.height, 16, 16384);
            out.screens.push_back(std::move(s));
        }
        if (out.screens.empty()) { if (err) *err = "O preset não tem ecrãs"; return false; }
        return true;
    } catch (const std::exception& e) {
        if (err) *err = std::string("XML inválido: ") + e.what();
        return false;
    }
}

std::string exportResolumeXml(const Project& p, const std::string& presetName) {
    std::ostringstream o;
    auto n = [](float v) { char b[32]; if (std::fabs(v - std::round(v)) < 1e-4f) snprintf(b, sizeof(b), "%d", (int)std::lround(v)); else snprintf(b, sizeof(b), "%.4f", v); return std::string(b); };
    long long id = 1700000000000LL;
    const char* I = "\t\t\t\t";
    o << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    o << "<XmlState name=\"" << esc(presetName) << "\">\n";
    o << "\t<versionInfo name=\"Resolume Arena\" majorVersion=\"7\" minorVersion=\"27\" microVersion=\"0\" revision=\"14395\"/>\n";
    o << "\t<ScreenSetup name=\"ScreenSetup\">\n\t\t<Params name=\"ScreenSetupParams\"/>\n";
    o << "\t\t<CurrentCompositionTextureSize width=\"" << p.comp.width << "\" height=\"" << p.comp.height << "\"/>\n\t\t<screens>\n";
    for (auto& s : p.screens) {
        o << I << "<Screen name=\"" << esc(s.name) << "\" uniqueId=\"" << id++ << "\">\n";
        o << I << "\t<Params name=\"Params\">\n";
        o << I << "\t\t<Param name=\"Name\" T=\"STRING\" default=\"\" value=\"" << esc(s.name) << "\"/>\n";
        o << I << "\t\t<Param name=\"Enabled\" T=\"BOOL\" default=\"1\" value=\"1\"/>\n";
        o << I << "\t\t<Param name=\"Hidden\" T=\"BOOL\" default=\"0\" value=\"0\"/>\n";
        o << I << "\t</Params>\n";
        o << I << "\t<guides>\n" << I << "\t\t<ScreenGuide name=\"ScreenGuide\" type=\"0\"/>\n" << I << "\t\t<ScreenGuide name=\"ScreenGuide\" type=\"1\"/>\n" << I << "\t</guides>\n";
        o << I << "\t<layers>\n";
        for (auto& sl : s.slices) {
            float qi[4][2], qo[4][2];
            sl.inputQuad(qi);
            sl.outputQuad(qo);
            o << I << "\t\t<Slice uniqueId=\"" << id++ << "\">\n";
            o << I << "\t\t\t<Params name=\"Common\">\n";
            o << I << "\t\t\t\t<Param name=\"Name\" T=\"STRING\" default=\"Layer\" value=\"" << esc(sl.name) << "\"/>\n";
            o << I << "\t\t\t\t<Param name=\"Enabled\" T=\"BOOL\" default=\"1\" value=\"" << (sl.enabled ? 1 : 0) << "\"/>\n";
            o << I << "\t\t\t</Params>\n";
            o << I << "\t\t\t<Params name=\"Input\">\n" << I << "\t\t\t\t<ParamChoice name=\"Input Source\" default=\"0:1\" value=\"0:1\" storeChoices=\"0\"/>\n" << I << "\t\t\t</Params>\n";
            o << I << "\t\t\t<Params name=\"Output\">\n" << I << "\t\t\t\t<Param name=\"Flip\" T=\"UINT8\" default=\"0\" value=\"0\"/>\n" << I << "\t\t\t</Params>\n";
            o << I << "\t\t\t<InputRect orientation=\"0\">\n";
            for (int k = 0; k < 4; ++k) o << I << "\t\t\t\t<v x=\"" << n(qi[k][0]) << "\" y=\"" << n(qi[k][1]) << "\"/>\n";
            o << I << "\t\t\t</InputRect>\n";
            float rc[4][2];   // retângulo da saída (antes do warp)
            if (sl.outWarp) {
                float x0 = std::min({qo[0][0], qo[1][0], qo[2][0], qo[3][0]}), x1 = std::max({qo[0][0], qo[1][0], qo[2][0], qo[3][0]});
                float y0 = std::min({qo[0][1], qo[1][1], qo[2][1], qo[3][1]}), y1 = std::max({qo[0][1], qo[1][1], qo[2][1], qo[3][1]});
                float r[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
                memcpy(rc, r, sizeof(rc));
            } else memcpy(rc, qo, sizeof(rc));
            o << I << "\t\t\t<OutputRect orientation=\"0\">\n";
            for (int k = 0; k < 4; ++k) o << I << "\t\t\t\t<v x=\"" << n(rc[k][0]) << "\" y=\"" << n(rc[k][1]) << "\"/>\n";
            o << I << "\t\t\t</OutputRect>\n";
            if (sl.outWarp) {
                o << I << "\t\t\t<Warper>\n" << I << "\t\t\t\t<Params name=\"Warper\">\n";
                o << I << "\t\t\t\t\t<ParamChoice name=\"Point Mode\" default=\"PM_LINEAR\" value=\"PM_LINEAR\" storeChoices=\"0\"/>\n";
                o << I << "\t\t\t\t\t<Param name=\"Flip\" T=\"UINT8\" default=\"0\" value=\"0\"/>\n" << I << "\t\t\t\t</Params>\n";
                o << I << "\t\t\t\t<BezierWarper controlWidth=\"4\" controlHeight=\"4\">\n" << I << "\t\t\t\t\t<vertices>\n";
                for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
                    float u = c / 3.f, w = r / 3.f;
                    float x = (1 - u) * (1 - w) * qo[0][0] + u * (1 - w) * qo[1][0] + u * w * qo[2][0] + (1 - u) * w * qo[3][0];
                    float y = (1 - u) * (1 - w) * qo[0][1] + u * (1 - w) * qo[1][1] + u * w * qo[2][1] + (1 - u) * w * qo[3][1];
                    o << I << "\t\t\t\t\t\t<v x=\"" << n(x) << "\" y=\"" << n(y) << "\"/>\n";
                }
                o << I << "\t\t\t\t\t</vertices>\n" << I << "\t\t\t\t</BezierWarper>\n";
                o << I << "\t\t\t\t<Homography>\n";
                for (const char* tag : {"src", "dst"}) {
                    o << I << "\t\t\t\t\t<" << tag << ">\n";
                    for (int k = 0; k < 4; ++k) o << I << "\t\t\t\t\t\t<v x=\"" << n(rc[k][0]) << "\" y=\"" << n(rc[k][1]) << "\"/>\n";
                    o << I << "\t\t\t\t\t</" << tag << ">\n";
                }
                o << I << "\t\t\t\t</Homography>\n" << I << "\t\t\t</Warper>\n";
            }
            o << I << "\t\t</Slice>\n";
        }
        o << I << "\t</layers>\n";
        o << I << "\t<OutputDevice>\n";
        o << I << "\t\t<OutputDeviceVirtual name=\"" << esc(s.name) << "\" deviceId=\"Virtual" << esc(s.name) << "\" width=\"" << s.width << "\" height=\"" << s.height << "\"/>\n";
        o << I << "\t</OutputDevice>\n";
        o << I << "</Screen>\n";
    }
    o << "\t\t</screens>\n\t</ScreenSetup>\n</XmlState>\n";
    return o.str();
}

} // namespace ps
