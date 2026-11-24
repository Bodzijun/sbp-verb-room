#include "SceneCommands.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace verbp {

namespace {
// ============================================================================================ equality / size helpers
bool eqTag(const verb::SurfaceTag& a, const verb::SurfaceTag& b) {
    return a.name == b.name && a.group == b.group && a.hidden == b.hidden && a.locked == b.locked;
}
bool eqMat(const verb::Material& a, const verb::Material& b) {
    return a.name == b.name && a.alpha == b.alpha && a.tau == b.tau && a.scatter == b.scatter && a.scatterHf == b.scatterHf && a.cls == b.cls;
}
bool eqWallGeom(const verb::Wall& a, const verb::Wall& b) {
    return a.ax == b.ax && a.ay == b.ay && a.bx == b.bx && a.by == b.by && a.z0 == b.z0 && a.z1 == b.z1;
}
bool eqSurf(const SurfaceSnap& a, const SurfaceSnap& b, bool ignoreTag) {
    if (a.kind != b.kind || a.hasMat != b.hasMat) return false;
    if (a.hasMat && !eqMat(a.mat, b.mat)) return false;
    if (!ignoreTag && !eqTag(a.tag, b.tag)) return false;
    if (a.kind == 1) return eqWallGeom(a.wall, b.wall);
    if (a.kind == 4 || a.kind == 5) return a.zone.poly == b.zone.poly && a.zone.z == b.zone.z;
    return true;
}
bool eqSource(const SourceCfg& a, const SourceCfg& b) {
    return a.name == b.name && a.color == b.color && a.format == b.format && a.channels == b.channels && a.firstIn == b.firstIn && a.sum == b.sum && a.spread == b.spread
           && a.pattern == b.pattern && a.instrument == b.instrument && a.instrumentAmount == b.instrumentAmount
           && a.aimAzDeg == b.aimAzDeg && a.aimElDeg == b.aimElDeg && a.aimRollDeg == b.aimRollDeg
           && a.customDirectivityData == b.customDirectivityData && a.customDirectivityName == b.customDirectivityName
           && a.customDirectivityPath == b.customDirectivityPath && a.directivityWarning == b.directivityWarning
           && a.freePoints == b.freePoints && a.ox == b.ox && a.oy == b.oy && a.oz == b.oz;
}
bool eqClutter(const verb::ClutterCfg& a, const verb::ClutterCfg& b) {
    return a.count == b.count && a.minLen == b.minLen && a.maxLen == b.maxLen && a.tlMin == b.tlMin && a.tlMax == b.tlMax && a.hMin == b.hMin
           && a.hMax == b.hMax && a.minDist == b.minDist && a.seed == b.seed;
}

size_t matBytes(const verb::Material& m) { return sizeof(verb::Material) + m.name.capacity(); }
size_t surfBytes(const SurfaceSnap& s) {
    return sizeof(SurfaceSnap) + matBytes(s.mat) + s.tag.name.capacity() + s.tag.group.capacity() + s.wall.editor.name.capacity()
           + s.wall.editor.group.capacity() + s.zone.poly.capacity() * sizeof(std::pair<double, double>) + s.zone.editor.name.capacity() + s.zone.editor.group.capacity();
}
size_t sourceBytes(const SourceCfg& c) {
    return sizeof(SourceCfg) + c.name.capacity() + c.color.capacity() + c.instrument.capacity() + c.customDirectivityData.capacity()
           + c.customDirectivityName.capacity() + c.customDirectivityPath.capacity() + c.directivityWarning.capacity()
           + (c.ox.capacity() + c.oy.capacity() + c.oz.capacity()) * sizeof(float);
}

SurfaceSnap surfaceSnap(const verb::Scene& s, int kind, int index) {
    SurfaceSnap r;
    r.kind = kind;
    auto setMat = [&](int m) {
        if (m >= 0 && m < (int)s.mats.size()) { r.mat = s.mats[(size_t)m]; r.hasMat = true; }
    };
    if (kind == 1) { r.wall = s.walls[(size_t)index]; r.tag = r.wall.editor; setMat(r.wall.mat); }
    else if (kind == 2) { r.tag = s.floorEditor; setMat(s.floorMat); }
    else if (kind == 3) { r.tag = s.ceilEditor; setMat(s.ceilMat); }
    else if (kind == 4) { r.zone = s.floorZones[(size_t)index]; r.tag = r.zone.editor; setMat(r.zone.mat); }
    else { r.zone = s.ceilZones[(size_t)index]; r.tag = r.zone.editor; setMat(r.zone.mat); }
    return r;
}

const std::vector<std::string>& modelParamIds() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v;
        for (int s = 0; s < kMaxSources; ++s) {
            for (int k = 0; k < 5; ++k) v.push_back(paramId(s, k).toStdString());
            v.push_back(sourceSwitchId(s, false).toStdString());
            v.push_back(sourceSwitchId(s, true).toStdString());
        }
        for (int k = 0; k < 4; ++k) v.push_back(paramId(kListener, k).toStdString());
        v.push_back("makeup");
        for(int o=0;o<=kMaxSources;++o)v.push_back(VerbProcessor::pathParamId(o).toStdString());
        return v;
    }();
    return ids;
}

float clampParam(VerbProcessor& proc, const std::string& id, double v) {
    if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(proc.apvts.getParameter(id))) {
        const auto& r = p->getNormalisableRange();
        return r.snapToLegalValue(r.getRange().clipValue((float)v));
    }
    return (float)v;
}

int parsePosAxis(const std::string& id) {
    if (id == "lx") return 0;
    if (id == "ly") return 1;
    if (id == "lz") return 2;
    if (id.size() >= 3 && id[0] == 's') {
        const char last = id.back();
        const int axis = last == 'x' ? 0 : (last == 'y' ? 1 : (last == 'z' ? 2 : -1));
        if (axis >= 0) {
            int n = 0;
            for (size_t i = 1; i + 1 < id.size(); ++i) {
                if (id[i] < '0' || id[i] > '9') return -1;
                n = n * 10 + (id[i] - '0');
            }
            if (n >= 1 && n <= kMaxSources) return axis;
        }
    }
    return -1;
}

float clampParamWithRange(const AutomationRange& ar, VerbProcessor& proc, const std::string& id, double v) {
    const int axis = parsePosAxis(id);
    if (axis >= 0) {
        const float lo = (float)ar.axisMin(axis), hi = (float)ar.axisMax(axis);
        const juce::NormalisableRange<float> r(lo, hi, 0.001f);
        return r.snapToLegalValue(r.getRange().clipValue((float)v));
    }
    return clampParam(proc, id, v);
}

void clampAllPositionParams(Model& m, VerbProcessor& proc) {
    for (int o = 0; o <= kMaxSources; ++o) {
        for (int k = 0; k < 3; ++k) {
            const std::string id = paramId(o, k).toStdString();
            auto it = m.params.find(id);
            const double cur = it != m.params.end() ? (double)it->second : (k == 2 ? 1.5 : 0.0);
            m.params[id] = clampParamWithRange(m.automationRange, proc, id, cur);
        }
    }
}

int ownMat(verb::Scene& s, SurfaceRef r) {  // as the editor's ownMaterialFor: a shared material is cloned first
    int* idx = nullptr;
    if (r.kind == 1 && r.index >= 0 && r.index < (int)s.walls.size()) idx = &s.walls[(size_t)r.index].mat;
    else if (r.kind == 2) idx = &s.floorMat;
    else if (r.kind == 3) idx = &s.ceilMat;
    else if (r.kind == 4 && r.index >= 0 && r.index < (int)s.floorZones.size()) idx = &s.floorZones[(size_t)r.index].mat;
    else if (r.kind == 5 && r.index >= 0 && r.index < (int)s.ceilZones.size()) idx = &s.ceilZones[(size_t)r.index].mat;
    if (!idx) return -1;
    if (*idx < 0) { s.mats.push_back(verb::surfacePresets()[2]); *idx = (int)s.mats.size() - 1; return *idx; }
    int users = 0;
    for (auto& w : s.walls) users += w.mat == *idx;
    users += s.floorMat == *idx; users += s.ceilMat == *idx;
    for (auto& z : s.floorZones) users += z.mat == *idx;
    for (auto& z : s.ceilZones) users += z.mat == *idx;
    if (users > 1) { verb::Material m = s.mats[(size_t)*idx]; s.mats.push_back(m); *idx = (int)s.mats.size() - 1; }
    return *idx;
}

int matIndexByValue(verb::Scene& s, const verb::Material& m) {
    for (size_t i = 0; i < s.mats.size(); ++i) if (eqMat(s.mats[i], m)) return (int)i;
    s.mats.push_back(m);
    return (int)s.mats.size() - 1;
}

// ============================================================================================ applying keyed changes
// Returns the number of skipped targets (index no longer exists).
int applySceneTargets(verb::Scene& s, const std::vector<Target>& targets, bool tagOnly) {
    int skipped = 0;
    auto matFor = [&](const SurfaceSnap& n) { return n.hasMat ? matIndexByValue(s, n.mat) : -1; };
    for (const auto& t : targets) {
        switch (t.key.type) {
        case KeyType::Surface: {
            const auto& n = std::get<SurfaceSnap>(*t.snap);
            const int i = t.key.index;
            if (n.kind == 1) {
                if (i < 0 || i >= (int)s.walls.size()) { ++skipped; break; }
                auto& w = s.walls[(size_t)i];
                if (tagOnly) { w.editor = n.tag; break; }
                const int m = matFor(n);
                w = n.wall; w.editor = n.tag; w.mat = m;
            } else if (n.kind == 2) {
                s.floorEditor = n.tag;
                if (!tagOnly) s.floorMat = matFor(n);
            } else if (n.kind == 3) {
                s.ceilEditor = n.tag;
                if (!tagOnly) s.ceilMat = matFor(n);
            } else {
                auto& list = n.kind == 4 ? s.floorZones : s.ceilZones;
                if (i < 0 || i >= (int)list.size()) { ++skipped; break; }
                if (tagOnly) { list[(size_t)i].editor = n.tag; break; }
                const int m = matFor(n);
                list[(size_t)i] = n.zone; list[(size_t)i].editor = n.tag; list[(size_t)i].mat = m;
            }
            break;
        }
        case KeyType::SurfaceList: {
            const auto& v = std::get<std::vector<SurfaceSnap>>(*t.snap);
            if (t.key.kind == 1) {
                std::vector<verb::Wall> walls;
                for (const auto& n : v) { verb::Wall w = n.wall; w.editor = n.tag; w.mat = matFor(n); walls.push_back(w); }
                s.walls = walls;
            } else {
                std::vector<verb::Zone> zones;
                for (const auto& n : v) { verb::Zone z = n.zone; z.editor = n.tag; z.mat = matFor(n); zones.push_back(z); }
                (t.key.kind == 4 ? s.floorZones : s.ceilZones) = zones;
            }
            break;
        }
        case KeyType::Height: s.height = std::get<HeightSnap>(*t.snap).h; s.ceilingInfinite = std::get<HeightSnap>(*t.snap).ceilingInfinite; break;
        case KeyType::Air: s.tempC = std::get<AirSnap>(*t.snap).tempC; s.humidity = std::get<AirSnap>(*t.snap).humidity; break;
        case KeyType::Clutter: s.clutter = std::get<verb::ClutterCfg>(*t.snap); break;
        case KeyType::Rough: s.roughness = std::get<RoughSnap>(*t.snap).roughness; s.roughSeed = std::get<RoughSnap>(*t.snap).seed; break;
        case KeyType::Diffraction: s.diffraction = std::get<DiffSnap>(*t.snap).on; break;
        default: break;
        }
    }
    return skipped;
}

bool isSceneKey(KeyType t) {
    return t == KeyType::Surface || t == KeyType::SurfaceList || t == KeyType::Height || t == KeyType::Air || t == KeyType::Clutter
           || t == KeyType::Rough || t == KeyType::Diffraction;
}

bool structuralEqual(const SourceCfg& a, const SourceCfg& b) {
    return a.name == b.name && a.format == b.format && a.channels == b.channels && a.firstIn == b.firstIn && a.sum == b.sum && a.freePoints == b.freePoints;
}

void applySources(VerbProcessor& proc, const std::vector<SourceCfg>& cur, const std::vector<SourceCfg>& tgt) {
    bool live = cur.size() == tgt.size();
    for (size_t i = 0; live && i < cur.size(); ++i)
        live = structuralEqual(cur[i], tgt[i]) && cur[i].ox.size() == tgt[i].ox.size() && cur[i].oy.size() == tgt[i].oy.size()
               && cur[i].oz.size() == tgt[i].oz.size();
    if (!live) { proc.setSources(tgt); return; }  // a structural change: the engine is rebuilt in the background
    for (size_t i = 0; i < cur.size(); ++i) {
        if (cur[i].color != tgt[i].color) proc.setSourceColor((int)i, tgt[i].color);
        if (cur[i].pattern != tgt[i].pattern) proc.setSourcePattern((int)i, tgt[i].pattern);
        if (cur[i].instrument != tgt[i].instrument || cur[i].instrumentAmount != tgt[i].instrumentAmount
            || cur[i].aimAzDeg != tgt[i].aimAzDeg || cur[i].aimElDeg != tgt[i].aimElDeg || cur[i].aimRollDeg != tgt[i].aimRollDeg
            || cur[i].customDirectivityData != tgt[i].customDirectivityData
            || cur[i].customDirectivityName != tgt[i].customDirectivityName
            || cur[i].customDirectivityPath != tgt[i].customDirectivityPath)
            proc.setSourceDirectivity((int)i, tgt[i].instrument, tgt[i].instrumentAmount,
                                      tgt[i].customDirectivityData, tgt[i].customDirectivityName, tgt[i].customDirectivityPath,
                                      tgt[i].aimAzDeg, tgt[i].aimElDeg, tgt[i].aimRollDeg);
        if (cur[i].spread != tgt[i].spread) proc.setSourceSpread((int)i, tgt[i].spread);
        for (size_t ch = 0; ch < tgt[i].ox.size(); ++ch)
            if (cur[i].ox[ch] != tgt[i].ox[ch] || cur[i].oy[ch] != tgt[i].oy[ch] || cur[i].oz[ch] != tgt[i].oz[ch])
                proc.setPointOffset((int)i, (int)ch, tgt[i].ox[ch], tgt[i].oy[ch], tgt[i].oz[ch]);
    }
}
}  // namespace

// ============================================================================================ public helpers
std::string Key::path() const {
    switch (type) {
    case KeyType::Surface:
        if (kind == 1) return "document.scene.walls[" + std::to_string(index) + "]";
        if (kind == 2) return "document.scene.floor";
        if (kind == 3) return "document.scene.ceiling";
        return std::string("document.scene.") + (kind == 4 ? "floorZones[" : "ceilingZones[") + std::to_string(index) + "]";
    case KeyType::SurfaceList: return kind == 1 ? "document.scene.walls" : kind == 4 ? "document.scene.floorZones" : "document.scene.ceilingZones";
    case KeyType::Height: return "document.scene.heightM";
    case KeyType::Air: return "document.scene.air";
    case KeyType::Clutter: return "document.scene.clutter";
    case KeyType::Rough: return "document.scene.roughness";
    case KeyType::Diffraction: return "document.scene.diffraction";
    case KeyType::Source: return "document.sources[" + std::to_string(index) + "]";
    case KeyType::SourceList: return "document.sources";
    case KeyType::Output: return "document.output";
    case KeyType::Tail: return "document.tail";
    case KeyType::Path: return "document.paths["+std::to_string(index)+"]";
    case KeyType::AutomationRange: return "document.scene.automationRange";
    case KeyType::Modulation: return "document.modulation";
    case KeyType::Param: return "parameters." + id;
    }
    return {};
}

Model readModel(VerbProcessor& p) {
    Model m;
    for(int o=0;o<=kMaxSources;++o)m.paths[(size_t)o]=p.getMovementPath(o);
    m.modRoutes = p.getModulationRoutes();
    m.scene = p.getDisplayScene();
    m.sources = p.getSources();
    m.output = p.getOutputCfg();
    m.tail = p.getTailSettings();
    m.automationRange = p.getAutomationRange();
    for (const auto& id : modelParamIds())
        if (auto* v = p.apvts.getRawParameterValue(id)) m.params[id] = v->load();
    return m;
}

