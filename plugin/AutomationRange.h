#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "MovementPath.h"
#include "SourceModel.h"
#include "layouts.hpp"
#include "scene.hpp"

namespace verbp {

struct AutomationRange {
    static constexpr double kGlobalMinXY = -200.0;
    static constexpr double kGlobalMaxXY = 200.0;
    static constexpr double kGlobalMinZ  = 0.0;
    static constexpr double kGlobalMaxZ  = 30.0;
    static constexpr double kMinSpanXY   = 1.0;
    static constexpr double kMinSpanZ    = 0.5;

    double xMin = kGlobalMinXY;
    double xMax = kGlobalMaxXY;
    double yMin = kGlobalMinXY;
    double yMax = kGlobalMaxXY;
    double zMin = kGlobalMinZ;
    double zMax = kGlobalMaxZ;

    bool isDefault() const noexcept {
        return std::abs(xMin - kGlobalMinXY) < 1e-6 && std::abs(xMax - kGlobalMaxXY) < 1e-6
            && std::abs(yMin - kGlobalMinXY) < 1e-6 && std::abs(yMax - kGlobalMaxXY) < 1e-6
            && std::abs(zMin - kGlobalMinZ) < 1e-6 && std::abs(zMax - kGlobalMaxZ) < 1e-6;
    }

    bool approxEqual(const AutomationRange& o, double eps = 1e-5) const noexcept {
        return std::abs(xMin - o.xMin) <= eps && std::abs(xMax - o.xMax) <= eps
            && std::abs(yMin - o.yMin) <= eps && std::abs(yMax - o.yMax) <= eps
            && std::abs(zMin - o.zMin) <= eps && std::abs(zMax - o.zMax) <= eps;
    }

    bool operator==(const AutomationRange& o) const noexcept { return approxEqual(o, 1e-6); }
    bool operator!=(const AutomationRange& o) const noexcept { return !(*this == o); }

    bool isValid() const noexcept {
        const auto ok = [](double v) { return std::isfinite(v); };
        return ok(xMin) && ok(xMax) && ok(yMin) && ok(yMax) && ok(zMin) && ok(zMax)
            && xMin >= kGlobalMinXY - 1e-9 && xMax <= kGlobalMaxXY + 1e-9 && (xMax - xMin) >= kMinSpanXY - 1e-9
            && yMin >= kGlobalMinXY - 1e-9 && yMax <= kGlobalMaxXY + 1e-9 && (yMax - yMin) >= kMinSpanXY - 1e-9
            && zMin >= kGlobalMinZ - 1e-9 && zMax <= kGlobalMaxZ + 1e-9 && (zMax - zMin) >= kMinSpanZ - 1e-9;
    }

    static AutomationRange sanitised(AutomationRange r) noexcept {
        auto fixAxis = [](double& lo, double& hi, double gLo, double gHi, double minSpan, double defLo, double defHi) {
            if (!std::isfinite(lo)) lo = defLo;
            if (!std::isfinite(hi)) hi = defHi;
            if (lo > hi) std::swap(lo, hi);
            lo = std::clamp(lo, gLo, gHi);
            hi = std::clamp(hi, gLo, gHi);
            if (hi - lo < minSpan) {
                const double mid = 0.5 * (lo + hi);
                lo = mid - 0.5 * minSpan;
                hi = mid + 0.5 * minSpan;
                if (lo < gLo) { lo = gLo; hi = std::min(gHi, lo + minSpan); }
                if (hi > gHi) { hi = gHi; lo = std::max(gLo, hi - minSpan); }
            }
        };
        fixAxis(r.xMin, r.xMax, kGlobalMinXY, kGlobalMaxXY, kMinSpanXY, kGlobalMinXY, kGlobalMaxXY);
        fixAxis(r.yMin, r.yMax, kGlobalMinXY, kGlobalMaxXY, kMinSpanXY, kGlobalMinXY, kGlobalMaxXY);
        fixAxis(r.zMin, r.zMax, kGlobalMinZ, kGlobalMaxZ, kMinSpanZ, kGlobalMinZ, kGlobalMaxZ);
        return r;
    }

    double axisMin(int k) const noexcept { return k == 0 ? xMin : (k == 1 ? yMin : zMin); }
    double axisMax(int k) const noexcept { return k == 0 ? xMax : (k == 1 ? yMax : zMax); }

