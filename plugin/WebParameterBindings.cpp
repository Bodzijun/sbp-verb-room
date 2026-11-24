#include "WebParameterBindings.h"
#include <cmath>
namespace verbweb {
namespace {
bool creativeOnly(const juce::String& id) { return id == "early" || id == "tail" || id == "doppler" || id.startsWith("decay") || id=="predelay" || id=="stereoWidth" || id=="freeze" || id=="duck" || id=="duckRelease"; }
}
WebParameterBindings::WebParameterBindings(VerbProcessor& p) : proc(p) {
    for (auto* base : p.getParameters())
        if (base->isAutomatable())
            if (auto* param = dynamic_cast<juce::RangedAudioParameter*>(base))
                if (auto* raw = p.apvts.getRawParameterValue(param->paramID))
                    bindings.push_back({param->paramID, param, raw});
}
WebParameterBindings::~WebParameterBindings() { endAll(); }
// A rebuild (source/output/preset load) ends every native gesture before the page learns about the new load serial.
// Late `change`/`end` of the gesture that the page still believed open are then expected (T9 E2): ignored, not an error toast.
void WebParameterBindings::endAll() { for (auto& b : bindings) if (b.gesture) { b.gesture = false; b.staleEnded = true; b.parameter->endChangeGesture(); } }
bool WebParameterBindings::changed() {
    bool change = false;
    for (auto& b : bindings) { const auto v = b.raw->load(); if (v != b.last) { b.last = v; change = true; } }
    return change;
}
juce::var WebParameterBindings::descriptors() const {
    juce::Array<juce::var> list;
    for (const auto& b : bindings) {
        const auto& id = b.id; const auto* p = b.parameter;
        auto d = obj({{"id", id}, {"index", p->getParameterIndex()}, {"name", p->getName(128)}, {"unit", ""},
            {"default", p->convertFrom0to1(p->getDefaultValue())}, {"automatable", true},
            {"modeClass", id == "mode" ? "modeSelector" : creativeOnly(id) ? "creativeOnly" : "always"}});
        auto* o = d.getDynamicObject();
        const bool source = id.startsWithChar('s') && id.substring(1, 2).containsOnly("123456789");
        const bool listener = id == "lx" || id == "ly" || id == "lz" || id == "laz" || id == "lpath";
        const char* group = source ? (id.endsWith("mute") ? "sourceMute" : id.endsWith("solo") ? "sourceSolo" : id.endsWith("g") ? "sourceGain" : "sourcePosition")
            : listener ? "listener" : id == "mode" ? "mode" : (id == "gain" || id == "makeup") ? "output" : id == "absorption" ? "absorption" : id.startsWith("solo") ? "solo" : creativeOnly(id) ? "creative" : "mix";
        o->setProperty("group", group);
        if (creativeOnly(id)) o->setProperty("physicalValue", id=="stereoWidth"?100:id=="duckRelease"?300:id == "early" || id == "tail" || id=="predelay" || id=="freeze" || id=="duck" ? 0 : 1);
        if (auto* c = dynamic_cast<const juce::AudioParameterChoice*>(p)) {
            o->setProperty("type", "choice"); o->setProperty("relay", "combo"); juce::Array<juce::var> a; for (auto& s : c->choices) a.add(s); o->setProperty("choices", a);
        } else if (dynamic_cast<const juce::AudioParameterBool*>(p)) { o->setProperty("type", "bool"); o->setProperty("relay", "toggle"); }
        else {
            const auto& r = p->getNormalisableRange();
            o->setProperty("type", "float"); o->setProperty("relay", "slider"); o->setProperty("min", r.start); o->setProperty("max", r.end); o->setProperty("step", r.interval); o->setProperty("decimals", 3);
            // The knob must follow the host parameter's own mapping: a centre skew only where the range really has one
            // (absorption / decay multipliers, centre 1); linear ranges such as Stereo width 0..200 % stay linear.
            const bool centre = std::abs(r.skew - 1.0f) > 1e-6f && !r.symmetricSkew;
            o->setProperty("skew", centre ? obj({{"type", "centre"}, {"centre", (double) r.convertFrom0to1(0.5f)}}) : obj({{"type", "none"}}));
            o->setProperty("unit", id.endsWith("path") ? "" : source || listener ? (id.endsWith("az") ? "deg" : id.endsWith("g") ? "dB" : "m") : id == "gain" || id == "makeup" || id == "early" || id == "tail" || id=="duck" ? "dB" : id=="predelay" || id=="duckRelease"?"ms":id == "mix" || id=="size" || id=="stereoWidth" ? "%" : "");
        }
        list.add(d);
    }
    return obj({{"schemaVersion", 1}, {"count", list.size()}, {"parameters", list}});
}
juce::var WebParameterBindings::snapshot(bool tailCurrent) const {
    auto values = obj(), texts = obj(), effectiveZ = obj();
    for (const auto& b : bindings) {
        const auto v = b.raw->load(); values.getDynamicObject()->setProperty(b.id, v);
        if (b.id.endsWithChar('z') && !b.id.endsWith("az")) effectiveZ.getDynamicObject()->setProperty(b.id, std::clamp((double)v, 0., proc.webMaximumZ()));
        texts.getDynamicObject()->setProperty(b.id, b.parameter->getText(b.parameter->convertTo0to1(v), 128));
    }
    const auto v = [&](const char* id) { return (double)prop(values, id); };
    const bool creative = v("mode") >= .5, solo = v("soloDirect") >= .5 || v("soloEarly") >= .5 || v("soloTail") >= .5;
    auto effective = obj({{"sizePercent",proc.webAppliedSize()},{"sizeRequested",v("size")},
        {"predelayMs",creative?v("predelay"):0},{"stereoWidthPercent",creative&&proc.numOutputChannels()>1?v("stereoWidth"):100},
        {"widthAvailable",proc.numOutputChannels()>1},{"freeze",creative&&v("freeze")>=.5},
        {"duckDb",creative?v("duck"):0},{"duckReleaseMs",creative?v("duckRelease"):300},{"duckReductionDb",proc.webDuckReductionDb()},
        {"lockReason",creative?"":"Locked in Physical: switch to Creative; stored values are preserved"},{"mode", creative ? "creative" : "physical"}, {"earlyDb", creative ? v("early") : 0}, {"tailDb", creative ? v("tail") : 0},
        {"doppler", creative ? v("doppler") : 1}, {"decay", array({creative ? v("decayLow") : 1, creative ? v("decayMid") : 1, creative ? v("decayHigh") : 1})},
        {"diffusion", v("diffusion")}, {"absorption", obj({{"requested", v("absorption")}, {"tailCurrent", tailCurrent}, {"applied", proc.webAppliedAbsorption() < 0 ? juce::var() : juce::var(proc.webAppliedAbsorption())}})}, {"zM", effectiveZ},
        {"mixEffective", obj({{"ignored", solo}, {"reason", solo ? "solo" : v("spatialDry") >= .5 ? "positionedDry" : "none"}})}});
    return obj({{"values", values}, {"effective", effective}, {"texts", texts}});
}
juce::var WebParameterBindings::apply(const juce::var& batch) {
    const auto reject=[&](const char* why){ endAll(); return obj({{"error",why}}); };
    const auto* list = batch.getArray();
    if (!list || list->size() > 1024) return reject("Invalid parameter batch");
    // Validate the entire batch before any host notification.
    auto active = std::vector<bool>(); for (const auto& b : bindings) active.push_back(b.gesture);
    bool creative = proc.apvts.getRawParameterValue("mode")->load() >= .5f;
    for (const auto& a : *list) {
        auto it = std::find_if(bindings.begin(), bindings.end(), [&](const Binding& b) { return b.id == prop(a, "id").toString(); });
        if (it == bindings.end()) return reject("Unknown parameter");
        const auto i = (size_t)(it - bindings.begin()); const auto type = prop(a, "type").toString();
        if (type == "begin") { if (active[i]) return reject("Gesture already open"); active[i] = true; }
        else if (type == "end") { if (!active[i]) { if (it->staleEnded) continue; return reject("Gesture is not open"); } active[i] = false; }
        else if (type == "change") {
            if (!active[i] && it->staleEnded) continue;
            auto value = prop(a, "value"); const double v = (double)value;
            if (!active[i] || !(value.isInt() || value.isInt64() || value.isDouble()) || !std::isfinite(v) || (!creative && creativeOnly(it->id))) return reject("Invalid or mode-locked parameter change");
            const auto& r = it->parameter->getNormalisableRange(); const double epsilon = 1e-6 * std::max(1., (double)r.end-r.start); if (v < r.start-epsilon || v > r.end+epsilon) return reject("Parameter out of range");
            if (it->id == "mode") creative = v >= .5;
        } else return reject("Unknown parameter action");
    }
    for (const auto& a : *list) {
        auto& b = *std::find_if(bindings.begin(), bindings.end(), [&](const Binding& x) { return x.id == prop(a, "id").toString(); });
        const auto type = prop(a, "type").toString();
        if (type == "begin") { b.gesture = true; b.staleEnded = false; b.parameter->beginChangeGesture(); }
        else if (!b.gesture && b.staleEnded) continue;
        else if (type == "end") { b.gesture = false; b.parameter->endChangeGesture(); }
        else { const auto& r=b.parameter->getNormalisableRange(); b.parameter->setValueNotifyingHost(b.parameter->convertTo0to1(std::clamp((float)(double)prop(a,"value"),r.start,r.end))); }
    }
    return obj({{"ok", true}});
}
}