void normalizeScene(verb::Scene& s) {
    verb::sanitizeScene(s);
    verb::compactMaterials(s);
    s.height = std::min(std::max(s.height, 1.0), 30.0);
}

std::vector<KeyChange> diffModels(const Model& a, const Model& b) {
    std::vector<KeyChange> out;
    auto add = [&](Key k, Snap before, Snap after, bool acoustic) { out.push_back({k, std::move(before), std::move(after), acoustic}); };
    const verb::Scene& sa = a.scene;
    const verb::Scene& sb = b.scene;
    auto surfaces = [&](int kind, size_t na, size_t nb) {
        if (na != nb) {
            std::vector<SurfaceSnap> va, vb;
            for (size_t i = 0; i < na; ++i) va.push_back(surfaceSnap(sa, kind, (int)i));
            for (size_t i = 0; i < nb; ++i) vb.push_back(surfaceSnap(sb, kind, (int)i));
            Key k; k.type = KeyType::SurfaceList; k.kind = kind;
            add(k, va, vb, true);
            return;
        }
        for (size_t i = 0; i < na; ++i) {
            const SurfaceSnap x = surfaceSnap(sa, kind, (int)i), y = surfaceSnap(sb, kind, (int)i);
            if (eqSurf(x, y, false)) continue;
            Key k; k.type = KeyType::Surface; k.kind = kind; k.index = (int)i;
            add(k, x, y, !eqSurf(x, y, true));
        }
    };
    for (int kind : {2, 3}) {
        const SurfaceSnap x = surfaceSnap(sa, kind, 0), y = surfaceSnap(sb, kind, 0);
        if (eqSurf(x, y, false)) continue;
        Key k; k.type = KeyType::Surface; k.kind = kind;
        add(k, x, y, !eqSurf(x, y, true));
    }
    surfaces(1, sa.walls.size(), sb.walls.size());
    surfaces(4, sa.floorZones.size(), sb.floorZones.size());
    surfaces(5, sa.ceilZones.size(), sb.ceilZones.size());
    if (sa.height != sb.height || sa.ceilingInfinite != sb.ceilingInfinite) { Key k; k.type = KeyType::Height; add(k, HeightSnap{sa.height,sa.ceilingInfinite}, HeightSnap{sb.height,sb.ceilingInfinite}, true); }
    if (sa.tempC != sb.tempC || sa.humidity != sb.humidity) { Key k; k.type = KeyType::Air; add(k, AirSnap{sa.tempC, sa.humidity}, AirSnap{sb.tempC, sb.humidity}, true); }
    if (!eqClutter(sa.clutter, sb.clutter)) { Key k; k.type = KeyType::Clutter; add(k, sa.clutter, sb.clutter, true); }
    if (sa.roughness != sb.roughness || sa.roughSeed != sb.roughSeed) { Key k; k.type = KeyType::Rough; add(k, RoughSnap{sa.roughness, sa.roughSeed}, RoughSnap{sb.roughness, sb.roughSeed}, true); }
    if (sa.diffraction != sb.diffraction) { Key k; k.type = KeyType::Diffraction; add(k, DiffSnap{sa.diffraction}, DiffSnap{sb.diffraction}, true); }
    if (a.sources.size() != b.sources.size()) {
        Key k; k.type = KeyType::SourceList; add(k, a.sources, b.sources, true);
    } else {
        for (size_t i = 0; i < a.sources.size(); ++i)
            if (!eqSource(a.sources[i], b.sources[i])) { Key k; k.type = KeyType::Source; k.index = (int)i; add(k, a.sources[i], b.sources[i], true); }
    }
    if (!(a.output == b.output)) {
        Key k; k.type = KeyType::Output; add(k, a.output, b.output, true);
    }
    if (a.tail.rays != b.tail.rays || a.tail.adaptive != b.tail.adaptive) { Key k; k.type = KeyType::Tail; add(k, a.tail, b.tail, true); }
    if (a.automationRange != b.automationRange) { Key k; k.type = KeyType::AutomationRange; add(k, a.automationRange, b.automationRange, false); }
    for (const auto& kv : b.params) {
        auto it = a.params.find(kv.first);
        if (it == a.params.end() || it->second != kv.second) {
            Key k; k.type = KeyType::Param; k.id = kv.first;
            add(k, it == a.params.end() ? 0.f : it->second, kv.second, true);
        }
    }
    for(int o=0;o<=kMaxSources;++o)if(!(a.paths[(size_t)o]==b.paths[(size_t)o])) {Key k;k.type=KeyType::Path;k.index=o;add(k,a.paths[(size_t)o],b.paths[(size_t)o],false);}
    if (a.modRoutes != b.modRoutes) { Key k; k.type = KeyType::Modulation; add(k, a.modRoutes, b.modRoutes, false); }
    return out;
}

std::optional<Snap> captureKey(const Model& m, const Key& k) {
    const verb::Scene& s = m.scene;
    switch (k.type) {
    case KeyType::Surface: {
        if (k.kind == 1 && (k.index < 0 || k.index >= (int)s.walls.size())) return std::nullopt;
        if (k.kind == 4 && (k.index < 0 || k.index >= (int)s.floorZones.size())) return std::nullopt;
        if (k.kind == 5 && (k.index < 0 || k.index >= (int)s.ceilZones.size())) return std::nullopt;
        return surfaceSnap(s, k.kind, k.index);
    }
    case KeyType::SurfaceList: {
        std::vector<SurfaceSnap> v;
        const size_t n = k.kind == 1 ? s.walls.size() : k.kind == 4 ? s.floorZones.size() : s.ceilZones.size();
        for (size_t i = 0; i < n; ++i) v.push_back(surfaceSnap(s, k.kind, (int)i));
        return Snap(v);
    }
    case KeyType::Height: return Snap(HeightSnap{s.height,s.ceilingInfinite});
    case KeyType::Air: return Snap(AirSnap{s.tempC, s.humidity});
    case KeyType::Clutter: return Snap(s.clutter);
    case KeyType::Rough: return Snap(RoughSnap{s.roughness, s.roughSeed});
    case KeyType::Diffraction: return Snap(DiffSnap{s.diffraction});
    case KeyType::Source:
        if (k.index < 0 || k.index >= (int)m.sources.size()) return std::nullopt;
        return Snap(m.sources[(size_t)k.index]);
    case KeyType::Path: return Snap(m.paths[(size_t)k.index]);
    case KeyType::SourceList: return Snap(m.sources);
    case KeyType::Output: return Snap(m.output);
    case KeyType::Tail: return Snap(m.tail);
    case KeyType::AutomationRange: return Snap(m.automationRange);
    case KeyType::Modulation: return Snap(m.modRoutes);
    case KeyType::Param: {
        auto it = m.params.find(k.id);
        if (it == m.params.end()) return std::nullopt;
        return Snap(it->second);
    }
    }
    return std::nullopt;
}

bool snapsEqual(const Snap& a, const Snap& b) {
    if (a.index() != b.index()) return false;
    if (auto* x = std::get_if<SurfaceSnap>(&a)) return eqSurf(*x, std::get<SurfaceSnap>(b), false);
    if (auto* x = std::get_if<std::vector<SurfaceSnap>>(&a)) {
        const auto& y = std::get<std::vector<SurfaceSnap>>(b);
        if (x->size() != y.size()) return false;
        for (size_t i = 0; i < x->size(); ++i) if (!eqSurf((*x)[i], y[i], false)) return false;
        return true;
    }
    if (auto* x = std::get_if<HeightSnap>(&a)) return x->h == std::get<HeightSnap>(b).h && x->ceilingInfinite == std::get<HeightSnap>(b).ceilingInfinite;
    if (auto* x = std::get_if<AirSnap>(&a)) return x->tempC == std::get<AirSnap>(b).tempC && x->humidity == std::get<AirSnap>(b).humidity;
    if (auto* x = std::get_if<verb::ClutterCfg>(&a)) return eqClutter(*x, std::get<verb::ClutterCfg>(b));
    if (auto* x = std::get_if<RoughSnap>(&a)) return x->roughness == std::get<RoughSnap>(b).roughness && x->seed == std::get<RoughSnap>(b).seed;
    if (auto* x = std::get_if<DiffSnap>(&a)) return x->on == std::get<DiffSnap>(b).on;
    if (auto* x = std::get_if<SourceCfg>(&a)) return eqSource(*x, std::get<SourceCfg>(b));
    if (auto* x = std::get_if<std::vector<SourceCfg>>(&a)) {
        const auto& y = std::get<std::vector<SourceCfg>>(b);
        if (x->size() != y.size()) return false;
        for (size_t i = 0; i < x->size(); ++i) if (!eqSource((*x)[i], y[i])) return false;
        return true;
    }
    if (auto* x = std::get_if<OutputCfg>(&a)) { const auto& y = std::get<OutputCfg>(b); return *x == y; }
    if (auto* x = std::get_if<TailSettings>(&a)) return x->rays == std::get<TailSettings>(b).rays && x->adaptive == std::get<TailSettings>(b).adaptive;
    if (auto* x = std::get_if<AutomationRange>(&a)) return *x == std::get<AutomationRange>(b);
    if (auto* x = std::get_if<MovementPath>(&a)) return *x==std::get<MovementPath>(b);
    if (auto* x = std::get_if<std::vector<ModulationRoute>>(&a)) return *x == std::get<std::vector<ModulationRoute>>(b);
    if (auto* x = std::get_if<float>(&a)) return std::fabs(*x - std::get<float>(b)) < 1e-4f;  // parameters are quantised to 0.001
    return false;
}

size_t snapBytes(const Snap& s) {
    if(auto* x=std::get_if<MovementPath>(&s))return sizeof(*x)+x->points.capacity()*sizeof(PathPoint)+x->times.capacity()*sizeof(std::optional<double>)+x->easings.capacity()*sizeof(PathEasing)+x->azKeys.capacity()*sizeof(AzimuthKey);
    if (auto* x = std::get_if<std::vector<ModulationRoute>>(&s)) return sizeof(*x) + x->capacity() * sizeof(ModulationRoute);
    if (auto* x = std::get_if<SurfaceSnap>(&s)) return surfBytes(*x);
    if (auto* x = std::get_if<std::vector<SurfaceSnap>>(&s)) { size_t n = sizeof(*x); for (auto& e : *x) n += surfBytes(e); return n; }
    if (auto* x = std::get_if<SourceCfg>(&s)) return sourceBytes(*x);
    if (auto* x = std::get_if<std::vector<SourceCfg>>(&s)) { size_t n = sizeof(*x); for (auto& e : *x) n += sourceBytes(e); return n; }
    return sizeof(Snap);
}

std::vector<std::string> changedPaths(const std::vector<KeyChange>& c) {
    std::vector<std::string> v;
    for (const auto& k : c) v.push_back(k.key.path());
    return v;
}

bool indicesMoved(const std::vector<KeyChange>& c) {
    for (const auto& k : c) if (k.key.isList()) return true;
    return false;
}

Fail applyTargets(VerbProcessor& proc, const std::vector<Target>& targets, int* conflicts) {
    int skipped = 0;
    // 0. automationRange first so subsequent parameter updates convert against the target range
    for (const auto& t : targets)
        if (t.key.type == KeyType::AutomationRange)
            proc.setAutomationRange(std::get<AutomationRange>(*t.snap), false, true);
    // 1. parameters (a source added / removed moves the parameters of the later sources, like the native editor does)
    for (const auto& t : targets)
        if (t.key.type == KeyType::Param)
            if (auto* p = proc.apvts.getParameter(t.key.id)) {
                if(t.key.id=="makeup")p->beginChangeGesture();
                p->setValueNotifyingHost(p->convertTo0to1(std::get<float>(*t.snap)));
                if(t.key.id=="makeup")p->endChangeGesture();
            }
    // 2. the scene, in one call
    bool anyScene = false, anyAcoustic = false;
    for (const auto& t : targets)
        if (isSceneKey(t.key.type)) { anyScene = true; anyAcoustic |= t.acoustic; }
    if (anyScene) {
        int s = 0;
        const bool ok = anyAcoustic ? proc.editScene([&](verb::Scene& sc) { s = applySceneTargets(sc, targets, false); })
                                    : proc.editSceneMetadata([&](verb::Scene& sc) { s = applySceneTargets(sc, targets, true); });
        if (!ok) return {"notReady", proc.isRoomLoadPending() ? "A room is loading; the edit was discarded." : "The scene edit was rejected.", ""};
        skipped += s;
    }
    // 3. sources
    {
        std::vector<SourceCfg> cur = proc.getSources(), tgt = cur;
        bool any = false;
        for (const auto& t : targets) {
            if (t.key.type == KeyType::Source) {
                any = true;
                if (t.key.index < (int)tgt.size()) tgt[(size_t)t.key.index] = std::get<SourceCfg>(*t.snap); else ++skipped;
            } else if (t.key.type == KeyType::SourceList) {
                any = true;
                tgt = std::get<std::vector<SourceCfg>>(*t.snap);
            }
        }
        if (any) applySources(proc, cur, tgt);
    }
    // 4. output and tail quality
    for (const auto& t : targets) {
        if (t.key.type == KeyType::Output) proc.setOutputCfg(std::get<OutputCfg>(*t.snap));
        else if (t.key.type == KeyType::Tail) proc.setTailSettings(std::get<TailSettings>(*t.snap));
    }
    for(const auto& t:targets)if(t.key.type==KeyType::Path)proc.setMovementPath(t.key.index,std::get<MovementPath>(*t.snap));
    for (const auto& t : targets) if (t.key.type == KeyType::Modulation) proc.setModulationRoutes(std::get<std::vector<ModulationRoute>>(*t.snap));
    if (conflicts) *conflicts = skipped;
    return {};
}

Fail normalizeAndValidate(Model& cand, const Model& before) {
    auto plates = [](const verb::Scene& s) { int n = 0; for (auto* l : {&s.floorZones, &s.ceilZones}) for (auto& z : *l) n += z.z >= 0; return n; };
    auto dropped = [&](const verb::Scene& pre, const verb::Scene& post) {
        return std::array<int, 4>{(int)pre.walls.size() - (int)post.walls.size(), (int)pre.floorZones.size() - (int)post.floorZones.size(),
                                  (int)pre.ceilZones.size() - (int)post.ceilZones.size(), plates(pre) - plates(post)};
    };
    verb::Scene b = before.scene;
    normalizeScene(b);
    const auto baseDrop = dropped(before.scene, b);
    verb::Scene pre = cand.scene;
    normalizeScene(cand.scene);
    const auto drop = dropped(pre, cand.scene);
    for (size_t i = 0; i < 4; ++i)
        if (drop[i] > baseDrop[i])
            return {"invalid", "The scene sanitiser would silently change this edit (zero-length wall, invalid zone, or more than 8 plates).", ""};
    if (cand.scene.walls.size() > 128) return {"budgetExceeded", "At most 128 walls.", "walls"};
    return {};
}