    double clampAxis(int k, double v) const noexcept {
        const double lo = axisMin(k), hi = axisMax(k);
        return std::clamp(std::isfinite(v) ? v : lo, lo, hi);
    }

    bool contains(double x, double y, double z, double eps = 1e-5) const noexcept {
        return x >= xMin - eps && x <= xMax + eps
            && y >= yMin - eps && y <= yMax + eps
            && z >= zMin - eps && z <= zMax + eps;
    }

    void expandPoint(double x, double y, double z) noexcept {
        if (std::isfinite(x)) {
            const double cx = std::clamp(x, kGlobalMinXY, kGlobalMaxXY);
            xMin = std::min(xMin, cx);
            xMax = std::max(xMax, cx);
        }
        if (std::isfinite(y)) {
            const double cy = std::clamp(y, kGlobalMinXY, kGlobalMaxXY);
            yMin = std::min(yMin, cy);
            yMax = std::max(yMax, cy);
        }
        if (std::isfinite(z)) {
            const double cz = std::clamp(z, kGlobalMinZ, kGlobalMaxZ);
            zMin = std::min(zMin, cz);
            zMax = std::max(zMax, cz);
        }
    }

    AutomationRange scaled(double cx, double cy, double f, bool includeZ) const noexcept {
        if (isDefault()) return *this;
        AutomationRange out = *this;
        out.xMin = cx + (xMin - cx) * f;
        out.xMax = cx + (xMax - cx) * f;
        out.yMin = cy + (yMin - cy) * f;
        out.yMax = cy + (yMax - cy) * f;
        if (includeZ) {
            out.zMin = zMin * f;
            out.zMax = zMax * f;
        }
        return sanitised(out);
    }
};

// Thread-safe immutable NormalisableRange<float> holder for dynamic position parameters.
// Readers (including audio thread in VST3 processParameterChanges / APVTS Adapter) do one
// wait-free acquire load of a never-freed, never-mutated element inside std::deque.
struct AxisRangeStore {
    AxisRangeStore(float lo, float hi) { setRange(lo, hi); }

    const juce::NormalisableRange<float>& get() const noexcept {
        return *active.load(std::memory_order_acquire);
    }

    void setRange(float lo, float hi) {
        if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo) return;
        const uint64_t key = packKey(lo, hi);
        std::lock_guard<std::mutex> lock(writerMutex);
        if (auto* cur = active.load(std::memory_order_relaxed)) {
            if (cur->start == lo && cur->end == hi) return;
        }
        if (auto it = cache.find(key); it != cache.end()) {
            active.store(it->second, std::memory_order_release);
            return;
        }
        storage.emplace_back(lo, hi, 0.001f);
        const auto* ptr = &storage.back();
        cache.emplace(key, ptr);
        active.store(ptr, std::memory_order_release);
    }

    std::size_t uniqueSlotCount() const {
        std::lock_guard<std::mutex> lock(writerMutex);
        return storage.size();
    }

private:
    static uint64_t packKey(float a, float b) noexcept {
        uint32_t ua = 0, ub = 0;
        std::memcpy(&ua, &a, sizeof a);
        std::memcpy(&ub, &b, sizeof b);
        return (uint64_t(ua) << 32) | uint64_t(ub);
    }

    mutable std::mutex writerMutex;
    std::deque<juce::NormalisableRange<float>> storage;
    std::unordered_map<uint64_t, const juce::NormalisableRange<float>*> cache;
    std::atomic<const juce::NormalisableRange<float>*> active{nullptr};
};

class DynamicPositionParameter final : public juce::RangedAudioParameter {
public:
    DynamicPositionParameter(const juce::ParameterID& pid,
                             const juce::String& pname,
                             std::shared_ptr<AxisRangeStore> store,
                             float defVal,
                             const juce::String& unit)
        : juce::RangedAudioParameter(pid, pname,
              juce::AudioProcessorParameterWithIDAttributes().withLabel(unit)),
          rangeStore(std::move(store)),
          valueDefault(defVal),
          value(defVal) {}

    ~DynamicPositionParameter() override = default;

    float get() const noexcept { return value.load(std::memory_order_relaxed); }

