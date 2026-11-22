#include "core/Model.h"
#include <nlohmann/json.hpp>
#include <algorithm>

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

Layer& Project::addLayer(const std::string& name) {
    Layer l;
    l.id = newId();
    l.name = name;
    l.region = {0, 0, (float)comp.width, (float)comp.height};
    l.clips.resize(comp.columns.size());
    comp.layers.insert(comp.layers.begin(), l); // nova layer fica no topo
    return comp.layers.front();
}

void Project::ensureColumns(int n) {
    while ((int)comp.columns.size() < n)
        comp.columns.push_back("Coluna " + std::to_string(comp.columns.size() + 1));
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
    c["width"] = comp.width; c["height"] = comp.height; c["fps"] = comp.fps;
    c["columns"] = comp.columns;
    c["layers"] = json::array();
    for (auto& l : comp.layers) {
        json lj;
        lj["id"] = l.id; lj["name"] = l.name; lj["opacity"] = l.opacity;
        lj["blend"] = (int)l.blend; lj["bypass"] = l.bypass; lj["solo"] = l.solo;
        lj["locked"] = l.locked; lj["fadeTime"] = l.fadeTime;
        lj["useRegion"] = l.useRegion; lj["region"] = rectJ(l.region);
        lj["clips"] = json::array();
        for (auto& cl : l.clips) {
            json cj;
            if (!cl.empty()) {
                cj["id"] = cl.id; cj["path"] = cl.path; cj["name"] = cl.name;
                cj["loop"] = (int)cl.loop; cj["speed"] = cl.speed; cj["opacity"] = cl.opacity;
                cj["fit"] = (int)cl.fit; cj["offset"] = {cl.offsetX, cl.offsetY}; cj["scale"] = cl.scale;
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
        sj["brightness"] = s.brightness; sj["testPattern"] = s.testPattern;
        sj["slices"] = json::array();
        for (auto& sl : s.slices) {
            json k;
            k["id"] = sl.id; k["name"] = sl.name; k["enabled"] = sl.enabled; k["sourceLayer"] = sl.sourceLayer;
            k["input"] = rectJ(sl.input);
            k["out"] = {sl.outX, sl.outY, sl.outW, sl.outH};
            k["rotation"] = sl.rotation; k["flipH"] = sl.flipH; k["flipV"] = sl.flipV;
            k["opacity"] = sl.opacity; k["brightness"] = sl.brightness; k["color"] = sl.color;
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
        get(c, "width", p.comp.width); get(c, "height", p.comp.height); get(c, "fps", p.comp.fps);
        get(c, "columns", p.comp.columns);
        for (auto& lj : c.value("layers", json::array())) {
            Layer l;
            get(lj, "id", l.id); get(lj, "name", l.name); get(lj, "opacity", l.opacity);
            int b = 0; get(lj, "blend", b); l.blend = (BlendMode)b;
            get(lj, "bypass", l.bypass); get(lj, "solo", l.solo); get(lj, "locked", l.locked);
            get(lj, "fadeTime", l.fadeTime); get(lj, "useRegion", l.useRegion);
            if (lj.contains("region")) l.region = rectFrom(lj["region"]);
            for (auto& cj : lj.value("clips", json::array())) {
                Clip cl;
                if (cj.is_object() && cj.contains("path")) {
                    get(cj, "id", cl.id); get(cj, "path", cl.path); get(cj, "name", cl.name);
                    int lm = 0; get(cj, "loop", lm); cl.loop = (LoopMode)lm;
                    get(cj, "speed", cl.speed); get(cj, "opacity", cl.opacity);
                    int f = 0; get(cj, "fit", f); cl.fit = (FitMode)f;
                    if (cj.contains("offset")) { cl.offsetX = cj["offset"][0]; cl.offsetY = cj["offset"][1]; }
                    get(cj, "scale", cl.scale);
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
            get(sj, "brightness", s.brightness); get(sj, "testPattern", s.testPattern);
            for (auto& k : sj.value("slices", json::array())) {
                Slice sl;
                get(k, "id", sl.id); get(k, "name", sl.name); get(k, "enabled", sl.enabled);
                get(k, "sourceLayer", sl.sourceLayer);
                if (k.contains("input")) sl.input = rectFrom(k["input"]);
                if (k.contains("out")) { auto& o = k["out"]; sl.outX = o[0]; sl.outY = o[1]; sl.outW = o[2]; sl.outH = o[3]; }
                get(k, "rotation", sl.rotation); get(k, "flipH", sl.flipH); get(k, "flipV", sl.flipV);
                get(k, "opacity", sl.opacity); get(k, "brightness", sl.brightness); get(k, "color", sl.color);
                s.slices.push_back(sl);
            }
            p.screens.push_back(s);
        }
        // validação defensiva
        p.comp.width  = std::clamp(p.comp.width, 16, 16384);
        p.comp.height = std::clamp(p.comp.height, 16, 16384);
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