// ============================================================================================ argument helpers
namespace {
struct Args {
    const juce::var& v;
    Fail f;
    explicit Args(const juce::var& x) : v(x) {}
    bool bad() const { return f.bad(); }
    void err(const char* code, const std::string& msg, const std::string& field) { if (!f.bad()) f = {code, msg, field}; }
    bool has(const char* k) const { auto* o = v.getDynamicObject(); return o && o->hasProperty(k); }
    juce::var get(const char* k) const { return v.getProperty(k, {}); }
    bool isNull(const char* k) const { return has(k) && get(k).isVoid(); }
    static bool isNum(const juce::var& x) { return x.isInt() || x.isInt64() || x.isDouble(); }
    std::optional<double> optNum(const char* k, double lo, double hi) {
        if (!has(k)) return std::nullopt;
        const juce::var x = get(k);
        if (!isNum(x)) { err("invalid", std::string(k) + " must be a number", k); return std::nullopt; }
        const double d = (double)x;
        if (!std::isfinite(d) || d < lo || d > hi) { err("invalid", std::string(k) + " is out of range", k); return std::nullopt; }
        return d;
    }
    double num(const char* k, double lo, double hi) {
        auto r = optNum(k, lo, hi);
        if (!r && !bad()) err("invalid", std::string(k) + " is required", k);
        return r ? *r : 0.0;
    }
    std::optional<int> optInt(const char* k, int lo, int hi) {
        if (!has(k)) return std::nullopt;
        const juce::var x = get(k);
        if (!(x.isInt() || x.isInt64() || (x.isDouble() && std::floor((double)x) == (double)x))) { err("invalid", std::string(k) + " must be an integer", k); return std::nullopt; }
        const double d = (double)x;
        if (d < lo || d > hi) { err("invalid", std::string(k) + " is out of range", k); return std::nullopt; }
        return (int)d;
    }
    int integer(const char* k, int lo, int hi) {
        auto r = optInt(k, lo, hi);
        if (!r && !bad()) err("invalid", std::string(k) + " is required", k);
        return r ? *r : 0;
    }
    std::optional<bool> optBool(const char* k) {
        if (!has(k)) return std::nullopt;
        const juce::var x = get(k);
        if (!x.isBool()) { err("invalid", std::string(k) + " must be a boolean", k); return std::nullopt; }
        return (bool)x;
    }
    bool boolean(const char* k) {
        auto r = optBool(k);
        if (!r && !bad()) err("invalid", std::string(k) + " is required", k);
        return r ? *r : false;
    }
    std::optional<std::string> optStr(const char* k, size_t maxLen) {
        if (!has(k)) return std::nullopt;
        const juce::var x = get(k);
        if (!x.isString()) { err("invalid", std::string(k) + " must be a string", k); return std::nullopt; }
        const std::string s = x.toString().toStdString();
        if (juce::String(s).length() > (int)maxLen) { err("invalid", std::string(k) + " is too long", k); return std::nullopt; }
        return s;
    }
    // [x, y] or [x, y, z]
    bool vec(const juce::var& x, size_t nMin, size_t nMax, double lim, std::vector<double>& out, const std::string& field) {
        auto* arr = x.getArray();
        if (!arr || arr->size() < (int)nMin || arr->size() > (int)nMax) { err("invalid", field + " must be an array of numbers", field); return false; }
        out.clear();
        for (const auto& e : *arr) {
            if (!isNum(e) || !std::isfinite((double)e) || std::fabs((double)e) > lim) { err("invalid", field + " holds a non-finite or out-of-range number", field); return false; }
            out.push_back((double)e);
        }
        return true;
    }
    std::vector<double> vecProp(const char* k, size_t nMin, size_t nMax, double lim) {
        std::vector<double> r;
        if (!has(k)) { err("invalid", std::string(k) + " is required", k); return r; }
        vec(get(k), nMin, nMax, lim, r, k);
        return r;
    }
};

int kindFromName(const juce::String& n) {
    if (n == "wall") return 1;
    if (n == "floor") return 2;
    if (n == "ceiling") return 3;
    if (n == "floorZone") return 4;
    if (n == "ceilingZone") return 5;
    return 0;
}

bool validRef(const verb::Scene& s, SurfaceRef r) {
    if (r.kind == 1) return r.index >= 0 && r.index < (int)s.walls.size();
    if (r.kind == 2 || r.kind == 3) return true;
    if (r.kind == 4) return r.index >= 0 && r.index < (int)s.floorZones.size();
    if (r.kind == 5) return r.index >= 0 && r.index < (int)s.ceilZones.size();
    return false;
}

std::optional<SurfaceRef> parseRef(Args& a, const juce::var& x, const verb::Scene& s, const std::string& field) {
    if (!x.getDynamicObject()) { a.err("invalid", field + " must be an object {kind,index}", field); return std::nullopt; }
    const juce::var k = x.getProperty("kind", {}), i = x.getProperty("index", {});
    SurfaceRef r;
    r.kind = k.isString() ? kindFromName(k.toString()) : 0;
    if (r.kind == 0 || !(i.isInt() || i.isInt64())) { a.err("invalid", field + " needs a valid kind and an integer index", field); return std::nullopt; }
    const int idx = (int)i;
    if (r.kind == 2 || r.kind == 3) {
        if (idx != 0) { a.err("invalid", "the base floor / ceiling use index 0", field); return std::nullopt; }
        r.index = -1;
    } else {
        r.index = idx;
    }
    if (!validRef(s, r)) { a.err("invalid", field + " refers to a surface that does not exist", field); return std::nullopt; }
    return r;
}

std::vector<SurfaceRef> parseRefs(Args& a, const char* key, const verb::Scene& s, bool allowBase) {
    std::vector<SurfaceRef> out;
    if (!a.has(key)) { a.err("invalid", std::string(key) + " is required", key); return out; }
    auto* arr = a.get(key).getArray();
    if (!arr || arr->isEmpty() || arr->size() > 194) { a.err("invalid", std::string(key) + " must hold 1..194 surface references", key); return out; }
    int n = 0;
    for (const auto& e : *arr) {
        auto r = parseRef(a, e, s, std::string(key) + "[" + std::to_string(n++) + "]");
        if (!r) return {};
        if (!allowBase && (r->kind == 2 || r->kind == 3)) { a.err("invalid", "the base floor / ceiling cannot be used here", key); return {}; }
        if (std::find(out.begin(), out.end(), *r) == out.end()) out.push_back(*r);
    }
    return out;
}

Fail lockedFail(const std::string& what) { return {"locked", what + " is locked; unlock it first.", "refs"}; }

Fail checkLocked(const verb::Scene& s, const std::vector<SurfaceRef>& refs, bool expandGroups) {
    if (surfacesLocked(s, expandGroups ? expandSurfaceGroups(s, refs) : refs)) return lockedFail("A surface of this selection");
    return {};
}

bool anyLockedSurface(const verb::Scene& s) {
    for (auto r : surfaceRefs(s)) if (auto* t = surfaceTag(s, r)) if (t->locked) return true;
    return false;
}

// Simple-polygon test on the plan view: false when two non-adjacent edges cross.
bool segCross(double ax, double ay, double bx, double by, double cx, double cy, double dx, double dy) {
    auto o = [](double px, double py, double qx, double qy, double rx, double ry) { return (qx - px) * (ry - py) - (qy - py) * (rx - px); };
    const double d1 = o(ax, ay, bx, by, cx, cy), d2 = o(ax, ay, bx, by, dx, dy), d3 = o(cx, cy, dx, dy, ax, ay), d4 = o(cx, cy, dx, dy, bx, by);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}
bool selfIntersects(const std::vector<std::pair<double, double>>& p) {
    const size_t n = p.size();
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            if (j == i + 1 || (i == 0 && j == n - 1)) continue;
            if (segCross(p[i].first, p[i].second, p[(i + 1) % n].first, p[(i + 1) % n].second, p[j].first, p[j].second, p[(j + 1) % n].first, p[(j + 1) % n].second)) return true;
        }
    return false;
}
double polyArea(const std::vector<std::pair<double, double>>& p) {
    double a = 0;
    for (size_t i = 0; i < p.size(); ++i) { const auto& u = p[i]; const auto& w = p[(i + 1) % p.size()]; a += u.first * w.second - w.first * u.second; }
    return std::fabs(a) * 0.5;
}

int platesIn(const verb::Scene& s) { int n = 0; for (auto* l : {&s.floorZones, &s.ceilZones}) for (auto& z : *l) n += z.z >= 0; return n; }

void clampBands(verb::Material& m) {
    for (int b = 0; b < verb::NB; ++b) {
        m.alpha[(size_t)b] = std::min(1.0, std::max(0.0, m.alpha[(size_t)b]));
        m.tau[(size_t)b] = std::min(1.0, std::max(0.0, m.tau[(size_t)b]));
        if (m.alpha[(size_t)b] + m.tau[(size_t)b] > 1.0) m.alpha[(size_t)b] = 1.0 - m.tau[(size_t)b];
    }
}

std::string refsKey(const juce::var& p) { return juce::JSON::toString(p.getProperty("refs", {}), true).toStdString(); }

// ============================================================================================ surface commands
constexpr double wallCornerTolerance = 1e-4;
using CornerXY = std::pair<double, double>;
CornerXY wallEnd(const verb::Wall& wall, int vertex) {
    return vertex == 0 ? CornerXY{wall.ax, wall.ay} : CornerXY{wall.bx, wall.by};
}
void setWallEnd(verb::Wall& wall, int vertex, CornerXY xy) {
    (vertex == 0 ? wall.ax : wall.bx) = xy.first;
    (vertex == 0 ? wall.ay : wall.by) = xy.second;
}
double cornerDistance(CornerXY a, CornerXY b) {
    return std::hypot(a.first - b.first, a.second - b.second);
}
struct CornerRef { SurfaceRef ref; int vertex; CornerXY xy; };

std::optional<CornerRef> parseCorner(Args& a, const char* field, const verb::Scene& scene) {
    if (!a.has(field)) return {};
    const auto value = a.get(field);
    auto ref = parseRef(a, value, scene, field);
    if (!ref) return {};
    Args vertexArgs(value);
    const int vertex = vertexArgs.integer("vertex", 0, 63);
    if (!vertexArgs.bad()) {
        if (ref->kind == 1 && vertex < 2) return CornerRef{*ref, vertex, wallEnd(scene.walls[(size_t)ref->index], vertex)};
        if (ref->kind == 4 || ref->kind == 5) {
            const auto& zone = (ref->kind == 4 ? scene.floorZones : scene.ceilZones)[(size_t)ref->index];
            if ((size_t)vertex < zone.poly.size()) return CornerRef{*ref, vertex, zone.poly[(size_t)vertex]};
        }
    }
    a.err("invalid", "Expected an existing wall or zone vertex", field);
    return {};
}
bool linkableWall(const verb::Scene& scene, const verb::Wall& wall) {
    return scene.layerModes[0] == 0 && !wall.editor.hidden && !wall.editor.locked;
}

Fail opTranslate(const juce::var& p, Model& m, const Ctx& c) {
    Args a(p);
    auto refs = parseRefs(a, "refs", m.scene, true);
    auto d = a.vecProp("deltaM", 2, 3, 1e5);
    const bool groups = a.optBool("includeGroups").value_or(true);
    const bool autoLink = a.optBool("autoLink").value_or(false);
    const auto& start = c.start.scene;
    const auto snapFrom = parseCorner(a, "snapFrom", start);
    const auto snapTarget = parseCorner(a, "snapTarget", start);
    if (a.bad()) return a.f;
    if (auto f = checkLocked(m.scene, refs, groups); f.bad()) return f;
    const auto expanded = groups ? expandSurfaceGroups(m.scene, refs) : refs;
    if (snapFrom || snapTarget) {
        if (!snapFrom || !snapTarget || snapFrom->ref.kind != 1 ||
            std::find(expanded.begin(), expanded.end(), snapFrom->ref) == expanded.end() ||
            std::find(expanded.begin(), expanded.end(), snapTarget->ref) != expanded.end())
            return {"invalid", "Snap needs a moved wall end and a stationary corner", "snapTarget"};
        const CornerXY delta{snapTarget->xy.first - snapFrom->xy.first, snapTarget->xy.second - snapFrom->xy.second};
        if (cornerDistance(delta, {d[0], d[1]}) > 1e-9)
            return {"invalid", "Snap corners do not match the translation", "deltaM"};
        d[0] = delta.first;
        d[1] = delta.second;
    }
    translateSurfaces(m.scene, start, expanded, d[0], d[1], d.size() > 2 ? d[2] : 0.0);
    if (!autoLink && !snapFrom) return {};
    const auto exactSnap = [&](const verb::Wall& from, verb::Wall& wall, int vertex) {
        if (snapFrom && wallEnd(from, vertex) == snapFrom->xy) setWallEnd(wall, vertex, snapTarget->xy);
    };

    std::vector<bool> moved(start.walls.size());
    std::vector<std::pair<double, double>> corners;
    for (auto r : expanded) {
        if (r.kind != 1 || (size_t)r.index >= start.walls.size()) continue;
        const auto& wall = start.walls[(size_t)r.index];
        moved[(size_t)r.index] = true;
        corners.emplace_back(wall.ax, wall.ay);
        corners.emplace_back(wall.bx, wall.by);
        if (snapFrom) {
            auto& after = m.scene.walls[(size_t)r.index];
            exactSnap(wall, after, 0);
            exactSnap(wall, after, 1);
            if (cornerDistance(wallEnd(after, 0), wallEnd(after, 1)) < wallCornerTolerance)
                return {"invalid", "the move would make a wall of zero length", "deltaM"};
        }
    }
    if (!autoLink || corners.empty() || (d[0] == 0 && d[1] == 0)) return {};
    const auto linked = [&](double x, double y) {
        return std::any_of(corners.begin(), corners.end(), [&](const auto& corner) {
            return std::hypot(x - corner.first, y - corner.second) <= wallCornerTolerance;
        });
    };
    for (size_t i = 0; i < start.walls.size() && i < m.scene.walls.size(); ++i) {
        const auto& from = start.walls[i];
        auto& wall = m.scene.walls[i];
        if (moved[i] || !linkableWall(start, from) || !linkableWall(m.scene, wall)) continue;
        const bool moveA = linked(from.ax, from.ay), moveB = linked(from.bx, from.by);
        if (!moveA && !moveB) continue;
        if (snapTarget && snapTarget->ref.kind == 1 && (size_t)snapTarget->ref.index == i &&
            (snapTarget->vertex == 0 ? moveA : moveB))
            return {"invalid", "Snap target moves with auto-link", "snapTarget"};
        if (moveA) { wall.ax = from.ax + d[0]; wall.ay = from.ay + d[1]; exactSnap(from, wall, 0); }
        if (moveB) { wall.bx = from.bx + d[0]; wall.by = from.by + d[1]; exactSnap(from, wall, 1); }
        if (std::hypot(wall.bx - wall.ax, wall.by - wall.ay) < wallCornerTolerance)
            return {"invalid", "the move would make a wall of zero length", "deltaM"};
    }
    return {};
}

Fail opMoveVertex(const juce::var& p, Model& m, const Ctx& c) {
    Args a(p);
    SurfaceRef ref;
    if (!a.has("ref")) a.err("invalid", "ref is required", "ref");
    else if (auto r = parseRef(a, a.get("ref"), m.scene, "ref")) ref = *r;
    const int vertex = a.integer("vertex", 0, 63);
    auto pos = a.vecProp("positionM", 2, 2, 1e5);
    const bool autoLink = a.optBool("autoLink").value_or(true);
    const auto snapTarget = parseCorner(a, "snapTarget", c.start.scene);
    if (a.bad()) return a.f;
    if (snapTarget) {
        if (cornerDistance(snapTarget->xy, {pos[0], pos[1]}) > 1e-9 ||
            (snapTarget->ref == ref && snapTarget->vertex == vertex))
            return {"invalid", "Snap target must be another corner at the requested position", "snapTarget"};
        pos = {snapTarget->xy.first, snapTarget->xy.second};
    }
    verb::Scene& s = m.scene;
    if (ref.kind == 1) {
        if (vertex > 1) return {"invalid", "a wall has vertices 0 (a) and 1 (b)", "vertex"};
        if (s.walls[(size_t)ref.index].editor.locked) return lockedFail("The wall");
        const bool end = vertex == 1;
        const double cx = end ? s.walls[(size_t)ref.index].bx : s.walls[(size_t)ref.index].ax, cy = end ? s.walls[(size_t)ref.index].by : s.walls[(size_t)ref.index].ay;
        std::vector<std::pair<int, int>> ends{{ref.index, vertex}};
        if (autoLink)  // coincident corners of pickable walls move together (as the native drag does)
            for (size_t i = 0; i < s.walls.size(); ++i)
                for (int e = 0; e < 2; ++e) {
                    if ((int)i == ref.index && e == vertex) continue;
                    const auto& w = s.walls[i];
                    if (!linkableWall(s, w)) continue;
                    if (std::hypot((e ? w.bx : w.ax) - cx, (e ? w.by : w.ay) - cy) < wallCornerTolerance) ends.push_back({(int)i, e});
                }
        if (snapTarget && snapTarget->ref.kind == 1 &&
            std::find(ends.begin(), ends.end(), std::make_pair(snapTarget->ref.index, snapTarget->vertex)) != ends.end())
            return {"invalid", "Snap target moves with auto-link", "snapTarget"};
        for (auto& pr : ends) {
            auto& w = s.walls[(size_t)pr.first];
            setWallEnd(w, pr.second, {pos[0], pos[1]});
        }
        for (auto& pr : ends) {
            const auto& w = s.walls[(size_t)pr.first];
            if (std::hypot(w.bx - w.ax, w.by - w.ay) < wallCornerTolerance) return {"invalid", "the move would make a wall of zero length", "positionM"};
        }
    } else if (ref.kind == 4 || ref.kind == 5) {
        auto& z = (ref.kind == 4 ? s.floorZones : s.ceilZones)[(size_t)ref.index];
        if (z.editor.locked) return lockedFail("The zone");
        if (vertex >= (int)z.poly.size()) return {"invalid", "the zone has fewer vertices", "vertex"};
        const bool wasSimple = !selfIntersects(z.poly);
        z.poly[(size_t)vertex] = {pos[0], pos[1]};
        if (wasSimple && selfIntersects(z.poly)) return {"invalid", "the move would make the zone outline cross itself", "positionM"};
    } else {
        return {"invalid", "only wall and zone vertices can be moved", "ref"};
    }
    return {};
}