    const juce::NormalisableRange<float>& getNormalisableRange() const override {
        return rangeStore->get();
    }

    float getValue() const override {
        return convertTo0to1(value.load(std::memory_order_relaxed));
    }

    void setValue(float newValue) override {
        value.store(convertFrom0to1(newValue), std::memory_order_relaxed);
    }

    float getDefaultValue() const override {
        return convertTo0to1(valueDefault);
    }

    int getNumSteps() const override {
        return juce::RangedAudioParameter::getNumSteps();
    }

    bool isDiscrete() const override { return false; }
    bool isBoolean() const override { return false; }

    juce::String getText(float v, int length) const override {
        float phys = convertFrom0to1(v);
        if (std::abs(phys) < 0.0005f) phys = 0.0f;
        juce::String text(phys, 3);
        return length > 0 ? text.substring(0, length) : text;
    }

    float getValueForText(const juce::String& text) const override {
        return convertTo0to1(text.getFloatValue());
    }

private:
    static_assert(std::atomic<float>::is_always_lock_free, "DynamicPositionParameter atomic float must be lock-free");
    std::shared_ptr<AxisRangeStore> rangeStore;
    const float valueDefault;
    std::atomic<float> value;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DynamicPositionParameter)
};

inline std::string posParamIdStr(int obj, int k) {
    static constexpr const char* kComp[4] = {"x", "y", "z", "az"};
    return (obj == kMaxSources ? std::string("l") : ("s" + std::to_string(obj + 1))) + kComp[k];
}

