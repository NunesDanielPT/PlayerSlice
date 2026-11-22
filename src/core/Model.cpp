#include "core/Model.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

using nlohmann::json;

namespace ps {

uint32_t paletteColor(int i) {
    // cores parecidas às do test pattern do Resolume (RGBA little-endian → 0xAABBGGRR)
    static const uint32_t pal[] = {
        0xFF2020E0, 0xFFE020E0, 0xFFE03010, 0xFF30E030, 0xFF20D0E0,
        0xFF8020F0, 0xFFE0C020, 0xFF2080FF, 0xFF8060FF, 0xFF60E0A0,
        0xFFC0A040, 0xFF4040A0,
    };
    return pal[i % (sizeof(pal) / sizeof(pal[0]))];
}

void Slice::inputQuad(float q[4][2]) const {
    float r[4][2];
    if (inWarp) { for (int i = 0; i < 4; ++i) { r[i][0] = inPts[i][0]; r[i][1] = inPts[i][1]; } }
    else {
        r[0][0] = input.x;           r[0][1] = input.y;
        r[1][0] = input.x + input.w; r[1][1] = input.y;
        r[2][0] = input.x + input.w; r[2][1] = input.y + input.h;
        r[3][0] = input.x;           r[3][1] = input.y + input.h;
    }
    int idx[4] = {0, 1, 2, 3};
    if (flipH) { std::swap(idx[0], idx[1]); std::swap(idx[2], idx[3]); }
    if (flipV) { std::swap(idx[0], idx[3]); std::swap(idx[1], idx[2]); }
    for (int i = 0; i < 4; ++i) { q[i][0] = r[idx[i]][0]; q[i][1] = r[idx[i]][1]; }
}

void Slice::outputQuad(float q[4][2]) const {
    if (outWarp) { for (int i = 0; i < 4; ++i) { q[i][0] = outPts[i][0]; q[i][1] = outPts[i][1]; } return; }
    float hw = outW * 0.5f, hh = outH * 0.5f;
    float a = rotation * 3.14159265f / 180.f, cs = std::cos(a), sn = std::sin(a);
    const float lx[4] = {-hw, hw, hw, -hw}, ly[4] = {-hh, -hh, hh, hh};
    for (int i = 0; i < 4; ++i) {
        q[i][0] = outX + lx[i] * cs - ly[i] * sn;
        q[i][1] = outY + lx[i] * sn + ly[i] * cs;
    }
}

Layer& Project::insertLayer(int index, const std::string& name) {
    Layer l;
    l.id = newId();
    l.name = name;
    l.region = {0, 0, (float)comp.width, (float)comp.height};
    l.clips.resize(comp.columns.size());
    index = std::clamp(index, 0, (int)comp.layers.size());
    comp.layers.insert(comp.layers.begin() + index, l);
    return comp.layers[index];
}

Layer& Project::addLayer(const std::string& name) { return insertLayer(0, name); } // nova layer fica no topo

void Project::ensureColumns(int n) {
    while ((int)comp.columns.size() < n)
        comp.columns.push_back(Column{"Coluna " + std::to_string(comp.columns.size() + 1), 0});
    for (auto& l : comp.layers)
        if ((int)l.clips.size() < (int)comp.columns.size()) l.clips.resize(comp.columns.size());
}

Screen& Project::addScreen(const std::string& name) {
    Screen s;
    s.id = newId();
    s.name = name;
    s.width = comp.width;
    s.height = comp.height;
    screens.push_back(s);
    return screens.back();
}

Slice& Project::addSlice(Screen& s, const std::string& name) {
    int count = 0;
    for (auto& sc : screens) count += (int)sc.slices.size();
    Slice sl;
    sl.id = newId();
    sl.name = name;
    float w = std::min(480.f, (float)comp.width), h = std::min(270.f, (float)comp.height);
    float off = 24.f * (s.slices.size() % 10);
    sl.input = {off, off, w, h};
    sl.outW = w; sl.outH = h;
    sl.outX = w * 0.5f + off; sl.outY = h * 0.5f + off;
    sl.color = paletteColor(count);
    s.slices.push_back(sl);
    return s.slices.back();
}

Screen* Project::findScreen(uint32_t id) {
    for (auto& s : screens) if (s.id == id) return &s;
    return nullptr;
}

Slice* Project::findSlice(uint32_t id, Screen** owner) {
    for (auto& s : screens)
        for (auto& sl : s.slices)
            if (sl.id == id) { if (owner) *owner = &s; return &sl; }
    return nullptr;
}

int Project::layerIndex(uint32_t layerId) const {
    for (int i = 0; i < (int)comp.layers.size(); ++i) if (comp.layers[i].id == layerId) return i;
    return -1;
}

Project Project::makeDefault() {
    Project p;
    p.comp.width = 1920; p.comp.height = 1080;
    p.ensureColumns(8);
    p.addLayer("Layer 1");
    p.addLayer("Layer 2");
    p.addLayer("Layer 3");
    Screen& s = p.addScreen("Saída 1");
    Slice& sl = p.addSlice(s, "Slice 1");
    sl.input = {0, 0, 1920, 1080};
    sl.outX = 960; sl.outY = 540; sl.outW = 1920; sl.outH = 1080;
    return p;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
static json fxJ(const std::vector<Effect>& fx) {
    json a = json::array();
    for (auto& e : fx) a.push_back({{"type", e.type}, {"on", e.enabled}, {"p", e.p}});
    return a;
}
static std::vector<Effect> fxFrom(const json& j) {
    std::vector<Effect> out;
    if (!j.is_array()) return out;
    for (auto& x : j) {
        if (!x.is_object() || !x.contains("type") || !x["type"].is_string()) continue;
        Effect e = makeEffect(x["type"].get<std::string>());
        if (!findEffect(e.type)) continue;
        e.enabled = x.value("on", true);
        if (x.contains("p") && x["p"].is_array())
            for (size_t i = 0; i < x["p"].size() && i < e.p.size(); ++i) if (x["p"][i].is_number()) e.p[i] = x["p"][i];
        out.push_back(e);
    }
    return out;
}

static json rectJ(const Rect& r) { return json::array({r.x, r.y, r.w, r.h}); }
static Rect rectFrom(const json& j) {
    Rect r;
    if (j.is_array() && j.size() == 4) { r.x = j[0]; r.y = j[1]; r.w = j[2]; r.h = j[3]; }
    return r;
}

std::string Project::toJson() const {
    json j;
    j["app"] = "PLAYERSLICE";
    j["format"] = 1;
    j["nextId"] = nextId;
    json& c = j["composition"];
    c["name"] = comp.name; c["description"] = comp.description;
    c["width"] = comp.width; c["height"] = comp.height; c["fps"] = comp.fps;
    c["colorDepth"] = comp.colorDepth; c["master"] = comp.master; c["effects"] = fxJ(comp.effects);
    c["columns"] = json::array();
    for (auto& col : comp.columns) c["columns"].push_back({{"name", col.name}, {"color", col.color}});
    c["layers"] = json::array();
    for (auto& l : comp.layers) {
        json lj;
        lj["id"] = l.id; lj["name"] = l.name; lj["opacity"] = l.opacity;
        lj["blend"] = (int)l.blend; lj["bypass"] = l.bypass; lj["solo"] = l.solo;
        lj["locked"] = l.locked; lj["fadeTime"] = l.fadeTime;
        lj["useRegion"] = l.useRegion; lj["region"] = rectJ(l.region);
        lj["color"] = l.color; lj["ignoreColumn"] = l.ignoreColumn;
        lj["transform"] = {l.posX, l.posY, l.scale, l.rotation}; lj["effects"] = fxJ(l.effects);
        lj["clips"] = json::array();
        for (auto& cl : l.clips) {
            json cj;
            if (!cl.empty()) {
                cj["id"] = cl.id; cj["path"] = cl.path; cj["name"] = cl.name;
                cj["loop"] = (int)cl.loop; cj["speed"] = cl.speed; cj["opacity"] = cl.opacity;
                cj["fit"] = (int)cl.fit; cj["offset"] = {cl.offsetX, cl.offsetY}; cj["scale"] = cl.scale;
                cj["rotation"] = cl.rotation; cj["flipH"] = cl.flipH; cj["flipV"] = cl.flipV; cj["effects"] = fxJ(cl.effects);
            }
            lj["clips"].push_back(cj);
        }
        c["layers"].push_back(lj);
    }
    j["screens"] = json::array();
    for (auto& s : screens) {
        json sj;
        sj["id"] = s.id; sj["name"] = s.name; sj["device"] = (int)s.device;
        sj["monitor"] = s.monitorKey; sj["width"] = s.width; sj["height"] = s.height;
        sj["brightness"] = s.brightness; sj["testPattern"] = s.testPattern; sj["inToggle"] = s.inToggle;
        sj["opacity"] = s.opacity; sj["contrast"] = s.contrast; sj["rgb"] = {s.red, s.green, s.blue};
        sj["guide"] = s.guidePath; sj["guideOpacity"] = s.guideOpacity;
        sj["slices"] = json::array();
        for (auto& sl : s.slices) {
            json k;
            k["id"] = sl.id; k["name"] = sl.name; k["enabled"] = sl.enabled; k["sourceLayer"] = sl.sourceLayer;
            k["input"] = rectJ(sl.input);
            k["out"] = {sl.outX, sl.outY, sl.outW, sl.outH};
            k["rotation"] = sl.rotation; k["flipH"] = sl.flipH; k["flipV"] = sl.flipV;
            k["opacity"] = sl.opacity; k["brightness"] = sl.brightness; k["color"] = sl.color;
            auto pts = [](const float p[4][2]) { json a = json::array(); for (int i = 0; i < 4; ++i) a.push_back({p[i][0], p[i][1]}); return a; };
            k["inWarp"] = sl.inWarp; k["inPts"] = pts(sl.inPts);
            k["outWarp"] = sl.outWarp; k["outPts"] = pts(sl.outPts);
            sj["slices"].push_back(k);
        }
        j["screens"].push_back(sj);
    }
    return j.dump(2);
}

template <class T> static void get(const json& j, const char* k, T& v) {
    auto it = j.find(k);
    if (it != j.end() && !it->is_null()) v = it->get<T>();
}

bool Project::fromJson(const std::string& text, std::string* err) {
    try {
        json j = json::parse(text);
        if (j.value("app", "") != "PLAYERSLICE") { if (err) *err = "Não é um projeto PLAYERSLICE"; return false; }
        Project p;
        get(j, "nextId", p.nextId);
        const json& c = j.at("composition");
        get(c, "name", p.comp.name); get(c, "description", p.comp.description);
        get(c, "width", p.comp.width); get(c, "height", p.comp.height); get(c, "fps", p.comp.fps);
        get(c, "colorDepth", p.comp.colorDepth); get(c, "master", p.comp.master);
        if (c.contains("effects")) p.comp.effects = fxFrom(c["effects"]);
        for (auto& cj : c.value("columns", json::array())) {
            Column col;
            if (cj.is_string()) col.name = cj.get<std::string>();          // formato antigo
            else if (cj.is_object()) { get(cj, "name", col.name); get(cj, "color", col.color); }
            p.comp.columns.push_back(col);
        }
        for (auto& lj : c.value("layers", json::array())) {
            Layer l;
            get(lj, "id", l.id); get(lj, "name", l.name); get(lj, "opacity", l.opacity);
            int b = 0; get(lj, "blend", b); l.blend = (BlendMode)b;
            get(lj, "bypass", l.bypass); get(lj, "solo", l.solo); get(lj, "locked", l.locked);
            get(lj, "fadeTime", l.fadeTime); get(lj, "useRegion", l.useRegion);
            if (lj.contains("region")) l.region = rectFrom(lj["region"]);
            get(lj, "color", l.color); get(lj, "ignoreColumn", l.ignoreColumn);
            if (lj.contains("transform") && lj["transform"].size() == 4) {
                l.posX = lj["transform"][0]; l.posY = lj["transform"][1]; l.scale = lj["transform"][2]; l.rotation = lj["transform"][3];
            }
            if (lj.contains("effects")) l.effects = fxFrom(lj["effects"]);
            for (auto& cj : lj.value("clips", json::array())) {
                Clip cl;
                if (cj.is_object() && cj.contains("path")) {
                    get(cj, "id", cl.id); get(cj, "path", cl.path); get(cj, "name", cl.name);
                    int lm = 0; get(cj, "loop", lm); cl.loop = (LoopMode)lm;
                    get(cj, "speed", cl.speed); get(cj, "opacity", cl.opacity);
                    int f = 0; get(cj, "fit", f); cl.fit = (FitMode)f;
                    if (cj.contains("offset")) { cl.offsetX = cj["offset"][0]; cl.offsetY = cj["offset"][1]; }
                    get(cj, "scale", cl.scale);
                    get(cj, "rotation", cl.rotation); get(cj, "flipH", cl.flipH); get(cj, "flipV", cl.flipV);
                    if (cj.contains("effects")) cl.effects = fxFrom(cj["effects"]);
                }
                l.clips.push_back(cl);
            }
            p.comp.layers.push_back(l);
        }
        for (auto& sj : j.value("screens", json::array())) {
            Screen s;
            get(sj, "id", s.id); get(sj, "name", s.name);
            int d = 0; get(sj, "device", d); s.device = (ScreenDevice)d;
            get(sj, "monitor", s.monitorKey); get(sj, "width", s.width); get(sj, "height", s.height);
            get(sj, "brightness", s.brightness); get(sj, "testPattern", s.testPattern); get(sj, "inToggle", s.inToggle);
            get(sj, "opacity", s.opacity); get(sj, "contrast", s.contrast);
            if (sj.contains("rgb") && sj["rgb"].size() == 3) { s.red = sj["rgb"][0]; s.green = sj["rgb"][1]; s.blue = sj["rgb"][2]; }
            get(sj, "guide", s.guidePath); get(sj, "guideOpacity", s.guideOpacity);
            for (auto& k : sj.value("slices", json::array())) {
                Slice sl;
                get(k, "id", sl.id); get(k, "name", sl.name); get(k, "enabled", sl.enabled);
                get(k, "sourceLayer", sl.sourceLayer);
                if (k.contains("input")) sl.input = rectFrom(k["input"]);
                if (k.contains("out")) { auto& o = k["out"]; sl.outX = o[0]; sl.outY = o[1]; sl.outW = o[2]; sl.outH = o[3]; }
                get(k, "rotation", sl.rotation); get(k, "flipH", sl.flipH); get(k, "flipV", sl.flipV);
                get(k, "opacity", sl.opacity); get(k, "brightness", sl.brightness); get(k, "color", sl.color);
                auto rdPts = [&](const char* key, float p[4][2]) {
                    if (!k.contains(key) || !k[key].is_array() || k[key].size() != 4) return false;
                    for (int i = 0; i < 4; ++i) { p[i][0] = k[key][i][0]; p[i][1] = k[key][i][1]; }
                    return true;
                };
                get(k, "inWarp", sl.inWarp); get(k, "outWarp", sl.outWarp);
                if (!rdPts("inPts", sl.inPts)) sl.inWarp = false;
                if (!rdPts("outPts", sl.outPts)) sl.outWarp = false;
                s.slices.push_back(sl);
            }
            p.screens.push_back(s);
        }
        // validação defensiva
        p.comp.width  = std::clamp(p.comp.width, 16, 16384);
        p.comp.height = std::clamp(p.comp.height, 16, 16384);
        if (p.comp.colorDepth != 16) p.comp.colorDepth = 8;
        p.ensureColumns(std::max<int>(1, (int)p.comp.columns.size()));
        uint32_t maxId = 0;
        for (auto& l : p.comp.layers) { maxId = std::max(maxId, l.id); for (auto& cl : l.clips) maxId = std::max(maxId, cl.id); }
        for (auto& s : p.screens) { maxId = std::max(maxId, s.id); for (auto& sl : s.slices) maxId = std::max(maxId, sl.id); }
        p.nextId = std::max(p.nextId, maxId + 1);
        for (auto& s : p.screens) for (auto& sl : s.slices)
            if (sl.sourceLayer && p.layerIndex(sl.sourceLayer) < 0) sl.sourceLayer = 0;
        *this = std::move(p);
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

} // namespace ps