Fail opJoinCorners(const juce::var& p, Model& m, const Ctx& c) {
    Args a(p);
    const double tolerance = a.optNum("toleranceM", .001, .25).value_or(.05);
    auto& scene = m.scene;
    std::vector<bool> included(scene.walls.size(), !a.has("refs"));
    if (a.has("refs")) {
        for (auto ref : parseRefs(a, "refs", scene, false)) {
            if (ref.kind != 1) return {"invalid", "Join corners accepts wall references only", "refs"};
            included[(size_t)ref.index] = true;
        }
    }
    if (a.bad()) return a.f;

    struct End { size_t wall; int vertex; CornerXY xy; };
    std::vector<End> ends;
    if (scene.layerModes[0] == 0)
        for (size_t i = 0; i < scene.walls.size(); ++i)
            if (included[i])
                for (int v = 0; v < 2; ++v) ends.push_back({i, v, wallEnd(scene.walls[i], v)});

    int joined = 0, skipped = 0;
    const auto count = ends.size();
    std::vector<bool> visited(count);
    for (size_t seed = 0; seed < count; ++seed) {
        if (visited[seed]) continue;
        std::vector<size_t> cluster{seed};
        visited[seed] = true;
        bool nearMiss = false;
        // Sewn ends travel with their cluster so that a repair cannot reopen that join.
        for (size_t q = 0; q < cluster.size(); ++q) {
            const auto& from = ends[cluster[q]];
            for (size_t j = 0; j < count; ++j) {
                if (from.wall == ends[j].wall) continue;
                const double gap = cornerDistance(from.xy, ends[j].xy);
                if (gap >= tolerance) continue;
                nearMiss |= gap > wallCornerTolerance;
                if (!visited[j]) { visited[j] = true; cluster.push_back(j); }
            }
        }
        if (!nearMiss) continue;

        auto anchor = ends[seed].xy;
        std::optional<CornerXY> lockedAnchor;
        bool conflict = false;
        for (auto j : cluster) {
            const auto& end = ends[j];
            if (!scene.walls[end.wall].editor.locked) continue;
            if (lockedAnchor && *lockedAnchor != end.xy) conflict = true;
            else lockedAnchor = end.xy;
        }
        if (lockedAnchor) anchor = *lockedAnchor;
        if (conflict || std::any_of(cluster.begin(), cluster.end(), [&](size_t j) {
                return cornerDistance(anchor, ends[j].xy) > tolerance;
            })) { ++skipped; continue; }

        auto proposed = scene.walls;
        for (auto j : cluster) {
            const auto& end = ends[j];
            if (!proposed[end.wall].editor.locked) setWallEnd(proposed[end.wall], end.vertex, anchor);
        }
        if (std::any_of(cluster.begin(), cluster.end(), [&](size_t j) {
                const auto& wall = proposed[ends[j].wall];
                return cornerDistance(wallEnd(wall, 0), wallEnd(wall, 1)) < wallCornerTolerance;
            })) { ++skipped; continue; }
        scene.walls = std::move(proposed);
        ++joined;
    }
    if (c.details) {
        auto* details = new juce::DynamicObject();
        details->setProperty("joined", joined);
        details->setProperty("skipped", skipped);
        *c.details = juce::var(details);
    }
    return {};
}

int findPreset(const std::string& name) {
    const auto& pr = verb::surfacePresets();
    for (size_t i = 0; i < pr.size(); ++i) if (juce::String(pr[i].name).equalsIgnoreCase(juce::String(name))) return (int)i;
    return -1;
}

std::string cleanText(std::string t);

Fail opCreate(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    const juce::String kindName = p.getProperty("kind", {}).toString();
    const int kind = kindName == "wall" ? 1 : kindName == "floorZone" ? 4 : kindName == "ceilingZone" ? 5 : 0;
    if (kind == 0) return {"invalid", "kind must be wall, floorZone or ceilingZone", "kind"};
    verb::Scene& s = m.scene;
    const auto group = a.optStr("groupName", 128);
    if (a.bad()) return a.f;
    const std::string groupName = group ? juce::String(cleanText(*group)).trim().toStdString() : std::string();
    if (!groupName.empty()) for (const auto ref : surfaceRefs(s))
        if (surfaceTag(s, ref)->group == groupName && surfaceTag(s, ref)->locked) return lockedFail("The construction");
    // material: an index of the scene, a preset name, or the editor's default (Plaster; a carpet for a floor zone)
    verb::Material mat = verb::surfacePresets()[2];
    bool open = false;
    if (kind == 4) for (const auto& pr : verb::surfacePresets()) if (juce::String(pr.name).containsIgnoreCase("carpet")) { mat = pr; break; }
    if (a.has("material") && a.has("preset")) return {"invalid", "give either material or preset", "material"};
    if (a.has("material")) {
        if (a.isNull("material")) { if (kind != 5) return {"invalid", "only a ceiling zone can be an opening", "material"}; open = true; }
        else {
            const int mi = a.integer("material", 0, 100000);
            if (a.bad()) return a.f;
            if (mi >= (int)s.mats.size()) return {"invalid", "that material does not exist", "material"};
            mat = s.mats[(size_t)mi];
        }
    } else if (a.has("preset")) {
        auto n = a.optStr("preset", 128);
        if (a.bad()) return a.f;
        const int pi = findPreset(*n);
        if (pi < 0) return {"invalid", "unknown material preset", "preset"};
        mat = verb::surfacePresets()[(size_t)pi];
    }
    if (kind == 1) {
        if (a.has("vertices") || a.has("heightM")) return {"invalid", "a wall takes a, b, zBottomM and zTopM", "vertices"};
        auto A = a.vecProp("a", 2, 2, 1e5), B = a.vecProp("b", 2, 2, 1e5);
        const double z0 = a.optNum("zBottomM", 0, 1e3).value_or(0.0);
        double z1 = 1e9;
        if (a.has("zTopM") && !a.isNull("zTopM")) z1 = a.num("zTopM", 0, 1e3);
        if (a.bad()) return a.f;
        if ((int)s.walls.size() >= 128) return {"budgetExceeded", "At most 128 walls.", "kind"};
        if (std::hypot(B[0] - A[0], B[1] - A[1]) < 1e-4) return {"invalid", "a wall of zero length", "b"};
        if (z1 < 1e8 && z1 < z0 + 0.01) return {"invalid", "zTopM must be above zBottomM", "zTopM"};
        s.mats.push_back(mat);
        s.walls.push_back(verb::Wall{A[0], A[1], B[0], B[1], (int)s.mats.size() - 1, z0, z1});
        s.walls.back().editor.group = groupName;
        return {};
    }
    if (a.has("a") || a.has("b") || a.has("zBottomM") || a.has("zTopM")) return {"invalid", "a zone takes vertices and heightM", "a"};
    if (!a.has("vertices")) return {"invalid", "vertices is required", "vertices"};
    auto* arr = a.get("vertices").getArray();
    if (!arr || arr->size() < 3) return {"invalid", "a zone needs at least 3 vertices", "vertices"};
    if (arr->size() > 64) return {"budgetExceeded", "At most 64 vertices per zone.", "vertices"};
    verb::Zone z;
    z.editor.group = groupName;
    int vi = 0;
    for (const auto& e : *arr) {
        std::vector<double> xy;
        if (!a.vec(e, 2, 2, 1e5, xy, "vertices[" + std::to_string(vi++) + "]")) return a.f;
        z.poly.push_back({xy[0], xy[1]});
    }
    if (selfIntersects(z.poly) || polyArea(z.poly) < 1e-6) return {"invalid", "the zone outline crosses itself or has no area", "vertices"};
    if (a.has("heightM") && !a.isNull("heightM")) {
        const double h = a.num("heightM", 0, 30);
        if (a.bad()) return a.f;
        if (h > 1e-6 && !open) z.z = h;
    }
    auto& list = kind == 4 ? s.floorZones : s.ceilZones;
    if ((int)list.size() >= 32) return {"budgetExceeded", "At most 32 zones per surface.", "kind"};
    if (z.z >= 0 && platesIn(s) >= verb::kMaxPlates) return {"budgetExceeded", "At most 8 plates (zones with a height).", "heightM"};
    if (open) z.mat = -1;
    else { s.mats.push_back(mat); z.mat = (int)s.mats.size() - 1; }
    list.push_back(z);
    return {};
}

Fail opDelete(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto refs = parseRefs(a, "refs", m.scene, false);
    if (a.bad()) return a.f;
    if (auto f = checkLocked(m.scene, refs, true); f.bad()) return f;
    verb::Scene& s = m.scene;
    for (int kind : {1, 4, 5}) {
        std::vector<int> idx;
        for (auto r : refs) if (r.kind == kind) idx.push_back(r.index);
        std::sort(idx.begin(), idx.end(), std::greater<int>());
        for (int i : idx) {
            if (kind == 1) s.walls.erase(s.walls.begin() + i);
            else if (kind == 4) s.floorZones.erase(s.floorZones.begin() + i);
            else s.ceilZones.erase(s.ceilZones.begin() + i);
        }
    }
    return {};
}

Fail opApplyPreset(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto refs = parseRefs(a, "refs", m.scene, true);
    auto name = a.optStr("preset", 128);
    if (!name && !a.bad()) a.err("invalid", "preset is required", "preset");
    if (a.bad()) return a.f;
    const int pi = findPreset(*name);
    if (pi < 0) return {"invalid", "unknown material preset", "preset"};
    if (auto f = checkLocked(m.scene, refs, false); f.bad()) return f;
    const verb::Material preset = verb::surfacePresets()[(size_t)pi];
    for (auto r : refs) { const int mi = ownMat(m.scene, r); if (mi >= 0) m.scene.mats[(size_t)mi] = preset; }
    return {};
}

bool isOpenSurface(const verb::Scene& s, SurfaceRef r) {
    if (r.kind == 3) return s.ceilMat < 0;
    if (r.kind == 5) return s.ceilZones[(size_t)r.index].mat < 0;
    return false;
}

struct BandEdit { int band; double value; };
std::vector<BandEdit> parseBands(Args& a, const char* valueKey) {
    std::vector<BandEdit> out;
    auto* arr = a.get("bands").getArray();
    if (!arr || arr->isEmpty() || arr->size() > 7) { a.err("invalid", "bands must hold 1..7 entries", "bands"); return out; }
    for (const auto& e : *arr) {
        Args b(e);
        const int band = b.integer("band", 0, 6);
        const double v = b.num(valueKey, 0, 1);
        if (b.bad()) { a.err("invalid", b.f.message, "bands"); return {}; }
        out.push_back({band, v});
    }
    return out;
}

Fail editMaterial(Model& m, const std::vector<SurfaceRef>& refs, const std::function<void(verb::Material&)>& fn) {
    for (auto r : refs) if (isOpenSurface(m.scene, r)) return {"invalid", "an open ceiling has no material; close it first (room.setOpening)", "refs"};
    for (auto r : refs) {
        const int mi = ownMat(m.scene, r);
        if (mi < 0) continue;
        verb::Material& mat = m.scene.mats[(size_t)mi];
        fn(mat);
        clampBands(mat);
    }
    return {};
}

Fail opSetAbsorption(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto refs = parseRefs(a, "refs", m.scene, true);
    auto bands = parseBands(a, "alpha");
    if (a.bad()) return a.f;
    if (auto f = checkLocked(m.scene, refs, false); f.bad()) return f;
    return editMaterial(m, refs, [&](verb::Material& mat) { mat.name = "Custom"; for (auto& b : bands) mat.alpha[(size_t)b.band] = b.value; });
}
Fail opSetTransmission(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto refs = parseRefs(a, "refs", m.scene, true);
    auto bands = parseBands(a, "tau");
    if (a.bad()) return a.f;
    if (auto f = checkLocked(m.scene, refs, false); f.bad()) return f;
    return editMaterial(m, refs, [&](verb::Material& mat) { mat.name = "Custom"; for (auto& b : bands) mat.tau[(size_t)b.band] = b.value; });
}
Fail opSetScatter(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto refs = parseRefs(a, "refs", m.scene, true);
    auto lf = a.optNum("scatterLF", 0, 1);
    std::optional<double> hf;
    bool hfNull = false;
    if (a.isNull("scatterHF")) hfNull = true; else hf = a.optNum("scatterHF", 0, 1);
    if (!a.bad() && !lf && !hf && !hfNull) a.err("invalid", "give scatterLF and/or scatterHF", "scatterLF");
    if (a.bad()) return a.f;
    if (auto f = checkLocked(m.scene, refs, false); f.bad()) return f;
    return editMaterial(m, refs, [&](verb::Material& mat) {
        mat.name = "Custom";
        if (lf) { if (mat.scatterHf < 0) mat.scatterHf = mat.scatter; mat.scatter = *lf; }
        if (hfNull) mat.scatterHf = -1; else if (hf) mat.scatterHf = *hf;
    });
}

Fail opSetHeights(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto refs = parseRefs(a, "refs", m.scene, false);
    const bool hasBottom = a.has("zBottomM"), hasTop = a.has("zTopM"), hasHeight = a.has("heightM");
    std::optional<double> bottom = a.optNum("zBottomM", 0, 1e3);
    const bool topNull = a.isNull("zTopM");
    std::optional<double> top = topNull ? std::nullopt : a.optNum("zTopM", 0, 1e3);
    const bool heightNull = a.isNull("heightM");
    std::optional<double> height = heightNull ? std::nullopt : a.optNum("heightM", 0, 30);
    if (!a.bad() && !hasBottom && !hasTop && !hasHeight) a.err("invalid", "give zBottomM, zTopM or heightM", "refs");
    if (a.bad()) return a.f;
    if (auto f = checkLocked(m.scene, refs, false); f.bad()) return f;
    verb::Scene& s = m.scene;
    for (auto r : refs) {
        if (r.kind == 1) {
            if (hasHeight) return {"invalid", "heightM applies to zones, not walls", "heightM"};
            auto& w = s.walls[(size_t)r.index];
            const double z0 = bottom ? *bottom : w.z0;
            const double z1 = hasTop ? (topNull ? 1e9 : *top) : w.z1;
            if (z1 < 1e8 && z1 < z0 + 0.05) return {"invalid", "zTopM must be at least 0.05 m above zBottomM", "zTopM"};
            if (z0 > std::min(z1, s.height) - 0.05) return {"invalid", "zBottomM must be below the top of the wall and the ceiling", "zBottomM"};
            w.z0 = z0; w.z1 = z1;
        } else if (r.kind == 4 || r.kind == 5) {
            if (hasBottom || hasTop) return {"invalid", "zBottomM / zTopM apply to walls, not zones", "zBottomM"};
            auto& z = (r.kind == 4 ? s.floorZones : s.ceilZones)[(size_t)r.index];
            const double nz = (!hasHeight || heightNull || *height <= 1e-6) ? -1.0 : *height;
            if (nz >= 0 && z.mat < 0) return {"invalid", "an opening cannot be a plate", "heightM"};
            if (nz >= 0 && z.z < 0 && platesIn(s) >= verb::kMaxPlates) return {"budgetExceeded", "At most 8 plates (zones with a height).", "heightM"};
            z.z = nz;
        } else {
            return {"invalid", "the base floor / ceiling have no height setting", "refs"};
        }
    }
    return {};
}

std::string cleanText(std::string t) {
    for (char& c : t) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    return t;
}