inline AutomationRange computeFitAutomationRange(
    const verb::Scene& s,
    const std::vector<SourceCfg>& sources,
    int outputLayout,
    double outputRadius,
    const std::array<MovementPath, kMaxSources + 1>& paths,
    const std::function<double(const char*)>& readParam,
    double marginM = -1.0)
{
    double rawXMin = 1e18, rawXMax = -1e18;
    double rawYMin = 1e18, rawYMax = -1e18;
    double rawZMin = 1e18, rawZMax = -1e18;
    bool hasXY = false, hasZ = false, hasGeom = false;

    auto accXY = [&](double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        rawXMin = std::min(rawXMin, x);
        rawXMax = std::max(rawXMax, x);
        rawYMin = std::min(rawYMin, y);
        rawYMax = std::max(rawYMax, y);
        hasXY = true;
    };
    auto accZ = [&](double z) {
        if (!std::isfinite(z)) return;
        rawZMin = std::min(rawZMin, z);
        rawZMax = std::max(rawZMax, z);
        hasZ = true;
    };

    for (const auto& w : s.walls) {
        hasGeom = true;
        accXY(w.ax, w.ay);
        accXY(w.bx, w.by);
        accZ(0.0);
        accZ(w.z0);
        accZ(w.z1 < 1e5 ? w.z1 : s.height);
    }
    for (const auto& z : s.floorZones) {
        if (!z.poly.empty()) hasGeom = true;
        for (const auto& pt : z.poly) accXY(pt.first, pt.second);
        accZ(z.z >= 0.0 ? z.z : 0.0);
    }
    for (const auto& z : s.ceilZones) {
        if (!z.poly.empty()) hasGeom = true;
        for (const auto& pt : z.poly) accXY(pt.first, pt.second);
        accZ(z.z >= 0.0 ? z.z : s.height);
    }
    if (hasGeom) {
        accZ(0.0);
        accZ(s.height);
    }

    const int nSrc = std::min((int)sources.size(), kMaxSources);
    for (int i = 0; i < nSrc; ++i) {
        const auto& cfg = sources[(size_t)i];
        const double sx = readParam(posParamIdStr(i, 0).c_str());
        const double sy = readParam(posParamIdStr(i, 1).c_str());
        const double sz = readParam(posParamIdStr(i, 2).c_str());
        const double azDeg = readParam(posParamIdStr(i, 3).c_str());
        accXY(sx, sy);
        accZ(sz);
        if (!cfg.sum) {
            const int nCh = cfg.effectiveChannels();
            if (cfg.freePoints && nCh > 0) {
                const double rad = azDeg * verb::kPi / 180.0;
                const double cs = std::cos(rad), sn = std::sin(rad);
                const auto pts = buildPoints(std::vector<SourceCfg>{cfg});
                for (const auto& pt : pts) {
                    const int ch = pt.channel;
                    if (ch < 0) continue;
                    const double ox = ch < (int)cfg.ox.size() ? (double)cfg.ox[(size_t)ch] : 0.0;
                    const double oy = ch < (int)cfg.oy.size() ? (double)cfg.oy[(size_t)ch] : 0.0;
                    const double oz = ch < (int)cfg.oz.size() ? (double)cfg.oz[(size_t)ch] : 0.0;
                    accXY(sx + ox * cs - oy * sn, sy + ox * sn + oy * cs);
                    accZ(sz + oz);
                }
            } else if (nCh > 1 && cfg.spread > 0.0) {
                accXY(sx - cfg.spread, sy - cfg.spread);
                accXY(sx + cfg.spread, sy + cfg.spread);
            }
        }
        const auto& path = paths[(size_t)i];
        for (const auto& pt : path.points) { accXY(pt[0], pt[1]); accZ(pt[2]); }
        if (path.lookAt && path.lookPoint) { accXY(path.lookPos[0], path.lookPos[1]); accZ(path.lookPos[2]); }
    }

    {
        const double lx = readParam("lx");
        const double ly = readParam("ly");
        const double lz = readParam("lz");
        accXY(lx, ly);
        accZ(lz);
        const auto lay = (verb::Layout)std::clamp(outputLayout, 0, (int)verb::Layout::AmbiX3);
        if (outputRadius > 0.0 && lay != verb::Layout::Mono && !verb::isAmbisonicLayout(lay)) {
            accXY(lx - outputRadius, ly - outputRadius);
            accXY(lx + outputRadius, ly + outputRadius);
        }
        const auto& path = paths[(size_t)kMaxSources];
        for (const auto& pt : path.points) { accXY(pt[0], pt[1]); accZ(pt[2]); }
        if (path.lookAt && path.lookPoint) { accXY(path.lookPos[0], path.lookPos[1]); accZ(path.lookPos[2]); }
    }

    if (!hasXY) { rawXMin = -5.0; rawXMax = 5.0; rawYMin = -5.0; rawYMax = 5.0; }
    if (!hasZ) { rawZMin = 0.0; rawZMax = std::max(3.0, s.height); }

    const double span = std::max(rawXMax - rawXMin, rawYMax - rawYMin);
    const double mXY = marginM > 0.0 ? marginM : std::clamp(0.1 * span, 1.0, 5.0);
    auto floorHalf = [](double v) { return std::floor(v * 2.0) * 0.5; };
    auto ceilHalf = [](double v) { return std::ceil(v * 2.0) * 0.5; };

    AutomationRange out;
    out.xMin = floorHalf(rawXMin - mXY);
    out.xMax = ceilHalf(rawXMax + mXY);
    out.yMin = floorHalf(rawYMin - mXY);
    out.yMax = ceilHalf(rawYMax + mXY);
    if (!hasGeom) {
        out.xMin = std::min(out.xMin, -5.0);
        out.xMax = std::max(out.xMax, 5.0);
        out.yMin = std::min(out.yMin, -5.0);
        out.yMax = std::max(out.yMax, 5.0);
    }
    out.zMin = 0.0;
    const double zBase = std::max(rawZMax, hasGeom ? s.height : std::max(3.0, s.height));
    out.zMax = ceilHalf(zBase + (s.ceilMat < 0 ? 2.0 : 1.0));
    return AutomationRange::sanitised(out);
}

inline int countObjectsOutsideRange(
    const AutomationRange& r,
    int numSources,
    const std::function<double(const char*)>& readParam)
{
    int count = 0;
    auto checkObj = [&](int obj) {
        const double x = readParam(posParamIdStr(obj, 0).c_str());
        const double y = readParam(posParamIdStr(obj, 1).c_str());
        const double z = readParam(posParamIdStr(obj, 2).c_str());
        if (!r.contains(x, y, z, 1e-5)) ++count;
    };
    const int n = std::min(numSources, kMaxSources);
    for (int i = 0; i < n; ++i) checkObj(i);
    checkObj(kMaxSources);
    return count;
}

}  // namespace verbp