Fail opSetMetadata(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto refs = parseRefs(a, "refs", m.scene, true);
    auto name = a.optStr("name", 128);
    auto hidden = a.optBool("hidden"), locked = a.optBool("locked");
    if (!a.bad() && !name && !hidden && !locked) a.err("invalid", "give name, hidden and/or locked", "refs");
    if (a.bad()) return a.f;
    for (auto r : refs) {
        auto* t = surfaceTag(m.scene, r);
        if (!t) continue;
        if (name) t->name = cleanText(*name);
        if (hidden) t->hidden = *hidden;
        if (locked) t->locked = *locked;
    }
    return {};
}

Fail opLink(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto refs = parseRefs(a, "refs", m.scene, false);
    auto g = a.optStr("groupName", 128);
    if (!g && !a.bad()) a.err("invalid", "groupName is required", "groupName");
    if (a.bad()) return a.f;
    if (refs.size() < 2) return {"invalid", "linking needs at least two surfaces", "refs"};
    if (auto f = checkLocked(m.scene, refs, false); f.bad()) return f;
    const std::string base = juce::String(cleanText(*g)).trim().isEmpty() ? std::string("Construction") : juce::String(cleanText(*g)).trim().toStdString();
    std::string name = base;
    auto conflicts = [&](const std::string& n) {
        for (auto r : surfaceRefs(m.scene))
            if (surfaceTag(m.scene, r)->group == n && std::find(refs.begin(), refs.end(), r) == refs.end()) return true;
        return false;
    };
    for (int suffix = 2; conflicts(name); ++suffix) name = base + " " + std::to_string(suffix);
    for (auto r : refs) if (auto* t = surfaceTag(m.scene, r)) t->group = name;
    return {};
}

Fail opUnlink(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto refs = parseRefs(a, "refs", m.scene, true);
    if (a.bad()) return a.f;
    if (auto f = checkLocked(m.scene, refs, false); f.bad()) return f;
    for (auto r : refs) if (auto* t = surfaceTag(m.scene, r)) t->group.clear();
    return {};
}

// ============================================================================================ room commands
Fail opScale(const juce::var& p, Model& m, const Ctx& c) {
    Args a(p);
    const double f = a.num("factor", 0.05, 20);
    const bool objects = a.boolean("includeObjects");
    if (a.bad()) return a.f;
    verb::Scene& s = m.scene;
    if (s.walls.empty()) return {};
    if (anyLockedSurface(s)) return {"locked", "The room has locked surfaces; unlock them before scaling.", "refs"};
    double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
    for (auto& w : s.walls) { x0 = std::min({x0, w.ax, w.bx}); x1 = std::max({x1, w.ax, w.bx}); y0 = std::min({y0, w.ay, w.by}); y1 = std::max({y1, w.ay, w.by}); }
    const double cx = 0.5 * (x0 + x1), cy = 0.5 * (y0 + y1);
    for (auto& w : s.walls) {
        w.ax = cx + (w.ax - cx) * f; w.ay = cy + (w.ay - cy) * f;
        w.bx = cx + (w.bx - cx) * f; w.by = cy + (w.by - cy) * f;
    }
    for (auto* zones : {&s.floorZones, &s.ceilZones})
        for (auto& z : *zones)
            for (auto& pt : z.poly) { pt.first = cx + (pt.first - cx) * f; pt.second = cy + (pt.second - cy) * f; }
    if (!m.automationRange.isDefault()) {
        m.automationRange = m.automationRange.scaled(cx, cy, f, objects);
        if (!objects) {
            const int ns = (int)m.sources.size();
            for (int o = 0; o <= ns; ++o) {
                const int obj = o == ns ? kListener : o;
                const auto getP = [&](int k) -> double {
                    auto it = m.params.find(paramId(obj, k).toStdString());
                    return it != m.params.end() ? (double)it->second : (k == 2 ? 1.5 : 0.0);
                };
                m.automationRange.expandPoint(getP(0), getP(1), getP(2));
            }
            m.automationRange = AutomationRange::sanitised(m.automationRange);
        }
    }
    if (objects) {
        s.height = std::min(std::max(s.height * f, 1.0), 30.0);
        for (auto& w : s.walls) { w.z0 *= f; if (w.z1 < 1e8) w.z1 *= f; }
        for (auto* zones : {&s.floorZones, &s.ceilZones})
            for (auto& z : *zones) if (z.z >= 0) z.z = std::min(z.z * f, s.height);
        const int ns = (int)m.sources.size();
        for (int o = 0; o <= ns; ++o) {
            const int obj = o == ns ? kListener : o;
            const std::string ix = paramId(obj, 0).toStdString(), iy = paramId(obj, 1).toStdString(), iz = paramId(obj, 2).toStdString();
            m.params[ix] = clampParamWithRange(m.automationRange, c.proc, ix, cx + (m.params[ix] - cx) * f);
            m.params[iy] = clampParamWithRange(m.automationRange, c.proc, iy, cy + (m.params[iy] - cy) * f);
            m.params[iz] = clampParamWithRange(m.automationRange, c.proc, iz, m.params[iz] * f);
            for (auto& pt : m.paths[(size_t)obj].points) {
                pt[0] = std::clamp(cx + (pt[0] - cx) * f, -200.0, 200.0);
                pt[1] = std::clamp(cy + (pt[1] - cy) * f, -200.0, 200.0);
                pt[2] = std::clamp(pt[2] * f, 0.0, 30.0);
            }
        }
    }
    return {};
}

Fail opFitAutomationRange(const juce::var& p, Model& m, const Ctx& c) {
    Args a(p);
    const auto margin = a.optNum("marginM", 0.1, 50.0);
    if (a.bad()) return a.f;
    auto readParam = [&m](const char* id) -> double {
        auto it = m.params.find(id);
        return it != m.params.end() ? (double)it->second : 0.0;
    };
    const auto fitted = computeFitAutomationRange(m.scene, m.sources, m.output.layout, m.output.radius, m.paths, readParam, margin ? *margin : -1.0);
    m.automationRange = fitted;
    clampAllPositionParams(m, c.proc);
    return {};
}

Fail opSetAutomationRange(const juce::var& p, Model& m, const Ctx& c) {
    Args a(p);
    const auto reset = a.optBool("reset");
    if (a.bad()) return a.f;
    const bool hasAxis = a.has("xM") || a.has("yM") || a.has("zM");
    if (!reset.value_or(false) && !hasAxis)
        return {"invalid", "Give xM, yM, zM or reset=true", "xM"};

    AutomationRange next = m.automationRange;
    if (reset.value_or(false)) {
        next = AutomationRange{};
    }
    if (a.has("xM")) {
        auto v = a.vecProp("xM", 2, 2, AutomationRange::kGlobalMaxXY);
        if (a.bad()) return a.f;
        if (v[1] - v[0] < AutomationRange::kMinSpanXY - 1e-9)
            return {"invalid", "X range must satisfy -200 <= min, max <= 200 and span >= 1.0 m.", "xM"};
        next.xMin = v[0]; next.xMax = v[1];
    }
    if (a.has("yM")) {
        auto v = a.vecProp("yM", 2, 2, AutomationRange::kGlobalMaxXY);
        if (a.bad()) return a.f;
        if (v[1] - v[0] < AutomationRange::kMinSpanXY - 1e-9)
            return {"invalid", "Y range must satisfy -200 <= min, max <= 200 and span >= 1.0 m.", "yM"};
        next.yMin = v[0]; next.yMax = v[1];
    }
    if (a.has("zM")) {
        auto v = a.vecProp("zM", 2, 2, AutomationRange::kGlobalMaxZ);
        if (a.bad()) return a.f;
        if (v[0] < AutomationRange::kGlobalMinZ - 1e-9 || v[1] > AutomationRange::kGlobalMaxZ + 1e-9
            || v[1] - v[0] < AutomationRange::kMinSpanZ - 1e-9)
            return {"invalid", "Z range must satisfy 0 <= min, max <= 30 and span >= 0.5 m.", "zM"};
        next.zMin = v[0]; next.zMax = v[1];
    }
    next = AutomationRange::sanitised(next);
    if (!next.isValid()) return {"invalid", "Invalid automation range.", "xM"};
    m.automationRange = next;
    clampAllPositionParams(m, c.proc);
    return {};
}

Fail opSetHeight(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    const auto h = a.optNum("heightM", 1, 30);
    const auto extent = p.getProperty("ceilingExtent",juce::var()).toString();
    if(!extent.isEmpty() && extent!="walls" && extent!="infinite") return {"invalid","Invalid ceiling extent","ceilingExtent"};
    if (a.bad()) return a.f;
    if(!h && extent.isEmpty())return {"invalid","Height or ceiling extent required","heightM"};
    if(h)m.scene.height = *h;
    if(!extent.isEmpty())m.scene.ceilingInfinite=extent=="infinite";
    return {};
}

Fail opSetOpening(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    SurfaceRef ref;
    if (!a.has("ref")) a.err("invalid", "ref is required", "ref");
    else if (auto r = parseRef(a, a.get("ref"), m.scene, "ref")) ref = *r;
    const bool open = a.boolean("open");
    if (a.bad()) return a.f;
    if (ref.kind != 3 && ref.kind != 5) return {"invalid", "only the ceiling or a ceiling zone can be opened", "ref"};
    if (surfaceTag(m.scene, ref)->locked) return lockedFail("The surface");
    int* mat = ref.kind == 3 ? &m.scene.ceilMat : &m.scene.ceilZones[(size_t)ref.index].mat;
    if (open) *mat = -1;
    else if (*mat < 0) { m.scene.mats.push_back(verb::surfacePresets()[2]); *mat = (int)m.scene.mats.size() - 1; }
    return {};
}

Fail opSetAir(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto t = a.optNum("tempC", -20, 45);
    auto h = a.optNum("humidityPct", 5, 100);
    if (!a.bad() && !t && !h) a.err("invalid", "give tempC and/or humidityPct", "tempC");
    if (a.bad()) return a.f;
    if (t) m.scene.tempC = *t;
    if (h) m.scene.humidity = *h;
    return {};
}

Fail opSetClutter(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto count = a.optInt("count", 0, 80);
    auto minLen = a.optNum("minLenM", 0.1, 3), maxLen = a.optNum("maxLenM", 0.2, 5);
    auto tlMin = a.optNum("tlMinDb", 0, 30), tlMax = a.optNum("tlMaxDb", 0, 40);
    auto hMin = a.optNum("minHeightM", 0.1, 3), hMax = a.optNum("maxHeightM", 0.2, 4);
    auto seed = a.optInt("seed", 1, 9999);
    auto minDist = a.optNum("minDistM", 0, 10);  // extension: persisted since T3 (see CONTRACT_ISSUES)
    if (!a.bad() && !count && !minLen && !maxLen && !tlMin && !tlMax && !hMin && !hMax && !seed && !minDist) a.err("invalid", "give at least one clutter field", "count");
    if (a.bad()) return a.f;
    verb::ClutterCfg& c = m.scene.clutter;
    if (count) c.count = *count;
    if (minLen) c.minLen = *minLen;
    if (maxLen) c.maxLen = *maxLen;
    if (tlMin) c.tlMin = *tlMin;
    if (tlMax) c.tlMax = *tlMax;
    if (hMin) c.hMin = *hMin;
    if (hMax) c.hMax = *hMax;
    if (minDist) c.minDist = *minDist;
    if (seed) { c.seed = *seed; m.scene.roughSeed = *seed; }
    if (c.minLen > c.maxLen) return {"invalid", "minLenM is above maxLenM", "minLenM"};
    if (c.tlMin > c.tlMax) return {"invalid", "tlMinDb is above tlMaxDb", "tlMinDb"};
    if (c.hMin > c.hMax) return {"invalid", "minHeightM is above maxHeightM", "minHeightM"};
    return {};
}

Fail opSetRoughness(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto r = a.optNum("roughness", 0, 1);
    auto seed = a.optInt("seed", 1, 9999);
    if (!a.bad() && !r && !seed) a.err("invalid", "give roughness and/or seed", "roughness");
    if (a.bad()) return a.f;
    if (r) m.scene.roughness = *r;
    if (seed) m.scene.roughSeed = *seed;
    return {};
}

Fail opSetDiffraction(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    const bool on = a.boolean("enabled");
    if (a.bad()) return a.f;
    m.scene.diffraction = on;
    return {};
}

Fail opClear(const juce::var&, Model& m, const Ctx&) {
    verb::Scene& s = m.scene;
    for (auto& w : s.walls) if (w.editor.locked) return lockedFail("A wall");
    for (auto* l : {&s.floorZones, &s.ceilZones}) for (auto& z : *l) if (z.editor.locked) return lockedFail("A zone");
    s.walls.clear(); s.floorZones.clear(); s.ceilZones.clear();
    s.clutter.count = 0; s.roughness = 0;
    return {};
}

// ============================================================================================ source / output / tail / listener
void setP(Model& m, const std::string& id, double v, VerbProcessor& proc) { m.params[id] = clampParamWithRange(m.automationRange, proc, id, v); }

Fail opSourceAdd(const juce::var&, Model& m, const Ctx& c) {
    if ((int)m.sources.size() >= kMaxSources) return {"budgetExceeded", "At most 32 sources.", ""};
    SourceCfg cfg;
    cfg.name = "Source " + std::to_string(m.sources.size() + 1);
    int next = 0;
    for (auto& s : m.sources) next = std::max(next, s.firstIn + s.effectiveChannels());
    if (next + 1 > kMaxInputChannels) return {"budgetExceeded", "All 64 input channels are used.", ""};
    cfg.firstIn = next;
    m.sources.push_back(cfg);
    if ((int)buildPoints(m.sources).size() > kMaxPoints) return {"budgetExceeded", "At most 64 spatial points.", ""};
    const int idx = (int)m.sources.size() - 1;
    double cx = 4, cy = 3;
    if (!m.scene.walls.empty()) {
        double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
        for (auto& w : m.scene.walls) { x0 = std::min({x0, w.ax, w.bx}); x1 = std::max({x1, w.ax, w.bx}); y0 = std::min({y0, w.ay, w.by}); y1 = std::max({y1, w.ay, w.by}); }
        cx = (x0 + x1) / 2; cy = (y0 + y1) / 2;
    }
    setP(m, paramId(idx, 0).toStdString(), cx - 1.0 + 0.4 * (idx % 5), c.proc);
    setP(m, paramId(idx, 1).toStdString(), cy + 0.4 * (idx / 5), c.proc);
    setP(m, paramId(idx, 2).toStdString(), 1.5, c.proc);
    setP(m, paramId(idx, 3).toStdString(), 0.0, c.proc);
    setP(m, paramId(idx, 4).toStdString(), 0.0, c.proc);
    setP(m, sourceSwitchId(idx, false).toStdString(), 0.0, c.proc);
    setP(m, sourceSwitchId(idx, true).toStdString(), 0.0, c.proc);
    return {};
}

Fail opSourceRemove(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    const int index = a.integer("index", 0, 31);
    if (a.bad()) return a.f;
    if (index >= (int)m.sources.size()) return {"invalid", "that source does not exist", "index"};
    if (m.sources.size() <= 1) return {"invalid", "at least one source must remain", "index"};
    for (int i = index; i + 1 < (int)m.sources.size(); ++i) {  // later sources move up with their poses, gains, mute and solo
        for (int k = 0; k < 5; ++k) m.params[paramId(i, k).toStdString()] = m.params[paramId(i + 1, k).toStdString()];
        for (bool solo : {false, true}) m.params[sourceSwitchId(i, solo).toStdString()] = m.params[sourceSwitchId(i + 1, solo).toStdString()];
    }
    for(int i=index;i+1<(int)m.sources.size();++i){m.paths[(size_t)i]=m.paths[(size_t)i+1];m.params[VerbProcessor::pathParamId(i).toStdString()]=m.params[VerbProcessor::pathParamId(i+1).toStdString()];}
    m.paths[m.sources.size()-1]={};
    for(int o=0;o<=kMaxSources;++o){  // E19: Look at targets follow the source list (the removed source leaves the target unset)
        auto& lp=m.paths[(size_t)o];
        if(lp.lookPoint||lp.lookTarget<0||lp.lookTarget>=kListener)continue;
        if(lp.lookTarget==index)lp.lookTarget=-1;else if(lp.lookTarget>index)--lp.lookTarget;
    }
    remapModRoutesOnSourceRemove(m.modRoutes, index);
    m.sources.erase(m.sources.begin() + index);
    return {};
}

Fail opSourceSetConfig(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    const int index = a.integer("index", 0, 31);
    auto name = a.optStr("name", 128);
    auto format = a.optInt("format", 0, 7);
    auto channels = a.optInt("channels", 1, 16);
    auto firstIn = a.optInt("firstIn", 0, 63);
    auto sum = a.optBool("sum"), freePoints = a.optBool("freePoints");
    std::optional<std::string> color;
    if (a.has("color")) {
        if (a.isNull("color")) {
            color = std::string();
        } else if (auto cs = a.optStr("color", 32)) {
            if (cs->empty()) color = std::string();
            else if (isValidSourceColorHex(*cs)) color = normalizeSourceColor(*cs);
            else a.err("invalid", "color must be empty, null, or #rrggbb", "color");
        }
    }
    if (!a.bad() && !name && !format && !channels && !firstIn && !sum && !freePoints && !color)
        a.err("invalid", "give at least one source field", "index");
    if (a.bad()) return a.f;
    if (index >= (int)m.sources.size()) return {"invalid", "that source does not exist", "index"};
    SourceCfg c = m.sources[(size_t)index];
    const bool wasFree = c.freePoints;
    if (name) c.name = cleanText(*name);
    if (color) c.color = *color;
    if (format) c.format = *format;
    const int fixedCh = sourceFormats()[(size_t)c.format].channels;  // 0 = the ring: free channel count
    if (fixedCh > 0) {
        if (channels && *channels != fixedCh) return {"invalid", "this format has a fixed number of channels", "channels"};
        c.channels = fixedCh;
    } else if (channels) {
        c.channels = *channels;
    }
    if (firstIn) c.firstIn = *firstIn;
    if (sum) c.sum = *sum;
    if (freePoints) c.freePoints = *freePoints;
    if (c.firstIn + c.effectiveChannels() > kMaxInputChannels) return {"budgetExceeded", "The source would read past input channel 64.", "firstIn"};
    if (c.freePoints && ((!wasFree) || c.ox.size() != (size_t)c.effectiveChannels() || c.oy.size() != c.ox.size() || c.oz.size() != c.ox.size()))
        freezeLayout(c);  // switching to free points (or a new channel count) starts from the current circle: nothing jumps
    m.sources[(size_t)index] = c;
    if ((int)buildPoints(m.sources).size() > kMaxPoints) return {"budgetExceeded", "At most 64 spatial points.", "channels"};
    return {};
}

Fail opSourcePattern(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    const int index = a.integer("index", 0, 31);
    const double v = a.num("pattern", 0, 1);
    if (a.bad()) return a.f;
    if (index >= (int)m.sources.size()) return {"invalid", "that source does not exist", "index"};
    m.sources[(size_t)index].pattern = v;
    return {};
}
Fail opSourceDirectivity(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    const int index = a.integer("index", 0, 31);
    auto inst = a.optStr("instrument", 128);
    auto amt = a.has("instrumentAmount") ? a.optNum("instrumentAmount", 0, 1) : a.optNum("amount", 0, 1);
    auto aimAz = a.optNum("aimAzDeg", -180, 180);
    auto aimEl = a.optNum("aimElDeg", -90, 90);
    auto aimRoll = a.optNum("aimRollDeg", -180, 180);
    auto customData = a.has("customDirectivityData") ? a.optStr("customDirectivityData", verb::kMaxVerbdirBytes)
                                                     : a.optStr("customData", verb::kMaxVerbdirBytes);
    auto customName = a.has("customDirectivityName") ? a.optStr("customDirectivityName", 128)
                                                     : a.optStr("customName", 128);
    auto customPath = a.has("customDirectivityPath") ? a.optStr("customDirectivityPath", 1024)
                                                     : a.optStr("customPath", 1024);
    if (!a.bad() && !inst && !amt && !aimAz && !aimEl && !aimRoll && !customData && !customName && !customPath)
        a.err("invalid", "give instrument, amount, aim and/or customData", "instrument");
    if (a.bad()) return a.f;
    if (index >= (int)m.sources.size()) return {"invalid", "that source does not exist", "index"};
    SourceCfg c = m.sources[(size_t)index];
    const SourceCfg before = c;
    if (inst) c.instrument = inst->empty() ? "none" : *inst;
    if (amt) c.instrumentAmount = *amt;
    if (customData) {
        if (customData->size() > verb::kMaxVerbdirBytes)
            return {"budgetExceeded", "Custom directivity exceeds the 64 KB limit.", "customData"};
        if (!customData->empty()) {
            std::string err;
            verb::DirectivityProfile prof;
            if (!verb::parseVerbdir(*customData, prof, err)) return {"invalid", "Invalid .verbdir data: " + err, "customData"};
            c.instrument = "custom";
            c.customDirectivityData = *customData;
            if (!customName && c.customDirectivityName.empty()) c.customDirectivityName = prof.name;
        } else {
            c.customDirectivityData.clear();
        }
    }
    if (customName) c.customDirectivityName = cleanText(*customName);
    if (customPath) c.customDirectivityPath = *customPath;
    if (c.instrument == "none") {
        if ((aimAz && *aimAz != 0.0) || (aimEl && *aimEl != 0.0) || (aimRoll && *aimRoll != 0.0))
            return {"invalid", "Aim correction requires an active directivity profile", "aimAzDeg"};
        c.customDirectivityData.clear();
        c.customDirectivityName.clear();
        c.customDirectivityPath.clear();
        c.aimAzDeg = 0.0;
        c.aimElDeg = 0.0;
        c.aimRollDeg = 0.0;
    } else if (c.instrument != "custom") {
        if (!verb::findBuiltinDirectivity(c.instrument))
            return {"invalid", "Unknown directivity profile: " + c.instrument, "instrument"};
    } else if (c.customDirectivityData.empty() && c.customDirectivityPath.empty()) {
        return {"invalid", "Custom directivity requires customData or customPath.", "customData"};
    }
    // A different profile starts with its own orientation: hand-rotated angles of the previous one are dropped
    // (values given in the same command still apply).
    if (before.instrument != c.instrument || before.customDirectivityData != c.customDirectivityData
        || before.customDirectivityPath != c.customDirectivityPath) {
        c.aimAzDeg = 0.0;
        c.aimElDeg = 0.0;
        c.aimRollDeg = 0.0;
    }
    if (c.instrument != "none") {
        if (aimAz) c.aimAzDeg = *aimAz;
        if (aimEl) c.aimElDeg = *aimEl;
        if (aimRoll) c.aimRollDeg = *aimRoll;
    }
    resolveSourceDirectivity(c);
    m.sources[(size_t)index] = std::move(c);
    return {};
}
Fail opSourceSpread(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    const int index = a.integer("index", 0, 31);
    const double v = a.num("spreadM", 0, 20);
    if (a.bad()) return a.f;
    if (index >= (int)m.sources.size()) return {"invalid", "that source does not exist", "index"};
    m.sources[(size_t)index].spread = v;
    return {};
}
Fail opSourcePointOffset(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    const int index = a.integer("index", 0, 31), channel = a.integer("channel", 0, 15);
    auto off = a.vecProp("offsetM", 3, 3, 100);
    if (a.bad()) return a.f;
    if (index >= (int)m.sources.size()) return {"invalid", "that source does not exist", "index"};
    SourceCfg& c = m.sources[(size_t)index];
    if (!c.freePoints) return {"invalid", "the source does not use free points", "index"};
    if (channel >= c.effectiveChannels()) return {"invalid", "that channel does not exist", "channel"};
    if (c.ox.size() != (size_t)c.effectiveChannels() || c.oy.size() != c.ox.size() || c.oz.size() != c.ox.size()) freezeLayout(c);
    c.ox[(size_t)channel] = (float)off[0]; c.oy[(size_t)channel] = (float)off[1]; c.oz[(size_t)channel] = (float)off[2];
    return {};
}

Fail opOutputSet(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto layout = a.optInt("layout", -1, 11);
    auto radius = a.optNum("radiusM", 0, 1);
    auto mic = a.optInt("micPattern", 0, 7);
    auto normalization = a.optInt("ambisonicsNormalization", 0, 2);
    auto levelDb = a.optInt("ambisonicsLevelDb", -6, 0);
    if (levelDb && *levelDb % 3 != 0) a.err("invalid", "choose 0, -3 or -6 dB", "ambisonicsLevelDb");
    if (!a.bad() && !layout && !radius && !mic && !normalization && !levelDb) a.err("invalid", "give at least one output setting", "layout");
    if (a.bad()) return a.f;
    if (layout) m.output.layout = *layout;
    if (normalization) m.output.ambisonicsNormalization = *normalization;
    if (levelDb) m.output.ambisonicsLevelDb = *levelDb;
    if (mic) m.output.micPattern = *mic;
    if (radius) m.output.radius = *radius;
    else if (mic && *mic == (int)verb::MicPattern::HumanEars) m.output.radius = 0.0875;  // ears at the head half-width, as the native editor
    return {};
}

Fail opTailQuality(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    auto rays = a.optInt("baseRays", 8000, 384000);
    auto ad = a.optBool("adaptive");
    if (!a.bad() && !rays && !ad) a.err("invalid", "give baseRays and/or adaptive", "baseRays");
    if (a.bad()) return a.f;
    if (rays) m.tail.rays = *rays;
    if (ad) m.tail.adaptive = *ad;
    return {};
}

Fail opListenerSet(const juce::var& p, Model& m, const Ctx& c) {  // extension (the listener pose lives in the parameters)
    Args a(p);
    const auto& ar = m.automationRange;
    auto x = a.optNum("xM", ar.xMin, ar.xMax), y = a.optNum("yM", ar.yMin, ar.yMax), z = a.optNum("zM", ar.zMin, ar.zMax), yaw = a.optNum("yawDeg", -180, 180);
    if (!a.bad() && !x && !y && !z && !yaw) a.err("invalid", "give xM, yM, zM and/or yawDeg", "xM");
    if (a.bad()) return a.f;
    if (x) setP(m, paramId(kListener, 0).toStdString(), *x, c.proc);
    if (y) setP(m, paramId(kListener, 1).toStdString(), *y, c.proc);
    if (z) setP(m, paramId(kListener, 2).toStdString(), *z, c.proc);
    if (yaw) setP(m, paramId(kListener, 3).toStdString(), *yaw, c.proc);
    return {};
}


Fail opPath(const juce::var& p,Model& m,const Ctx&) {
    Args a(p);const int o=a.integer("object",0,kMaxSources);
    if(a.bad())return a.f;
    if(o!=kListener && o>=(int)m.sources.size())return {"invalid","Source does not exist","object"};
    auto parseEasing=[](const juce::var& v,PathEasing& out)->bool{
        if(!v.isString())return false;
        const auto s=v.toString();
        if(s=="linear"){out=PathEasing::Linear;return true;}
        if(s=="easeIn"){out=PathEasing::EaseIn;return true;}
        if(s=="easeOut"){out=PathEasing::EaseOut;return true;}
        if(s=="easeInOut"){out=PathEasing::EaseInOut;return true;}
        return false;
    };
    auto parseAzEasing=[](const juce::var& v,AzimuthEasing& out)->bool{
        if(!v.isString())return false;
        const auto s=v.toString();
        if(s=="linear"){out=AzimuthEasing::Linear;return true;}
        if(s=="easeIn"){out=AzimuthEasing::EaseIn;return true;}
        if(s=="easeOut"){out=AzimuthEasing::EaseOut;return true;}
        if(s=="easeInOut"){out=AzimuthEasing::EaseInOut;return true;}
        if(s=="hold"){out=AzimuthEasing::Hold;return true;}
        return false;
    };
    auto parseAzKeyObj=[&](const juce::var& item,AzimuthKey& out,const char*& fieldOut,std::string& msgOut)->bool{
        if(!item.isObject()){fieldOut="azKeys";msgOut="Expected azimuth key object";return false;}
        const auto tv=item.getProperty("t",{}), av=item.getProperty("az",{});
        if(!Args::isNum(tv)){fieldOut="t";msgOut="Expected numeric t";return false;}
        const double t=(double)tv;
        if(!std::isfinite(t)||t<0.0||t>100.0){fieldOut="t";msgOut="t out of range [0, 100]";return false;}
        if(!Args::isNum(av)){fieldOut="az";msgOut="Expected numeric az";return false;}
        const double az=(double)av;
        if(!std::isfinite(az)||az<-180.0||az>180.0){fieldOut="az";msgOut="az out of range [-180, 180]";return false;}
        AzimuthEasing ease=AzimuthEasing::Linear;
        if(item.hasProperty("easing")&&!parseAzEasing(item.getProperty("easing",{}),ease)){
            fieldOut="easing";msgOut="Invalid azimuth key easing";return false;
        }
        out={t,az,ease};
        return true;
    };
    auto parseLfoObj=[&](const juce::var& item,PathLfo& lfoOut,const char*& fieldOut,std::string& msgOut)->bool{
        if(!item.isObject()){fieldOut="lfo";msgOut="Expected LFO object";return false;}
        if(item.hasProperty("enabled")){
            const auto ev=item.getProperty("enabled",{});
            if(!ev.isBool()){fieldOut="lfo";msgOut="lfo.enabled must be a boolean";return false;}
            lfoOut.enabled=(bool)ev;
        }
        if(item.hasProperty("shape")){
            const auto sv=item.getProperty("shape",{});
            LfoShape sh{};
            if(!sv.isString()||!parseLfoShape(sv.toString().toRawUTF8(),sh)){fieldOut="lfo";msgOut="Invalid lfo.shape";return false;}
            lfoOut.shape=sh;
        }
        auto lfoNum=[&](const char* key,double lo,double hi,double& dst)->bool{
            if(!item.hasProperty(key))return true;
            const auto v=item.getProperty(key,{});
            if(!Args::isNum(v)){fieldOut="lfo";msgOut=std::string("Expected numeric lfo.")+key;return false;}
            const double d=(double)v;
            if(!std::isfinite(d)||d<lo||d>hi){fieldOut="lfo";msgOut=std::string("lfo.")+key+" out of range";return false;}
            dst=d;return true;
        };
        if(!lfoNum("rateHz",0.0,kMaxPathLfoRateHz,lfoOut.rateHz))return false;
        if(!lfoNum("depth",0.0,1.0,lfoOut.depth))return false;
        if(!lfoNum("phaseDeg",0.0,360.0,lfoOut.phaseDeg))return false;
        if(item.hasProperty("seed")){
            const auto sv=item.getProperty("seed",{});
            if(!(sv.isInt()||sv.isInt64()||(sv.isDouble()&&std::floor((double)sv)==(double)sv))){fieldOut="lfo";msgOut="lfo.seed must be an integer";return false;}
            const double d=(double)sv;
            if(!std::isfinite(d)||d<1.0||d>999999.0){fieldOut="lfo";msgOut="lfo.seed out of range [1, 999999]";return false;}
            lfoOut.seed=(uint32_t)d;
        }
        return true;
    };
    auto next=m.paths[(size_t)o];const auto op=a.get("op").toString();
    if(op=="path.clear"){
        MovementPath cleared;  // E19: orientation belongs to the object; G1: azKeys require a moving path and clear with it
        cleared.lookAt=next.lookAt;cleared.lookPoint=next.lookPoint;cleared.lookTarget=next.lookTarget;cleared.lookPos=next.lookPos;cleared.orientOffset=next.orientOffset;
        m.paths[(size_t)o]=cleared;return {};
    }
    if(op=="path.set") {
        auto* points=a.get("points").getArray();
        if(!points || points->size()<2 || points->size()>64)return {"invalid","Expected 2..64 points","points"};
        next.points.clear(); next.start=0.0; next.end=100.0; next.times.clear(); next.easings.clear();
        for(const auto& pt:*points) {
            auto* xyz=pt.getArray();if(!xyz || xyz->size()!=3)return {"invalid","Expected [x,y,z]","points"};
            PathPoint q{};for(int k=0;k<3;++k){if(!Args::isNum((*xyz)[k]))return {"invalid","Expected numeric point","points"};q[(size_t)k]=(double)(*xyz)[k];}next.points.push_back(q);
        }
        if(a.has("times")){
            auto* ts=a.get("times").getArray();
            if(!ts || (size_t)ts->size()!=next.points.size())return {"invalid","Expected times matching point count","times"};
            next.times.resize(next.points.size());
            for(size_t i=0;i<next.points.size();++i){
                const auto& tv=(*ts)[(int)i];
                if(tv.isVoid())continue;
                if(!Args::isNum(tv))return {"invalid","Expected numeric or null point time","times"};
                next.times[i]=(double)tv;
            }
        }
        if(a.has("easings")){
            auto* es=a.get("easings").getArray();
            if(!es || (size_t)es->size()!=next.points.size())return {"invalid","Expected easings matching point count","easings"};
            next.easings.resize(next.points.size(),PathEasing::Linear);
            for(size_t i=0;i<next.points.size();++i){
                if(!parseEasing((*es)[(int)i],next.easings[i]))return {"invalid","Invalid path easing","easings"};
            }
        }
        if(a.has("azKeys")){
            auto* ks=a.get("azKeys").getArray();
            if(!ks || (size_t)ks->size()>kMaxAzKeys)return {"invalid","Expected 0..128 azimuth keys","azKeys"};
            next.azKeys.clear();
            for(const auto& item:*ks){
                AzimuthKey ak;const char* fld="azKeys";std::string msg;
                if(!parseAzKeyObj(item,ak,fld,msg))return {"invalid",msg,fld};
                next.azKeys.push_back(ak);
            }
            normalizePathAzKeys(next);
        }
    } else if(op=="path.addPoint" || op=="path.movePoint") {
        auto xyz=a.vecProp("point",3,3,200);if(a.bad())return a.f;
        PathPoint q{xyz[0],xyz[1],xyz[2]};
        if(op=="path.addPoint") {
            if(a.has("index")){
                int i=a.integer("index",0,(int)next.points.size());if(a.bad())return a.f;
                next.points.insert(next.points.begin()+i,q);
                if(!next.times.empty()&&(size_t)i<=next.times.size())next.times.insert(next.times.begin()+i,std::nullopt);
                if(!next.easings.empty()&&(size_t)i<=next.easings.size())next.easings.insert(next.easings.begin()+i,PathEasing::Linear);
            }else{
                next.points.push_back(q);
                if(!next.times.empty())next.times.push_back(std::nullopt);
                if(!next.easings.empty())next.easings.push_back(PathEasing::Linear);
            }
        } else {int i=a.integer("index",0,(int)next.points.size()-1);if(a.bad())return a.f;next.points[(size_t)i]=q;}
    } else if(op=="path.removePoint") {
        int i=a.integer("index",0,(int)next.points.size()-1);if(a.bad())return a.f;
        if(next.points.size()<=2)return {"invalid","A path requires two points; use clear","index"};
        next.points.erase(next.points.begin()+i);
        if((size_t)i<next.times.size())next.times.erase(next.times.begin()+i);
        if((size_t)i<next.easings.size())next.easings.erase(next.easings.begin()+i);
        if(!next.times.empty()){if(i==0)next.times[0].reset();if(!next.closed)next.times.back().reset();}
    } else if(op=="path.setPointTiming") {
        if(next.points.size()<2)return {"invalid","Path has fewer than two points","object"};
        int i=a.integer("index",0,(int)next.points.size()-1);if(a.bad())return a.f;
        if(!a.has("time")&&!a.has("easing"))return {"invalid","Expected time and/or easing","time"};
        if(a.has("time")){
            if(a.isNull("time")){
                if(!next.times.empty())next.times[(size_t)i].reset();
            }else{
                double t=a.num("time",0.0,100.0);if(a.bad())return a.f;
                if(next.times.empty())next.times.resize(next.points.size());
                next.times[(size_t)i]=t;
            }
        }
        if(a.has("easing")){
            PathEasing ease=PathEasing::Linear;
            if(!parseEasing(a.get("easing"),ease))return {"invalid","Invalid path easing","easing"};
            const bool isKey=(i==0)||((size_t)i<next.times.size()&&next.times[(size_t)i].has_value());
            const bool isLast=(!next.closed && (size_t)i+1==next.points.size());
            if(ease!=PathEasing::Linear && (!isKey || isLast))
                return {"invalid","Only key points before the end can have easing","easing"};
            if(next.easings.empty()&&ease!=PathEasing::Linear)next.easings.resize(next.points.size(),PathEasing::Linear);
            if(!next.easings.empty())next.easings[(size_t)i]=ease;
        }
    } else if(op=="path.setAzimuthKey") {
        if(next.points.size()<2)return {"invalid","Path has fewer than two points","object"};
        if(a.has("index")){
            int i=a.integer("index",0,(int)next.azKeys.size()-1);if(a.bad())return a.f;
            if(!a.has("t")&&!a.has("az")&&!a.has("easing"))return {"invalid","Expected t, az, and/or easing","az"};
            AzimuthKey ak=next.azKeys[(size_t)i];
            if(a.has("t")){ak.t=a.num("t",0.0,100.0);if(a.bad())return a.f;}
            if(a.has("az")){ak.azDeg=a.num("az",-180.0,180.0);if(a.bad())return a.f;}
            if(a.has("easing")&&!parseAzEasing(a.get("easing"),ak.easing))return {"invalid","Invalid azimuth key easing","easing"};
            next.azKeys.erase(next.azKeys.begin()+i);
            next.azKeys.push_back(ak);
            normalizePathAzKeys(next);
        }else{
            double t=a.num("t",0.0,100.0);if(a.bad())return a.f;
            double az=a.num("az",-180.0,180.0);if(a.bad())return a.f;
            const bool hasEase=a.has("easing");AzimuthEasing ease=AzimuthEasing::Linear;
            if(hasEase&&!parseAzEasing(a.get("easing"),ease))return {"invalid","Invalid azimuth key easing","easing"};
            int hit=-1;for(int i=0;i<(int)next.azKeys.size();++i)if(std::abs(next.azKeys[(size_t)i].t-t)<kAzKeyMinGap){hit=i;break;}
            if(hit>=0){
                next.azKeys[(size_t)hit].t=t;next.azKeys[(size_t)hit].azDeg=az;
                if(hasEase)next.azKeys[(size_t)hit].easing=ease;
            }else{
                if(next.azKeys.size()>=kMaxAzKeys)return {"invalid","Maximum 128 azimuth keys","azKeys"};
                next.azKeys.push_back({t,az,ease});
            }
            normalizePathAzKeys(next);
        }
    } else if(op=="path.removeAzimuthKey") {
        if(next.points.size()<2)return {"invalid","Path has fewer than two points","object"};
        int i=a.integer("index",0,(int)next.azKeys.size()-1);if(a.bad())return a.f;
        next.azKeys.erase(next.azKeys.begin()+i);
    } else if(op=="path.setAzimuthKeys") {
        auto* ks=a.get("keys").getArray();
        if(!ks)return {"invalid","Expected keys array","keys"};
        if(!ks->isEmpty()&&next.points.size()<2)return {"invalid","Path has fewer than two points","object"};
        if((size_t)ks->size()>kMaxAzKeys)return {"invalid","Expected 0..128 azimuth keys","keys"};
        std::vector<AzimuthKey> parsed;parsed.reserve((size_t)ks->size());
        for(const auto& item:*ks){
            AzimuthKey ak;const char* fld="keys";std::string msg;
            if(!parseAzKeyObj(item,ak,fld,msg))return {"invalid",msg,fld};
            parsed.push_back(ak);
        }
        next.azKeys=std::move(parsed);
        normalizePathAzKeys(next);
    } else if(op=="path.setLfo") {
        if(!a.has("lfo"))return {"invalid","lfo is required","lfo"};
        const char* fld="lfo";std::string msg;
        if(!parseLfoObj(a.get("lfo"),next.lfo,fld,msg))return {"invalid",msg,fld};
        if(next.points.size()<2 && !next.lfo.isDefault())return {"invalid","Path has fewer than two points","object"};
    } else if(op!="path.setFlags")return {"unsupported","Unknown path operation","op"};
    if(op=="path.set" || op=="path.setFlags"){
        if(op=="path.set") next.lfo=PathLfo{};
        if(a.has("lfo")){
            const char* fld="lfo";std::string msg;
            if(!parseLfoObj(a.get("lfo"),next.lfo,fld,msg))return {"invalid",msg,fld};
        }
        for(auto flag:{"enabled","closed","smooth","orient"})if(a.has(flag)) {
            bool value=a.boolean(flag);if(a.bad())return a.f;
            if(juce::String(flag)=="enabled")next.enabled=value;
            if(juce::String(flag)=="closed")next.closed=value;
            if(juce::String(flag)=="smooth")next.smooth=value;
            if(juce::String(flag)=="orient"){next.orient=value;if(value)next.lookAt=false;}
        }
        // E19 orientation: orientMode manual|along|lookAt, lookTarget (object index, listener = 32), lookPos [x,y,z] (fixed point), orientOffset (deg)
        if(a.has("orientMode")){
            const auto mode=a.get("orientMode").toString();
            if(mode=="manual"){next.orient=false;next.lookAt=false;}
            else if(mode=="along"){next.orient=true;next.lookAt=false;}
            else if(mode=="lookAt"){next.orient=false;next.lookAt=true;}
            else return {"invalid","Expected manual, along or lookAt","orientMode"};
        }
        if(a.has("lookTarget")){
            int t=a.integer("lookTarget",0,kListener);if(a.bad())return a.f;
            if(t==o)return {"invalid","An object cannot look at itself","lookTarget"};
            if(t!=kListener && t>=(int)m.sources.size())return {"invalid","Source does not exist","lookTarget"};
            next.lookTarget=t;next.lookPoint=false;
        }
        if(a.has("lookPos")){
            auto xyz=a.vecProp("lookPos",3,3,200);if(a.bad())return a.f;
            next.lookPos={xyz[0],xyz[1],xyz[2]};next.lookPoint=true;
        }
        if(a.has("orientOffset")){double off=a.num("orientOffset",-360.0,360.0);if(a.bad())return a.f;next.orientOffset=off;}
        if(next.lookAt&&next.orient)next.orient=false;
        if(op=="path.setFlags" && !next.closed && !next.times.empty())next.times.back().reset();
        if(a.has("laps")){int laps=a.integer("laps",1,64);if(a.bad())return a.f;next.laps=laps;}
        if(a.has("group")){int group=a.integer("group",-1,31);if(a.bad())return a.f;next.group=group;}
        if(a.has("start")){double st=a.num("start",0.0,100.0-kMinPathWindowGap);if(a.bad())return a.f;next.start=st;}
        if(a.has("end")){double en=a.num("end",kMinPathWindowGap,100.0);if(a.bad())return a.f;next.end=en;}
    }
    if(!validPath(next))return {"invalid","Invalid path points or timing",!validPathLfo(next.lfo)||(op=="path.setLfo")?"lfo":(!next.azKeys.empty()&&(op=="path.setAzimuthKey"||op=="path.setAzimuthKeys")?"azKeys":(!next.times.empty()||!next.easings.empty()||next.start!=0.0||next.end!=100.0)?"times":"points")};
    normalizePathTiming(next);
    normalizePathAzKeys(next);
    m.paths[(size_t)o]=std::move(next);
    return {};
}

Fail opModulation(const juce::var& p, Model& m, const Ctx&) {
    Args a(p);
    const auto op = a.get("op").toString();
    auto parseRoute = [&](const juce::var& item, ModulationRoute& out, std::string& fieldOut, std::string& msgOut) -> bool {
        if (!item.isObject()) { fieldOut = "route"; msgOut = "Expected modulation route object"; return false; }
        ModulationRoute r;
        if (item.hasProperty("enabled")) {
            const auto v = item.getProperty("enabled", {});
            if (!v.isBool()) { fieldOut = "enabled"; msgOut = "enabled must be a boolean"; return false; }
            r.enabled = (bool)v;
        }
        {
            const auto ov = item.getProperty("object", {});
            if (!(ov.isInt() || ov.isInt64() || (ov.isDouble() && std::floor((double)ov) == (double)ov))) {
                fieldOut = "object"; msgOut = "object must be an integer"; return false;
            }
            const int objIdx = (int)(double)ov;
            if (objIdx < 0 || objIdx > kListener || (objIdx < kListener && objIdx >= (int)m.sources.size())) {
                fieldOut = "object"; msgOut = "object does not exist"; return false;
            }
            r.object = objIdx;
        }
        {
            const auto dv = item.getProperty("driver", {});
            if (!dv.isString() || !parseModDriver(dv.toString().toStdString(), r.driver)) {
                fieldOut = "driver"; msgOut = "Invalid modulation driver"; return false;
            }
        }
        if (item.hasProperty("point")) {
            auto* arr = item.getProperty("point", {}).getArray();
            if (!arr || arr->size() != 3) { fieldOut = "point"; msgOut = "point must be [x, y, z]"; return false; }
            for (int k = 0; k < 3; ++k) {
                const auto& e = (*arr)[k];
                if (!Args::isNum(e) || !std::isfinite((double)e)) { fieldOut = "point"; msgOut = "point values must be finite numbers"; return false; }
            }
            r.pointX = (double)(*arr)[0];
            r.pointY = (double)(*arr)[1];
            r.pointZ = (double)(*arr)[2];
            if (r.pointX < -200.0 || r.pointX > 200.0 || r.pointY < -200.0 || r.pointY > 200.0 || r.pointZ < 0.0 || r.pointZ > 30.0) {
                fieldOut = "point"; msgOut = "point coordinates out of range"; return false;
            }
        }
        {
            const auto tv = item.getProperty("target", {});
            if (!tv.isString() || !parseModTarget(tv.toString().toStdString(), r.target)) {
                fieldOut = "target"; msgOut = "Invalid modulation target"; return false;
            }
        }
        if (item.hasProperty("targetObject")) {
            const auto tov = item.getProperty("targetObject", {});
            if (!(tov.isInt() || tov.isInt64() || (tov.isDouble() && std::floor((double)tov) == (double)tov))) {
                fieldOut = "targetObject"; msgOut = "targetObject must be an integer"; return false;
            }
            r.targetObject = (int)(double)tov;
        }
        if (modTargetIsGlobal(r.target)) {
            if (r.targetObject != -1) { fieldOut = "targetObject"; msgOut = "Global targets require targetObject -1"; return false; }
        } else {
            if (r.targetObject < -1 || r.targetObject >= (int)m.sources.size() || (r.targetObject == -1 && r.object == kListener)) {
                fieldOut = "targetObject"; msgOut = "targetObject does not refer to a valid source"; return false;
            }
        }
        if (item.hasProperty("hostOut")) {
            const auto hv = item.getProperty("hostOut", {});
            if (!(hv.isInt() || hv.isInt64() || (hv.isDouble() && std::floor((double)hv) == (double)hv))) {
                fieldOut = "hostOut"; msgOut = "hostOut must be an integer"; return false;
            }
            const int ho = (int)(double)hv;
            if (ho < 0 || ho > kMaxModHostOuts) {
                fieldOut = "hostOut"; msgOut = "hostOut out of range [0, 16]"; return false;
            }
            r.hostOut = ho;
        }
        if (item.hasProperty("attackMs")) {
            const auto av = item.getProperty("attackMs", {});
            if (!Args::isNum(av) || !std::isfinite((double)av) || (double)av < 0.0 || (double)av > 2000.0) {
                fieldOut = "attackMs"; msgOut = "attackMs out of range [0, 2000]"; return false;
            }
            r.attackMs = (double)av;
        }
        if (item.hasProperty("releaseMs")) {
            const auto rv = item.getProperty("releaseMs", {});
            if (!Args::isNum(rv) || !std::isfinite((double)rv) || (double)rv < 0.0 || (double)rv > 5000.0) {
                fieldOut = "releaseMs"; msgOut = "releaseMs out of range [0, 5000]"; return false;
            }
            r.releaseMs = (double)rv;
        }
        const auto cv = item.getProperty("curve", {});
        if (!cv.isObject()) { fieldOut = "curve"; msgOut = "curve object is required"; return false; }
        auto parseRange = [&](const char* key, double& loOut, double& hiOut) -> bool {
            auto* arr = cv.getProperty(key, {}).getArray();
            if (!arr || arr->size() != 2) { fieldOut = key; msgOut = std::string(key) + " must be [min, max]"; return false; }
            if (!Args::isNum((*arr)[0]) || !Args::isNum((*arr)[1])) { fieldOut = key; msgOut = std::string(key) + " elements must be numbers"; return false; }
            loOut = (double)(*arr)[0]; hiOut = (double)(*arr)[1];
            if (!std::isfinite(loOut) || !std::isfinite(hiOut) || std::abs(loOut) > 1e6 || std::abs(hiOut) > 1e6) {
                fieldOut = key; msgOut = std::string(key) + " out of range"; return false;
            }
            return true;
        };
        if (!parseRange("inputRange", r.curve.inputMin, r.curve.inputMax)) return false;
        if (!parseRange("outputRange", r.curve.outputMin, r.curve.outputMax)) return false;
        {
            const double tgtLo = modTargetMin(r.target);
            const double tgtHi = modTargetMax(r.target);
            r.curve.outputMin = std::clamp(r.curve.outputMin, tgtLo, tgtHi);
            r.curve.outputMax = std::clamp(r.curve.outputMax, tgtLo, tgtHi);
        }
        if (cv.hasProperty("invert")) {
            const auto iv = cv.getProperty("invert", {});
            if (!iv.isBool()) { fieldOut = "invert"; msgOut = "curve.invert must be a boolean"; return false; }
            r.curve.invert = (bool)iv;
        }
        if (cv.hasProperty("clamp")) {
            const auto clv = cv.getProperty("clamp", {});
            if (!clv.isBool()) { fieldOut = "clamp"; msgOut = "curve.clamp must be a boolean"; return false; }
            r.curve.clamp = (bool)clv;
        }
        auto* nodes = cv.getProperty("nodes", {}).getArray();
        if (!nodes || nodes->size() < kMinCurveNodes || nodes->size() > kMaxCurveNodes) {
            fieldOut = "nodes"; msgOut = "curve.nodes must have 2..8 points"; return false;
        }
        r.curve.nodeCount = (int)nodes->size();
        for (int ni = 0; ni < r.curve.nodeCount; ++ni) {
            const auto& nd = (*nodes)[ni];
            if (!nd.isObject()) { fieldOut = "nodes"; msgOut = "Each curve node must be an object"; return false; }
            const auto xv = nd.getProperty("x", {}), yv = nd.getProperty("y", {});
            if (!Args::isNum(xv) || !Args::isNum(yv)) { fieldOut = "nodes"; msgOut = "Curve node x and y must be numbers"; return false; }
            const double x = (double)xv, y = (double)yv;
            if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
                fieldOut = "nodes"; msgOut = "Curve node x and y must be in [0, 1]"; return false;
            }
            if (ni > 0 && x < r.curve.nodes[(size_t)(ni - 1)].x) {
                fieldOut = "nodes"; msgOut = "Curve node x values must be non-decreasing"; return false;
            }
            bool sm = false;
            if (nd.hasProperty("smooth")) {
                const auto smv = nd.getProperty("smooth", {});
                if (!smv.isBool()) { fieldOut = "nodes"; msgOut = "Curve node smooth must be a boolean"; return false; }
                sm = (bool)smv;
            }
            r.curve.nodes[(size_t)ni] = {x, y, sm};
        }
        if (!validModRoute(r)) { fieldOut = "route"; msgOut = "Invalid modulation route"; return false; }
        out = r;
        return true;
    };

    if (op == "modulation.addRoute") {
        if ((int)m.modRoutes.size() >= kMaxModRoutes) return {"budgetExceeded", "At most 64 modulation routes.", "routes"};
        if (!a.has("route")) return {"invalid", "route is required", "route"};
        ModulationRoute r; std::string fld, msg;
        if (!parseRoute(a.get("route"), r, fld, msg)) return {"invalid", msg, fld};
        m.modRoutes.push_back(r);
        return {};
    }
    if (op == "modulation.setRoute") {
        const int idx = a.integer("index", 0, kMaxModRoutes - 1);
        if (a.bad()) return a.f;
        if (idx >= (int)m.modRoutes.size()) return {"invalid", "Modulation route does not exist", "index"};
        if (!a.has("route")) return {"invalid", "route is required", "route"};
        ModulationRoute r; std::string fld, msg;
        if (!parseRoute(a.get("route"), r, fld, msg)) return {"invalid", msg, fld};
        m.modRoutes[(size_t)idx] = r;
        return {};
    }
    if (op == "modulation.removeRoute") {
        const int idx = a.integer("index", 0, kMaxModRoutes - 1);
        if (a.bad()) return a.f;
        if (idx >= (int)m.modRoutes.size()) return {"invalid", "Modulation route does not exist", "index"};
        m.modRoutes.erase(m.modRoutes.begin() + idx);
        return {};
    }
    if (op == "modulation.setRoutes") {
        auto* arr = a.get("routes").getArray();
        if (!arr) return {"invalid", "routes array is required", "routes"};
        if (arr->size() > kMaxModRoutes) return {"budgetExceeded", "At most 64 modulation routes.", "routes"};
        std::vector<ModulationRoute> nextRoutes;
        nextRoutes.reserve((size_t)arr->size());
        for (const auto& item : *arr) {
            ModulationRoute r; std::string fld, msg;
            if (!parseRoute(item, r, fld, msg)) return {"invalid", msg, fld};
            nextRoutes.push_back(r);
        }
        m.modRoutes = std::move(nextRoutes);
        return {};
    }
    return {"unsupported", "Unknown modulation operation", "op"};
}
// ============================================================================================ labels / coalescing
std::string lbl(const char* s) { return s; }
std::string refsCount(const juce::var& p, const char* one, const char* many) {
    auto* arr = p.getProperty("refs", {}).getArray();
    return arr && arr->size() > 1 ? many : one;
}
std::string noKey(const juce::var&) { return {}; }
std::string keyRefs(const juce::var& p) { return refsKey(p); }
std::string keyIndex(const juce::var& p) { return std::to_string((int)p.getProperty("index", -1)) + "/" + std::to_string((int)p.getProperty("channel", -1)); }
std::string keyRef(const juce::var& p) { return juce::JSON::toString(p.getProperty("ref", {}), true).toStdString() + std::to_string((int)p.getProperty("vertex", -1)); }
std::string keyEmpty(const juce::var&) { return "-"; }

std::string labelMetadata(const juce::var& p) {
    const bool l = p.hasProperty("locked"), h = p.hasProperty("hidden"), n = p.hasProperty("name");
    if (l && !h && !n) return (bool)p.getProperty("locked", false) ? "Lock surface" : "Unlock surface";
    if (h && !l && !n) return (bool)p.getProperty("hidden", false) ? "Hide surface" : "Show surface";
    if (n && !l && !h) return "Rename surface";
    return "Edit surface properties";
}
std::string labelCreate(const juce::var& p) {
    const juce::String k = p.getProperty("kind", {}).toString();
    return k == "wall" ? "Add wall" : k == "floorZone" ? "Add floor zone" : "Add ceiling zone";
}
std::string labelScale(const juce::var& p) { return "Scale room x" + juce::String((double)p.getProperty("factor", 1.0), 2).toStdString(); }
std::string labelOpening(const juce::var& p) { return (bool)p.getProperty("open", false) ? "Open ceiling" : "Close ceiling"; }
std::string labelRemoveSource(const juce::var& p) { return "Remove source " + std::to_string((int)p.getProperty("index", 0) + 1); }
std::string labelSourceConfig(const juce::var& p) {
    if (p.hasProperty("color") && !p.hasProperty("name") && !p.hasProperty("format") && !p.hasProperty("channels")
        && !p.hasProperty("firstIn") && !p.hasProperty("sum") && !p.hasProperty("freePoints"))
        return "Source colour";
    return "Edit source";
}
std::string keySourceConfig(const juce::var& p) { return p.hasProperty("color") ? std::string() : keyIndex(p); }
std::string labelMove(const juce::var& p) { return refsCount(p, "Move surface", "Move surfaces"); }
std::string labelDelete(const juce::var& p) { return refsCount(p, "Delete surface", "Delete surfaces"); }
std::string labelLink(const juce::var& p) { return refsCount(p, "Link surfaces", "Link surfaces"); }
}  // namespace

// Undo scope (the stack holds exactly the entries below with undoable = true):
//   undoable: every edit of the room document -- surfaces, materials, heights, air, clutter, roughness, diffraction, source
//   list / configuration / pattern / spread / point offsets, output layout, tail quality and the listener pose.
//   NOT undoable: layer masks (a view setting like Lock pan, stored with the scene but not an edit of the room),
//   the view state / selection / camera (editor session memory), plain automatable parameters (positions, gains, mute/solo,
//   mix ... belong to the host's automation and undo), loading a room / preset / project (clears the history instead).
// To reproduce the native editor's narrower undo (no air / clutter / sources / output / tail) set `undoable` to false for those.
static const Descriptor kDescriptors[] = {
    {"path.set",true,[](const juce::var&){return lbl("Set path");},noKey,opPath},
    {"path.clear",true,[](const juce::var&){return lbl("Clear path");},noKey,opPath},
    {"path.addPoint",true,[](const juce::var&){return lbl("Add path point");},noKey,opPath},
    {"path.movePoint",true,[](const juce::var&){return lbl("Move path point");},[](const juce::var& p){return p.getProperty("object",{}).toString().toStdString()+"/"+p.getProperty("index",{}).toString().toStdString();},opPath},
    {"path.removePoint",true,[](const juce::var&){return lbl("Remove path point");},noKey,opPath},
    {"path.setFlags",true,[](const juce::var&){return lbl("Path flags");},noKey,opPath},
    {"path.setPointTiming",true,[](const juce::var&){return lbl("Set point timing");},[](const juce::var& p){return p.getProperty("object",{}).toString().toStdString()+"/"+p.getProperty("index",{}).toString().toStdString();},opPath},
    {"path.setAzimuthKey",true,[](const juce::var&){return lbl("Set azimuth key");},[](const juce::var& p){return p.hasProperty("index")?p.getProperty("object",{}).toString().toStdString()+"/az/"+p.getProperty("index",{}).toString().toStdString():std::string();},opPath},
    {"path.removeAzimuthKey",true,[](const juce::var&){return lbl("Remove azimuth key");},noKey,opPath},
    {"path.setAzimuthKeys",true,[](const juce::var&){return lbl("Set azimuth keys");},noKey,opPath},
    {"path.setLfo",true,[](const juce::var&){return lbl("Set path LFO");},[](const juce::var& p){return p.getProperty("object",{}).toString().toStdString()+"/lfo";},opPath},
    {"modulation.addRoute",true,[](const juce::var&){return lbl("Add modulation route");},noKey,opModulation},
    {"modulation.setRoute",true,[](const juce::var&){return lbl("Edit modulation route");},[](const juce::var& p){const auto rt=p.getProperty("route",{});return std::string("mod/")+p.getProperty("index",{}).toString().toStdString()+"/"+rt.getProperty("driver",{}).toString().toStdString()+"/"+rt.getProperty("target",{}).toString().toStdString()+"/"+rt.getProperty("hostOut",0).toString().toStdString();},opModulation},
    {"modulation.removeRoute",true,[](const juce::var&){return lbl("Remove modulation route");},noKey,opModulation},
    {"modulation.setRoutes",true,[](const juce::var&){return lbl("Set modulation routes");},noKey,opModulation},
    {"output.autoMakeup", true, [](const juce::var&) {return lbl("Auto makeup");}, noKey,
        [](const juce::var&, Model& m, const Ctx& c)->Fail { float v=0; if(!c.proc.autoMakeupDb(v))return {"notReady","No complete energy result or enabled source", ""}; m.params["makeup"]=v; return {}; }},
    {"surface.translate", true, labelMove, keyRefs, opTranslate},
    {"surface.moveVertex", true, [](const juce::var&) { return lbl("Move corner"); }, keyRef, opMoveVertex},
    {"scene.joinCorners", true, [](const juce::var&) { return lbl("Join corners"); }, noKey, opJoinCorners},
    {"surface.create", true, labelCreate, noKey, opCreate},
    {"surface.delete", true, labelDelete, noKey, opDelete},
    {"surface.applyPreset", true, [](const juce::var&) { return lbl("Apply material preset"); }, noKey, opApplyPreset},
    {"surface.setAbsorption", true, [](const juce::var&) { return lbl("Edit absorption"); }, keyRefs, opSetAbsorption},
    {"surface.setTransmission", true, [](const juce::var&) { return lbl("Edit transmission"); }, keyRefs, opSetTransmission},
    {"surface.setScatter", true, [](const juce::var&) { return lbl("Edit scattering"); }, keyRefs, opSetScatter},
    {"surface.setHeights", true, [](const juce::var&) { return lbl("Edit surface height"); }, keyRefs, opSetHeights},
    {"surface.setMetadata", true, labelMetadata, keyRefs, opSetMetadata},
    {"surface.link", true, labelLink, noKey, opLink},
    {"surface.unlink", true, [](const juce::var&) { return lbl("Unlink surfaces"); }, noKey, opUnlink},
    {"room.scale", true, labelScale, keyEmpty, opScale},  // a scale gesture: the last factor wins (inside a transaction) / presses within 750 ms merge
    {"room.fitAutomationRange", true, [](const juce::var&) { return lbl("Fit automation range"); }, noKey, opFitAutomationRange},
    {"room.setAutomationRange", true, [](const juce::var& p) { return (bool)p.getProperty("reset", false) ? lbl("Reset automation range") : lbl("Set automation range"); }, noKey, opSetAutomationRange},
    {"room.setHeight", true, [](const juce::var&) { return lbl("Set room height"); }, keyEmpty, opSetHeight},
    {"room.setOpening", true, labelOpening, noKey, opSetOpening},
    {"room.setAir", true, [](const juce::var&) { return lbl("Change air"); }, keyEmpty, opSetAir},
    {"room.setClutter", true, [](const juce::var&) { return lbl("Change clutter"); }, keyEmpty, opSetClutter},
    {"room.setRoughness", true, [](const juce::var&) { return lbl("Change roughness"); }, keyEmpty, opSetRoughness},
    {"room.setDiffraction", true, [](const juce::var&) { return lbl("Change diffraction"); }, noKey, opSetDiffraction},
    {"room.clear", true, [](const juce::var&) { return lbl("Clear room"); }, noKey, opClear},
    {"source.add", true, [](const juce::var&) { return lbl("Add source"); }, noKey, opSourceAdd},
    {"source.remove", true, labelRemoveSource, noKey, opSourceRemove},
    {"source.setConfig", true, labelSourceConfig, keySourceConfig, opSourceSetConfig},
    {"source.setPattern", true, [](const juce::var&) { return lbl("Source directivity"); }, keyIndex, opSourcePattern},
    {"source.setDirectivity", true, [](const juce::var&) { return lbl("Source directivity profile"); }, keyIndex, opSourceDirectivity},
    {"source.setInstrument", true, [](const juce::var&) { return lbl("Source directivity profile"); }, keyIndex, opSourceDirectivity},
    {"source.setSpread", true, [](const juce::var&) { return lbl("Source spread"); }, keyIndex, opSourceSpread},
    {"source.setPointOffset", true, [](const juce::var&) { return lbl("Move source point"); }, keyIndex, opSourcePointOffset},
    {"output.set", true, [](const juce::var&) { return lbl("Change output"); }, keyEmpty, opOutputSet},
    {"tail.setQuality", true, [](const juce::var&) { return lbl("Change tail quality"); }, keyEmpty, opTailQuality},
    {"listener.set", true, [](const juce::var&) { return lbl("Move listener"); }, keyEmpty, opListenerSet},
};

const Descriptor* findDescriptor(const std::string& op) {
    for (const auto& d : kDescriptors) if (op == d.op) return &d;
    return nullptr;
}

bool isOtherServiceOp(const std::string& op) {
    static const char* ops[] = {"preset.list", "preset.load", "preset.save", "ir.export", "analysis.start", "analysis.cancel", "rt60.start", "rt60.cancel",
                                "telemetry.subscribe", "preferences.set", "comparison.capture", "comparison.activate", "source.loadDirectivityFile"};
    for (auto* o : ops) if (op == o) return true;
    return false;
}

}  // namespace verbp
