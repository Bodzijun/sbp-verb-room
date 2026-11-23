#include "../EditorBridge.h"
#include "../RoomLibrary.h"
#include "WebSchemaValidation.h"
#include "AudioAllocationGuard.h"
#include "geometry.hpp"
#if VERB_WEB_UI_ENABLED
#include "../VerbWebEditor.h"
#include "../PluginEditor.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif
#include <chrono>
#include <cstdio>
using namespace verbweb;
static int fails = 0, passes = 0, checked = 0;
static juce::Array<juce::var> captured;
static Schemas schemas;
static void check(bool ok, const std::string& what) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str()); ok ? ++passes : ++fails; }
static void record(const juce::var& m) {
    std::string why;
    const bool valid = schemas.validate(schemas.files["envelope.schema.json"], m, "envelope.schema.json", why, "$");
    if (!valid) { std::printf("SCHEMA ERROR: %s\n%s\n", why.c_str(), juce::JSON::toString(m, true).substring(0, 500).toRawUTF8()); ++fails; }
    ++checked; captured.add(m.clone());
}
static bool ready(VerbProcessor& p) { const auto until = juce::Time::getMillisecondCounterHiRes() + 15000; while (juce::Time::getMillisecondCounterHiRes() < until) { juce::MessageManager::getInstance()->runDispatchLoopUntil(5); if (p.getLoadState() == 2 && !p.isRoomLoadPending()) return true; if (p.getLoadState() == 3) return false; } return false; }
static std::string boxText(double w, double d, double h, int extraWalls = 0, bool furniture = false) {
    std::ostringstream o;
    o << "sr 48000\nlength 1.0\nheight " << h << "\norder 1\n"
      << "material Plaster 0.02 0.03 0.04 0.05 0.06 0.07 0.08 0.1 0.2\n"
      << "material Carpet 0.05 0.1 0.25 0.45 0.65 0.7 0.7 0.2 0.6\n"
      << "material Glass 0.18 0.06 0.04 0.03 0.02 0.02 0.02 0.05 0.1\n"
      << "floor Carpet\nceiling Plaster\n"
      << "wall 0 0 " << w << " 0 Plaster\nwall " << w << " 0 " << w << ' ' << d << " Plaster\n"
      << "wall " << w << ' ' << d << " 0 " << d << " Plaster\nwall 0 " << d << " 0 0 Glass\n";
    if (furniture) o << "wall 2 1 3 1 Glass 0 1.2\nzone floor Carpet 1 1 2 1 2 2 1 2\nzone ceiling Plaster at 2.5 1 1 2 1 2 2 1 2\n";
    for (int i = 0; i < extraWalls; ++i) o << "wall 1 1 " << 1.5 + 0.01 * i << " 1.5 Plaster\n";
    o << "listener 1 1 1.5 0\nsource A 1.25 1.25 1.5 0 0 0 0\nsource B 3 2 1.5 0 0 0 0\nreceiver L 1 1 1.5 55 0.5\nreceiver R 1 1 1.5 -55 0.5\n";
    return o.str();
}


struct Step { std::string name, json; };
struct HostListener : juce::AudioProcessorParameter::Listener {
    int changes = 0, begins = 0, ends = 0;
    void parameterValueChanged(int, float) override { ++changes; }
    void parameterGestureChanged(int, bool starting) override { starting ? ++begins : ++ends; }
};
#include "T7RaysTest.h" // Opt-in short live-pose throttle and query tests.
#include "T8ReviewTest.h"
#include "../BoundedDiffraction.h"
#include "T9ReviewTest.h"
#if VERB_WEB_UI_ENABLED
#include "LivePointerHarness.h"
#include "P9WebBake.h"
#endif
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI init;
    const auto started = juce::Time::getMillisecondCounterHiRes();
#if VERB_WEB_UI_ENABLED
    if (juce::SystemStats::getEnvironmentVariable("VERB_ONLY_P9", {}) == "1") return runP9WebBake();
#endif
    schemas.load(juce::File("web-ui/schema"));
    VerbProcessor p; p.setTailSettings({8000, false}); p.prepareToPlay(48000, 256);
    p.loadSceneText(boxText(5,4,3,0,true), "Bridge test box", true); check(ready(p), "tiny test room loads");
    if (juce::SystemStats::getEnvironmentVariable("VERB_T7_ONLY", {}).isNotEmpty()) return runT7RaysTest(p);
    if (juce::SystemStats::getEnvironmentVariable("VERB_T8_ONLY", {}).isNotEmpty()) return runT8ReviewTest(p);
    if (juce::SystemStats::getEnvironmentVariable("VERB_T9_ONLY", {}).isNotEmpty()) return runT9ReviewTest(p);
#if VERB_WEB_UI_ENABLED
    if (juce::SystemStats::getEnvironmentVariable("VERB_T6_LIVE", {}).isNotEmpty()) return runLivePointerHarness(p);
#endif
    juce::var lastParamSnapshot;
    EditorBridge bridge(p, [&](const juce::String& channel, const juce::var& value) {
        if (channel == "verb.message") record(value);
        else if (channel == "verb.parameters") lastParamSnapshot = value;
    });
    int serial = 0;
    const auto send = [&](const juce::var& payload, const juce::String& base = {}, const juce::String& tx = {}) {
        auto m = obj({{"schemaVersion", 1}, {"sessionId", "test-live"}, {"kind", "command"}, {"requestId", "test-" + juce::String(++serial)}, {"baseRevision", base.isNotEmpty() ? base : decimal(p.getRevision())}, {"payload", payload}});
        if (tx.isNotEmpty()) m.getDynamicObject()->setProperty("transactionId", tx);
        record(m); const auto replies = bridge.handle(m); if (const auto* a = replies.getArray()) for (const auto& r : *a) record(r); return replies;
    };
    const auto command = [&](const std::string& text) { return send(juce::JSON::parse(juce::String(text))); };
    const auto hello = bridge.handle(obj({{"schemaVersion",1},{"sessionId","test-live"},{"kind","hello"},{"payload",obj({{"supportedVersions",array({1})},{"ready",true}})}}));
    for (const auto& m : *hello.getArray()) record(m);
    check(hello.isArray() && prop((*hello.getArray())[0], "kind").toString() == "snapshot", "ready returns full validated snapshot");
    const auto snap = prop((*hello.getArray())[0], "payload");
    check((int)prop(prop(prop(snap,"parameters"),"descriptors"),"count") == 283, "snapshot contains all 283 live APVTS descriptors including P1");
    juce::File("out/T4/live-snapshot.json").replaceWithText(juce::JSON::toString((*hello.getArray())[0]));
    check((bool)prop(prop(prop(snap,"capabilities"),"features"),"output.meters").getProperty("available",false),"E12 native output meters capability available");
    command(R"({"op":"telemetry.subscribe","topics":["meters"]})");captured.clear();bridge.tick(true,980);bool outputEvent=false;
    for(const auto& message:captured){const auto event=prop(message,"payload");if(prop(event,"event").toString()=="meters.output")outputEvent=prop(event,"peaks").isArray()&&prop(event,"rms").isArray()&&prop(event,"clips").isArray()&&(int)prop(event,"outputChannels")==p.numOutputChannels();}
    check(outputEvent,"E12 schema validated output telemetry includes all channels peak/RMS/clip");
    const auto clearClip=command(R"({"op":"output.resetClip","channel":0})");check(prop((*clearClip.getArray())[0],"kind").toString()=="ack","E12 clip reset command accepted");
    auto* mix = p.apvts.getParameter("mix"); HostListener listener; mix->addListener(&listener);
    const auto gesture = bridge.parameterBatch(array({obj({{"type","begin"},{"id","mix"}}),obj({{"type","change"},{"id","mix"},{"value",37.5}}),obj({{"type","end"},{"id","mix"}})}));
    check((bool)prop(gesture,"ok") && listener.begins == 1 && listener.changes == 1 && listener.ends == 1, "gesture reaches host begin/change/end exactly once");
    mix->setValueNotifyingHost(mix->convertTo0to1(20.f)); const int changes = listener.changes; bridge.tick(true,1000); bridge.tick(true,1035);
    check(listener.changes == changes && (double)prop(prop(prop(bridge.snapshot(),"payload"),"parameters"),"values").getProperty("mix",{}) == 20, "host automation reflected without echo");
    mix->removeListener(&listener);
    const auto bad = bridge.parameterBatch(array({obj({{"type","begin"},{"id","early"}}),obj({{"type","change"},{"id","early"},{"value",-6}}),obj({{"type","end"},{"id","early"}})}));
    check(prop(bad,"error").isString(), "Physical mode rejects Creative-only change atomically");
    auto* absorb=p.apvts.getParameter("absorption");HostListener absListener;absorb->addListener(&absListener);
    const double lower=absorb->getNormalisableRange().start;
    const auto epsilon=bridge.parameterBatch(array({obj({{"type","begin"},{"id","absorption"}}),obj({{"type","change"},{"id","absorption"},{"value",lower-1e-8}}),obj({{"type","end"},{"id","absorption"}})}));
    check((bool)prop(epsilon,"ok")&&p.apvts.getRawParameterValue("absorption")->load()>=lower,"E11 epsilon undershoot clamps to legal endpoint");
    bridge.parameterBatch(array({obj({{"type","begin"},{"id","absorption"}})}));
    const auto overshoot=bridge.parameterBatch(array({obj({{"type","change"},{"id","absorption"},{"value",100.}})}));
    check(prop(overshoot,"error").isString()&&absListener.begins==absListener.ends,"E11 rejected change closes native gesture");
    absorb->removeListener(&absListener);absorb->setValueNotifyingHost(absorb->convertTo0to1(1.f));
    const auto& pr = verb::surfacePresets(); const std::string preset = pr[20].name, preset2 = pr[10].name;
    const std::vector<Step> steps = {
        {"surface.translate", R"({"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0.5,0.25]})"},
        {"surface.moveVertex (auto-linked corner)", R"({"op":"surface.moveVertex","ref":{"kind":"wall","index":1},"vertex":0,"positionM":[5.25,0.25]})"},
        {"surface.moveVertex (zone corner)", R"({"op":"surface.moveVertex","ref":{"kind":"floorZone","index":0},"vertex":2,"positionM":[2.5,2.5]})"},
        {"surface.create wall", R"({"op":"surface.create","kind":"wall","a":[1,1],"b":[1.75,2.25],"preset":")" + preset + R"("})"},
        {"surface.create floorZone", R"({"op":"surface.create","kind":"floorZone","vertices":[[3,2],[4,2],[4,3],[3,3]]})"},
        {"surface.create ceilingZone plate", R"({"op":"surface.create","kind":"ceilingZone","vertices":[[3,1],[4,1],[4,2],[3,2]],"heightM":2.25})"},
        {"surface.create ceilingZone opening", R"({"op":"surface.create","kind":"ceilingZone","vertices":[[0.5,0.5],[1,0.5],[1,1],[0.5,1]],"material":null})"},
        {"surface.applyPreset", R"({"op":"surface.applyPreset","refs":[{"kind":"wall","index":2}],"preset":")" + preset2 + R"("})"},
        {"surface.setAbsorption", R"({"op":"surface.setAbsorption","refs":[{"kind":"wall","index":3}],"bands":[{"band":0,"alpha":0.5},{"band":3,"alpha":0.75}]})"},
        {"surface.setTransmission", R"({"op":"surface.setTransmission","refs":[{"kind":"wall","index":4}],"bands":[{"band":2,"tau":0.25},{"band":5,"tau":0.125}]})"},
        {"surface.setScatter", R"({"op":"surface.setScatter","refs":[{"kind":"wall","index":2}],"scatterLF":0.5,"scatterHF":null})"},
        {"surface.setHeights (wall)", R"({"op":"surface.setHeights","refs":[{"kind":"wall","index":4}],"zBottomM":0.25,"zTopM":2})"},
        {"surface.setHeights (zone to plate)", R"({"op":"surface.setHeights","refs":[{"kind":"floorZone","index":0}],"heightM":0.5})"},
        {"surface.setMetadata (rename + hide)", R"({"op":"surface.setMetadata","refs":[{"kind":"wall","index":0}],"name":"Front","hidden":true})"},
        {"surface.setMetadata (floor locked)", R"({"op":"surface.setMetadata","refs":[{"kind":"floor","index":0}],"locked":true})"},
        {"surface.setMetadata (floor unlocked)", R"({"op":"surface.setMetadata","refs":[{"kind":"floor","index":0}],"locked":false})"},
        {"surface.link", R"({"op":"surface.link","refs":[{"kind":"wall","index":0},{"kind":"wall","index":1}],"groupName":"Stage"})"},
        {"surface.unlink", R"({"op":"surface.unlink","refs":[{"kind":"wall","index":0}]})"},
        {"room.scale (with objects)", R"({"op":"room.scale","factor":1.25,"includeObjects":true})"},
        {"room.setHeight", R"({"op":"room.setHeight","heightM":3.5})"},
        {"room.setOpening (ceiling open)", R"({"op":"room.setOpening","ref":{"kind":"ceiling","index":0},"open":true})"},
        {"room.setOpening (ceiling closed)", R"({"op":"room.setOpening","ref":{"kind":"ceiling","index":0},"open":false})"},
        {"room.setAir", R"({"op":"room.setAir","tempC":25,"humidityPct":40})"},
        {"room.setClutter", R"({"op":"room.setClutter","count":6,"minDistM":0.75,"seed":11})"},
        {"room.setRoughness", R"({"op":"room.setRoughness","roughness":0.25,"seed":5})"},
        {"room.setDiffraction", R"({"op":"room.setDiffraction","enabled":false})"},
        {"source.add", R"({"op":"source.add"})"},
        {"source.setConfig (stereo)", R"({"op":"source.setConfig","index":0,"format":1,"name":"Stereo pair"})"},
        {"source.setPattern", R"({"op":"source.setPattern","index":1,"pattern":0.5})"},
        {"source.setSpread", R"({"op":"source.setSpread","index":0,"spreadM":1.25})"},
        {"source.setConfig (free points)", R"({"op":"source.setConfig","index":0,"freePoints":true})"},
        {"source.setPointOffset", R"({"op":"source.setPointOffset","index":0,"channel":1,"offsetM":[0.25,0.5,0.125]})"},
        {"source.remove", R"({"op":"source.remove","index":1})"},
        {"output.set", R"({"op":"output.set","layout":2,"micPattern":7})"},
        {"output.set (Ambisonics format)", R"({"op":"output.set","ambisonicsNormalization":2,"ambisonicsLevelDb":-6})"},
        {"tail.setQuality", R"({"op":"tail.setQuality","baseRays":12000,"adaptive":true})"},
        {"listener.set", R"({"op":"listener.set","xM":2,"yM":2.5,"yawDeg":45})"},
        {"surface.delete", R"({"op":"surface.delete","refs":[{"kind":"wall","index":4}]})"},
        {"room.clear", R"({"op":"room.clear"})"},
    };

    for (const auto& step : steps) {
        const auto response = command(step.json); const auto& a = *response.getArray();
        check(prop(a[0],"kind").toString() == "ack", step.name + " through bridge");
        if (prop(a[0],"kind").toString() != "ack") std::printf("  %s\n",juce::JSON::toString(a[0],true).toRawUTF8());
    }
    command(R"({"op":"room.setHeight","ceilingExtent":"infinite"})");check(p.getDisplayScene().ceilingInfinite,"E11 extent command reaches scene");
    command(R"({"op":"history.undo"})");check(!p.getDisplayScene().ceilingInfinite,"E11 extent command undo");
    command(R"({"op":"history.redo"})");check(p.getDisplayScene().ceilingInfinite,"E11 extent command redo");
    command(R"({"op":"history.undo"})"); command(R"({"op":"history.redo"})");
    check(!p.getEditorSession().undoLabel().empty(), "history has human-readable undo label");
    command(R"({"op":"editor.setFlags","flags":{"keepRouting":false,"navLocked":true}})");
    command(R"({"op":"layers.set","layer":0,"mode":1})");
    command(R"({"op":"session.setViewState","viewState":{"page":"edit","projection":"front"}})");
    command(R"({"op":"preferences.set","patch":{"viewportTheme":"Amber","materialColours":{"Wood":"#ff0000"},"layoutChoice":"B"}})");
    command(R"({"op":"catalogs.list"})");
    const auto stale = send(obj({{"op","room.setHeight"},{"heightM",4}}), "0");
    check(prop((*stale.getArray())[0],"kind").toString() == "error" && prop((*stale.getArray()).getLast(),"kind").toString() == "snapshot", "forced revision gap returns error plus full resync");
    command(R"({"op":"transaction.begin","label":"test transaction"})"); // missing tx is an explicit validated error
    send(obj({{"op","transaction.begin"},{"label","test transaction"}}), {}, "drag");
    send(obj({{"op","room.setHeight"},{"heightM",3.2}}), {}, "drag"); send(obj({{"op","transaction.cancel"}}), {}, "drag");
    command(R"({"op":"preset.load","source":"builtin","id":0})"); check(ready(p), "built-in room loads through bridge");
    const auto rays = bridge.reflectionPaths(obj({{"sourceIndex",0},{"maxPaths",128},{"maxOrder",1}})); record(obj({{"schemaVersion",1},{"sessionId","test-live"},{"kind","event"},{"payload",rays}}));
    const auto paths = prop(rays,"paths"); check(paths.isArray() && paths.size() == 7, "rectangular built-in room: direct plus six first-order paths");
    bool analytic = true; const double c = verb::speedOfSound(p.getDisplayScene().tempC);
    if (paths.isArray()) for (const auto& path : *paths.getArray()) {
        const auto points = prop(path,"pointsM"); double length = 0;
        for (int i=1;i<points.size();++i) { auto a=(*points.getArray())[i-1], b=(*points.getArray())[i]; length += std::sqrt(std::pow((double)a[0]-(double)b[0],2)+std::pow((double)a[1]-(double)b[1],2)+std::pow((double)a[2]-(double)b[2],2)); }
        if ((int)prop(path,"order") == 1) {
            const auto scene=p.getEnergyScene(0,true); const verb::Geo g(scene); const auto ref=prop(path,"surfaces")[0];
            const auto kind=prop(ref,"kind").toString(); const int surf=kind=="wall"?(int)prop(ref,"index"):kind=="floor"?g.floorId():g.ceilId();
            const auto source=points[0], receiver=points[points.size()-1];
            const auto image=verb::mirror(g,{(double)source[0],(double)source[1],(double)source[2]},surf);
            analytic &= std::abs(length-verb::norm(image-verb::Vec3{(double)receiver[0],(double)receiver[1],(double)receiver[2]})) < 1e-6;
        }
        analytic &= std::abs(length-(double)prop(path,"lengthM")) < 1e-8 && std::abs(length/c-(double)prop(path,"delayS")) < 1e-10;
    }
    check(analytic, "all ray delays equal polyline length / speed of sound");
    juce::File("out/T4/rays.vocal-booth.json").replaceWithText(juce::JSON::toString(obj({{"fixtureMeta",obj({{"title","Live C++ built-in Vocal booth paths"},{"valueSources",obj({{"paths","code"}})},{"notes",array({"Captured by VerbWebBridgeTest. Engine modeled geometry; listener centre, static positions."})},{"unverified",array({"Moving-source retarded-time history"})}})},{"messages",array({obj({{"schemaVersion",1},{"sessionId","test-live"},{"kind","event"},{"payload",rays}})})}})));
    const auto portable = p.getRoomPresetText(); juce::String why; check(p.loadRoomPresetText(portable,why) && ready(p), "full .verbroom reload after bridge scene changes");
    command(R"({"op":"telemetry.subscribe","topics":["meters","tail","status","analysis"]})");
    bridge.tick(true,2500); // enable the accumulated-peak branch before guarded processing
    juce::AudioBuffer<float> block(2,256); juce::MidiBuffer midi; double impulsePeak = 0;
    for (int i=0;i<64;++i) { block.clear(); if(i==0) block.setSample(0,0,1.f); stage2Process(p,block,midi); impulsePeak = std::max(impulsePeak,(double)block.getMagnitude(0,256)); }
    check(p.takeWebInputPeak(0) >= 1.f && p.inputPeak(0) == 0.f, "web meter preserves impulse peak across silent blocks; native meter keeps last block semantics");
    check(stage2AudioAllocations==0, "active bridge processBlock has zero allocations (shared Stage2 guard)");
    check(impulsePeak > 0 && std::isfinite(impulsePeak), "loaded/reloaded room emits finite nonzero impulse");
    {verbp::MovementPath path;double x=p.apvts.getRawParameterValue("s1x")->load(),y=p.apvts.getRawParameterValue("s1y")->load(),z=p.apvts.getRawParameterValue("s1z")->load();path.points={{{x,y,z}},{{x+.2,y,z}}};check(p.setMovementPath(0,path),"E9b native bridge path accepted");const auto revision=p.getRevision();const auto depth=p.getEditorSession().undoDepth();
        auto reply=command(R"({"op":"path.bake","object":0})");check(prop((*reply.getArray())[0],"kind").toString()=="ack"&&p.getRevision()==revision&&p.getEditorSession().undoDepth()==depth,"E9b native bake ack changes no revision or undo");
        reply=command(R"({"op":"path.bakeCancel"})");check(prop((*reply.getArray())[0],"kind").toString()=="ack","E9b native cancel ack");const auto until=juce::Time::getMillisecondCounterHiRes()+3000;while(prop(p.pathBakeStatus(),"state").toString()!="cancelled"&&juce::Time::getMillisecondCounterHiRes()<until){block.clear();stage2Process(p,block,midi);juce::MessageManager::getInstance()->runDispatchLoopUntil(5);}bridge.tick(true,2990);bool found=false;for(const auto&m:captured){auto payload=prop(m,"payload");if(prop(payload,"event").toString()=="status.load"&&prop(prop(payload,"pathBake"),"state").toString()=="cancelled")found=true;}check(found,"E9b runtime bake state/cancel reason delivered in typed status event");p.setMovementPath(0,{});}
    {auto clr=command(R"({"op":"path.bakeClear"})");check(prop((*clr.getArray())[0],"kind").toString()=="ack","E24 native bakeClear ack");const auto until2=juce::Time::getMillisecondCounterHiRes()+3000;while(prop(p.pathBakeStatus(),"state").toString()!="notBaked"&&juce::Time::getMillisecondCounterHiRes()<until2){block.clear();stage2Process(p,block,midi);juce::MessageManager::getInstance()->runDispatchLoopUntil(5);}check(prop(p.pathBakeStatus(),"state").toString()=="notBaked","E24 bakeClear returns the bake state to notBaked");}
    bridge.tick(true,3000); const int beforeHidden=captured.size(); bridge.tick(false,3100); bridge.tick(false,5000); check(captured.size()==beforeHidden,"hidden editor emits no telemetry");
    const auto currentDeadline=juce::Time::getMillisecondCounterHiRes()+5000;
    while(!p.isWebSceneCurrent() && juce::Time::getMillisecondCounterHiRes()<currentDeadline) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    check(p.isWebSceneCurrent(), "Energy waits for current modeled geometry after preset reload");
    command(R"({"op":"analysis.start","source":-1,"heightM":1.2,"durationS":1,"resolution":16,"rays":12000,"components":7})");
    bool foundResult=false; const auto until=juce::Time::getMillisecondCounterHiRes()+10000;
    while (juce::Time::getMillisecondCounterHiRes()<until && !foundResult) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10); bridge.tick(true,juce::Time::getMillisecondCounterHiRes());
        for(const auto& e:captured) if(prop(prop(e,"payload"),"event").toString()=="analysis.result" && prop(prop(e,"payload"),"outcome").toString()=="done") {
            const auto result=prop(prop(e,"payload"),"result"); const auto blob=prop(result,"blob"); std::vector<std::byte> bytes;
            check(bridge.resource(prop(blob,"fullUrl").toString(),bytes) && bytes.size()==(size_t)(int)prop(blob,"fullByteLength"),"real Energy result binary resource matches descriptor");
            bool finite=true; const auto mask=(size_t)(int)prop(blob,"maskByteLength");
            for(size_t i=mask;i+4<=bytes.size();i+=4){float value;std::memcpy(&value,bytes.data()+i,4);finite &= std::isfinite(value)&&value>=0;}
            check(finite,"real Direct/Early/Tail binary cells are finite nonnegative energies"); foundResult=true; break;
        }
    }
    check(foundResult,"Energy Direct/Early worker completes");
    bridge.tick(false,juce::Time::getMillisecondCounterHiRes());
    const auto restored = bridge.handle(obj({{"schemaVersion",1},{"sessionId","test-live"},{"kind","hello"},{"payload",obj({{"supportedVersions",array({1})},{"ready",true}})}}));
    bool replayed=false;
    for(const auto& m:*restored.getArray()){record(m);if(prop(prop(m,"payload"),"event").toString()=="analysis.result")replayed=true;}
    check(replayed,"hello after hidden page replays the real Energy descriptor");
    {
        const auto preset = p.getRoomPresetText(); juce::MemoryBlock savedState; p.getStateInformation(savedState);
        // Same-room edits retain the resource and mark it stale, loads do not.
        check(p.editScene([](verb::Scene& s){s.humidity+=.01;}),"E12b ordinary room edit accepted");
        bridge.tick(true,juce::Time::getMillisecondCounterHiRes());
        bool stale=false;for(const auto& m:captured)stale|=prop(prop(m,"payload"),"event").toString()=="analysis.stale";
        check(stale,"E12b ordinary edit keeps analysis and emits STALE");
        for(int load=0;load<4;++load) {
            const auto pending=juce::Time::getMillisecondCounterHiRes()+10000;
            while(!p.isWebSceneCurrent()&&juce::Time::getMillisecondCounterHiRes()<pending){block.clear();p.processBlock(block,midi);juce::MessageManager::getInstance()->runDispatchLoopUntil(4);}
            command(R"({"op":"analysis.start","source":0,"heightM":1.2,"durationS":1,"resolution":16,"rays":12000,"components":7})");
            const size_t first=captured.size();bool done=false;juce::String url;
            const auto until=juce::Time::getMillisecondCounterHiRes()+10000;
            while(!done&&juce::Time::getMillisecondCounterHiRes()<until){bridge.tick(true,juce::Time::getMillisecondCounterHiRes());juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
                for(size_t i=first;i<captured.size();++i){auto e=prop(captured[i],"payload");if(prop(e,"event").toString()=="analysis.result"&&prop(e,"outcome").toString()=="done"){done=true;url=prop(prop(prop(e,"result"),"blob"),"fullUrl").toString();}}}
            check(done,"E12b result ready before load invalidation test");
            if(load==0){const auto r=verbp::builtinRooms()[5];p.loadRoomWithSetup(r.text,r.name,r.setup,r.micPattern);}
            if(load==1)p.loadSceneText(p.getRoomText(),"E12b txt load",true);
            if(load==2){juce::String why;check(p.loadRoomPresetText(preset,why),"E12b verbroom restore accepted");}
            if(load==3){auto running=command(R"({"op":"analysis.start","source":0,"heightM":1.2,"durationS":20,"resolution":96,"rays":192000,"components":7})");check(prop((*running.getArray())[0],"kind").toString()=="ack","E12b running analysis accepted before state LOAD cancels it");}
            if(load==3)p.setStateInformation(savedState.getData(),(int)savedState.getSize());
            const size_t before=captured.size();bridge.tick(true,juce::Time::getMillisecondCounterHiRes());
            bool cleared=false;for(size_t i=before;i<captured.size();++i)cleared|=prop(prop(captured[i],"payload"),"event").toString()=="analysis.cleared";
            std::vector<std::byte> obsolete;
            check(cleared&&!bridge.resource(url,obsolete),"E12b library/txt/verbroom/state load clears descriptor and old binary");
            const auto helloAgain=bridge.handle(obj({{"schemaVersion",1},{"sessionId","test-live"},{"kind","hello"},{"payload",obj({{"supportedVersions",array({1})},{"ready",true}})}}));
            bool oldReplay=false;for(const auto& m:*helloAgain.getArray()){record(m);oldReplay|=prop(prop(m,"payload"),"event").toString()=="analysis.result";}
            check(!oldReplay,"E12b hello after room load cannot replay old map");
        }
    }
    command(R"({"op":"analysis.cancel"})"); command(R"({"op":"rt60.cancel"})");
    // Wait for the preceding asynchronous state reload before structural commands.
    if (!ready(p)) return 1;
    // P1b bridge checks: lpath descriptor group, laps in snapshot/command, and bidirectional path automation.
    {
        const auto desc = prop(prop(prop(prop((*hello.getArray())[0], "payload"), "parameters"), "descriptors"), "parameters");
        juce::String lpathGroup, s1pathName, s2pathName, lpathName;
        if (auto* arr = desc.getArray()) {
            for (const auto& d : *arr) {
                const auto id = prop(d, "id").toString();
                if (id == "lpath") { lpathGroup = prop(d, "group").toString(); lpathName = prop(d, "name").toString(); }
                if (id == "s1path") s1pathName = prop(d, "name").toString();
                if (id == "s2path") s2pathName = prop(d, "name").toString();
            }
        }
        check(lpathGroup == "listener", "lpath parameter descriptor belongs to listener group");
        check(s1pathName.containsIgnoreCase("Source 1 path") && s2pathName.containsIgnoreCase("Source 2 path") && lpathName.containsIgnoreCase("Listener path"), "path parameters expose readable host names");
        const auto addSrcRes = command(R"({"op":"source.add"})");
        check(prop((*addSrcRes.getArray())[0], "kind").toString() == "ack", "source.add for Source 2 path test");
        check(ready(p), "engine ready after source.add for path test");
        const auto setPathRes = command(R"({"op":"path.set","object":32,"points":[[1,2,1.5],[5,2,1.5],[5,6,1.5]],"enabled":true,"closed":true,"laps":3})");
        check(prop((*setPathRes.getArray())[0], "kind").toString() == "ack", "path.set accepts laps");
        const auto setSrc2Res = command(R"({"op":"path.set","object":1,"points":[[2,2,1.5],[6,2,1.5]],"enabled":true})");
        check(prop((*setSrc2Res.getArray())[0], "kind").toString() == "ack", "path.set accepts Source 2 path");
        const auto postPathSnap = bridge.handle(obj({{"schemaVersion",1},{"sessionId","test-live"},{"kind","hello"},{"payload",obj({{"supportedVersions",array({1})},{"ready",true}})}}));
        const auto pathsProp = prop(prop(prop(prop((*postPathSnap.getArray())[0], "payload"), "document"), "paths")[32], "laps");
        check((int)pathsProp == 3, "document snapshot exposes listener path laps=3");

        auto* lpathParam = p.apvts.getParameter("lpath");
        auto* s2pathParam = p.apvts.getParameter("s2path");
        HostListener pathHost, src2Host;
        lpathParam->addListener(&pathHost);
        s2pathParam->addListener(&src2Host);
        const auto batchRes = bridge.parameterBatch(array({
            obj({{"type","begin"},{"id","lpath"}}),
            obj({{"type","change"},{"id","lpath"},{"value",0.25}}),
            obj({{"type","end"},{"id","lpath"}}),
            obj({{"type","begin"},{"id","s2path"}}),
            obj({{"type","change"},{"id","s2path"},{"value",0.625}}),
            obj({{"type","end"},{"id","s2path"}})
        }));
        lpathParam->removeListener(&pathHost);
        s2pathParam->removeListener(&src2Host);
        check((bool)prop(batchRes, "ok"), "UI parameterBatch updates lpath and s2path");
        check(pathHost.begins == 1 && pathHost.changes == 1 && pathHost.ends == 1 && std::abs(lpathParam->getValue() - 0.25f) < 1e-4f, "UI lpath gesture notifies host automation");
        check(src2Host.begins == 1 && src2Host.changes == 1 && src2Host.ends == 1 && std::abs(s2pathParam->getValue() - 0.625f) < 1e-4f, "UI s2path gesture notifies host automation");

        lastParamSnapshot = juce::var();
        lpathParam->setValueNotifyingHost(0.5f / 3.0f);
        s2pathParam->setValueNotifyingHost(0.375f);
        bridge.tick(true, juce::Time::getMillisecondCounterHiRes());
        const double emittedLpath = (double)prop(prop(lastParamSnapshot, "values"), "lpath");
        const double emittedS2path = (double)prop(prop(lastParamSnapshot, "values"), "s2path");
        check(lastParamSnapshot.isObject() && std::abs(emittedLpath - 0.5 / 3.0) < 1e-4 && std::abs(emittedS2path - 0.375) < 1e-4, "host lpath and s2path automation emit verb.parameters to UI");
        lpathParam->setValueNotifyingHost(0.f);
        s2pathParam->setValueNotifyingHost(0.f);
        bridge.tick(true, juce::Time::getMillisecondCounterHiRes() + 40);
        // E16 bridge checks: path.setFlags group, document omission when uncoupled, coupled bake & blocked ASCII reason.
        {
            const auto gL = command(R"({"op":"path.setFlags","object":32,"group":1})");
            const auto gS2 = command(R"({"op":"path.setFlags","object":1,"group":1})");
            check(prop((*gL.getArray())[0], "kind").toString() == "ack" && prop((*gS2.getArray())[0], "kind").toString() == "ack", "E16 path.setFlags accepts coupling group");
            const auto e16Snap = bridge.handle(obj({{"schemaVersion",1},{"sessionId","test-live"},{"kind","hello"},{"payload",obj({{"supportedVersions",array({1})},{"ready",true}})}}));
            const auto pathsDoc = prop(prop(prop((*e16Snap.getArray())[0], "payload"), "document"), "paths");
            auto* p0Obj = pathsDoc[0].getDynamicObject();
            check(p0Obj && !p0Obj->hasProperty("group"), "E16 uncoupled path omits group in document snapshot");
            check((int)prop(pathsDoc[32], "group") == 1 && (int)prop(pathsDoc[1], "group") == 1, "E16 coupled paths serialize group=1 in document snapshot");
            command(R"({"op":"path.set","object":32,"points":[[0.8,0.5,1.2],[1.2,0.5,1.2]],"enabled":true,"closed":false,"laps":1})");
            command(R"({"op":"path.set","object":1,"points":[[0.6,1.0,1.2],[1.4,1.0,1.2]],"enabled":true,"closed":false,"laps":1})");
            command(R"({"op":"path.setFlags","object":32,"group":1})");
            command(R"({"op":"path.setFlags","object":1,"group":1})");
            const auto bakeCmd = command(R"({"op":"path.bake","object":"all"})");
            check(prop((*bakeCmd.getArray())[0], "kind").toString() == "ack", "E16 coupled group bake command accepted");
            const auto e16Deadline = juce::Time::getMillisecondCounterHiRes() + 15000.0;
            while (prop(p.pathBakeStatus(), "state").toString() != "baked" && juce::Time::getMillisecondCounterHiRes() < e16Deadline) {
                block.clear(); stage2Process(p, block, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
            }
            bridge.tick(true, juce::Time::getMillisecondCounterHiRes() + 60);
            const auto st = p.pathBakeStatus();
            bool foundS2 = false, foundL = false;
            if (auto* arr = prop(st, "paths").getArray()) {
                for (const auto& it : *arr) {
                    if ((int)prop(it, "object") == 1) foundS2 = (int)prop(it, "group") == 1 && (int)prop(it, "drivenBy") == 1 && prop(it, "state").toString() == "baked";
                    if ((int)prop(it, "object") == 32) foundL = (int)prop(it, "group") == 1 && (int)prop(it, "drivenBy") == 1 && prop(it, "state").toString() == "baked";
                }
            }
            check(prop(st, "state").toString() == "baked" && foundS2 && foundL, "E16 coupled bake finishes cleanly with per-path driver metadata");
            const auto ugS2 = command(R"({"op":"path.setFlags","object":1,"group":-1})");
            check(prop((*ugS2.getArray())[0], "kind").toString() == "ack", "E16 uncoupling source 2 accepted");
            const auto blkDeadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
            while (prop(p.pathBakeStatus(), "state").toString() != "blocked" && juce::Time::getMillisecondCounterHiRes() < blkDeadline) {
                block.clear(); stage2Process(p, block, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
            }
            bridge.tick(true, juce::Time::getMillisecondCounterHiRes() + 120);
            const auto blkReason = prop(p.pathBakeStatus(), "reason").toString();
            bool reasonAscii = blkReason.isNotEmpty();
            for (unsigned char c : blkReason.toStdString()) reasonAscii &= (c >= 32 && c <= 126);
            check(prop(p.pathBakeStatus(), "state").toString() == "blocked" && reasonAscii, "E16 uncoupled listener+source reports blocked with pure-ASCII reason");
            command(R"({"op":"path.setFlags","object":32,"group":-1})");
        }
        const auto clrRes = command(R"({"op":"path.clear","object":32})");
        const auto clrSrc2Res = command(R"({"op":"path.clear","object":1})");
        const auto rmSrc2Res = command(R"({"op":"source.remove","index":1})");
        check(prop((*clrRes.getArray())[0], "kind").toString() == "ack" && prop((*clrSrc2Res.getArray())[0], "kind").toString() == "ack" && prop((*rmSrc2Res.getArray())[0], "kind").toString() == "ack", "path.clear cleans up listener and source 2 test paths");
    }
    {
        // E18 bridge checks: path timing fields omitted at default, start/end & point timing/easing commands, decreasing rejection, undo/redo.
        const auto e18Set = command(R"({"op":"path.set","object":0,"points":[[1,1,1.5],[2,1,1.5],[3,1,1.5]],"enabled":true})");
        check(prop((*e18Set.getArray())[0], "kind").toString() == "ack", "E18 path.set default accepted");
        auto snapDoc = [&]() {
            const auto h = bridge.handle(obj({{"schemaVersion",1},{"sessionId","test-live"},{"kind","hello"},{"payload",obj({{"supportedVersions",array({1})},{"ready",true}})}}));
            return prop(prop(prop((*h.getArray())[0], "payload"), "document"), "paths")[0];
        };
        const auto defDoc = snapDoc();
        auto* defObj = defDoc.getDynamicObject();
        check(defObj && !defObj->hasProperty("start") && !defObj->hasProperty("end") && !defObj->hasProperty("times") && !defObj->hasProperty("easings"),
              "E18 default path omits start/end/times/easings in bridge snapshot");

        const auto e18Win = command(R"({"op":"path.setFlags","object":0,"start":15,"end":85})");
        const auto e18Pt = command(R"({"op":"path.setPointTiming","object":0,"index":1,"time":40,"easing":"easeInOut"})");
        check(prop((*e18Win.getArray())[0], "kind").toString() == "ack" && prop((*e18Pt.getArray())[0], "kind").toString() == "ack",
              "E18 path.setFlags window and path.setPointTiming accepted");
        const auto timedDoc = snapDoc();
        const auto timedArr = prop(timedDoc, "times");
        const auto easingArr = prop(timedDoc, "easings");
        check(std::abs((double)prop(timedDoc, "start") - 15.0) < 1e-6 && std::abs((double)prop(timedDoc, "end") - 85.0) < 1e-6
              && timedArr.isArray() && timedArr.size() == 3 && std::abs((double)timedArr[1] - 40.0) < 1e-6
              && easingArr.isArray() && easingArr.size() == 3 && easingArr[1].toString() == "easeInOut",
              "E18 timed path serializes start/end/times/easings in bridge snapshot");

        command(R"({"op":"path.addPoint","object":0,"point":[4,1,1.5]})");
        const auto e18Bad = command(R"({"op":"path.setPointTiming","object":0,"index":2,"time":20})");
        check(prop((*e18Bad.getArray())[0], "kind").toString() == "error", "E18 decreasing point time returns bridge error");
        const auto afterBad = snapDoc();
        check(std::abs((double)prop(afterBad, "times")[1] - 40.0) < 1e-6 && prop(afterBad, "times")[2].isVoid(),
              "E18 rejected timing edit leaves document state unchanged");

        const auto e18Clr = command(R"({"op":"path.setPointTiming","object":0,"index":1,"time":null,"easing":"linear"})");
        check(prop((*e18Clr.getArray())[0], "kind").toString() == "ack", "E18 clearing point time to null accepted");
        const auto clearedDoc = snapDoc();
        auto* clearedObj = clearedDoc.getDynamicObject();
        check(clearedObj && !clearedObj->hasProperty("times") && !clearedObj->hasProperty("easings"),
              "E18 cleared point timing omits times/easings from snapshot");

        const auto undoRes = command(R"({"op":"history.undo"})");
        check(prop((*undoRes.getArray())[0], "kind").toString() == "ack"
              && std::abs((double)prop(snapDoc(), "times")[1] - 40.0) < 1e-6
              && prop(snapDoc(), "easings")[1].toString() == "easeInOut",
              "E18 history.undo restores cleared point time and easing");
        const auto redoRes = command(R"({"op":"history.redo"})");
        auto* redoObj = snapDoc().getDynamicObject();
        check(prop((*redoRes.getArray())[0], "kind").toString() == "ack"
              && redoObj && !redoObj->hasProperty("times") && !redoObj->hasProperty("easings"),
              "E18 history.redo reapplies clearing point time and easing");

        command(R"({"op":"path.clear","object":0})");
    }
    {
        // E22 bridge checks: Profiles initial dir, lastDirectivityDir preference + fallback, aim commands, validation, polarSlices, undo/redo.
        const auto expectedProfiles = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("SBP/Verb Room/Profiles");
        const auto initDir = bridge.directivityInitialDir(true);
        check(initDir == expectedProfiles && initDir.isDirectory(),
              "E22 directivityInitialDir defaults to Documents/SBP/Verb Room/Profiles and creates it");

        const auto tmpPrefDir = juce::File::getCurrentWorkingDirectory().getChildFile("out/E22/tmp-profiles-dir");
        tmpPrefDir.createDirectory();
        const auto prefSetRes = send(obj({{"op", "preferences.set"}, {"patch", obj({{"lastDirectivityDir", tmpPrefDir.getFullPathName()}})}}));
        check(prop((*prefSetRes.getArray())[0], "kind").toString() == "ack"
              && bridge.directivityInitialDir(false) == tmpPrefDir,
              "E22 preferences.set lastDirectivityDir overrides directivityInitialDir when folder exists");

        tmpPrefDir.deleteRecursively();
        check(bridge.directivityInitialDir(true) == expectedProfiles,
              "E22 missing lastDirectivityDir falls back to Profiles");
        command(R"({"op":"preferences.set","patch":{"lastDirectivityDir":""}})");

        auto src0 = [&]() {
            const auto h = bridge.handle(obj({{"schemaVersion",1},{"sessionId","test-live"},{"kind","hello"},{"payload",obj({{"supportedVersions",array({1})},{"ready",true}})}}));
            return prop(prop(prop((*h.getArray())[0], "payload"), "document"), "sources")[0];
        };

        check(prop((*command(R"({"op":"source.setDirectivity","index":0,"instrument":"brass_trumpet_bell_model","amount":1})").getArray())[0], "kind").toString() == "ack",
              "E22 select brass_trumpet_bell_model accepted");
        const auto sDef = src0();
        auto* sDefObj = sDef.getDynamicObject();
        const auto slices0 = prop(sDef, "polarSlices");
        check(sDefObj && !sDefObj->hasProperty("aimAzDeg") && !sDefObj->hasProperty("aimElDeg") && !sDefObj->hasProperty("aimRollDeg")
              && slices0.isArray() && slices0.size() == 3,
              "E22 snapshot omits zero aimAzDeg/aimElDeg/aimRollDeg and exposes polarSlices");

        check(prop((*command(R"({"op":"source.setDirectivity","index":0,"aimAzDeg":90,"aimElDeg":30,"aimRollDeg":-45})").getArray())[0], "kind").toString() == "ack",
              "E22 set aimAzDeg/aimElDeg/aimRollDeg accepted");
        const auto sAim = src0();
        check(std::abs((double)prop(sAim, "aimAzDeg") - 90.0) < 1e-6
              && std::abs((double)prop(sAim, "aimElDeg") - 30.0) < 1e-6
              && std::abs((double)prop(sAim, "aimRollDeg") + 45.0) < 1e-6,
              "E22 snapshot reports non-zero aimAzDeg=90, aimElDeg=30, aimRollDeg=-45");

        // Partial update preserves existing elevation while changing azimuth
        check(prop((*command(R"({"op":"source.setDirectivity","index":0,"aimAzDeg":45})").getArray())[0], "kind").toString() == "ack",
              "E22 partial aimAzDeg update accepted");
        const auto sPart = src0();
        check(std::abs((double)prop(sPart, "aimAzDeg") - 45.0) < 1e-6
              && std::abs((double)prop(sPart, "aimElDeg") - 30.0) < 1e-6,
              "E22 partial update preserves aimElDeg=30");

        // A different profile starts with its own orientation (hand-rotated angles are dropped); explicit values in the same command still apply.
        check(prop((*command(R"({"op":"source.setDirectivity","index":0,"instrument":"brass_tuba_bell_model"})").getArray())[0], "kind").toString() == "ack",
              "E22b switching profile accepted");
        const auto sSwitch = src0();
        auto* sSwitchObj = sSwitch.getDynamicObject();
        check(sSwitchObj && !sSwitchObj->hasProperty("aimAzDeg") && !sSwitchObj->hasProperty("aimElDeg") && !sSwitchObj->hasProperty("aimRollDeg"),
              "E22b switching to another profile resets the aim angles");
        check(prop((*command(R"({"op":"source.setDirectivity","index":0,"instrument":"brass_trumpet_bell_model","aimAzDeg":45,"aimElDeg":30,"aimRollDeg":-45})").getArray())[0], "kind").toString() == "ack",
              "E22b switching back with explicit aim accepted");
        const auto sBack = src0();
        check(std::abs((double)prop(sBack, "aimAzDeg") - 45.0) < 1e-6 && std::abs((double)prop(sBack, "aimElDeg") - 30.0) < 1e-6,
              "E22b explicit aim in the same command survives the profile change");

        // Reject out-of-range (unrecorded direct bridge call since schema also forbids 181) and non-zero aim on none
        const auto badRangeMsg = obj({{"schemaVersion", 1}, {"sessionId", "test-live"}, {"kind", "command"}, {"requestId", "test-bad-range"},
            {"baseRevision", decimal(p.getRevision())}, {"payload", juce::JSON::parse(R"({"op":"source.setDirectivity","index":0,"aimAzDeg":181})")}});
        const auto badRange = bridge.handle(badRangeMsg);
        const auto badNone = command(R"({"op":"source.setDirectivity","index":0,"instrument":"none","aimAzDeg":45})");
        check(badRange.isArray() && !badRange.getArray()->isEmpty() && prop((*badRange.getArray())[0], "kind").toString() == "error"
              && prop((*badNone.getArray())[0], "kind").toString() == "error",
              "E22 rejects out-of-bounds aimAzDeg and non-zero aim on instrument=none");

        // Use explicit transaction so EditorSession records a discrete undo step rather than coalescing rapid same-key edits
        send(obj({{"op", "transaction.begin"}}), {}, "tx-e22-aim");
        const auto res90 = send(juce::JSON::parse(R"({"op":"source.setDirectivity","index":0,"aimAzDeg":90,"aimElDeg":0,"aimRollDeg":0})"), {}, "tx-e22-aim");
        send(obj({{"op", "transaction.commit"}}), {}, "tx-e22-aim");
        check(prop((*res90.getArray())[0], "kind").toString() == "ack",
              "E22 set aimAzDeg=90, el=0, roll=0 accepted");
        const auto s90 = src0();
        auto* s90Obj = s90.getDynamicObject();
        const auto slices90 = prop(s90, "polarSlices");
        bool rotSliceOk = s90Obj && !s90Obj->hasProperty("aimElDeg") && !s90Obj->hasProperty("aimRollDeg")
            && slices90.isArray() && slices90.size() == 3;
        for (int b = 0; b < 3 && rotSliceOk; ++b) {
            if (std::abs((double)slices90[b][18] - (double)slices0[b][0]) > 1e-4) rotSliceOk = false;
        }
        check(rotSliceOk, "E22 aimAzDeg=90 omits zero el/roll and shifts polarSlices[b][18] to slices0[b][0]");

        // Undo / redo restores aim state
        check(prop((*command(R"({"op":"history.undo"})").getArray())[0], "kind").toString() == "ack", "E22 history.undo accepted");
        const auto sUndo = src0();
        check(std::abs((double)prop(sUndo, "aimAzDeg") - 45.0) < 1e-6 && std::abs((double)prop(sUndo, "aimElDeg") - 30.0) < 1e-6,
              "E22 history.undo restores aimAzDeg=45, aimElDeg=30");
        check(prop((*command(R"({"op":"history.redo"})").getArray())[0], "kind").toString() == "ack", "E22 history.redo accepted");
        const auto sRedo = src0();
        check(std::abs((double)prop(sRedo, "aimAzDeg") - 90.0) < 1e-6,
              "E22 history.redo reapplies aimAzDeg=90");

        command(R"({"op":"source.setDirectivity","index":0,"instrument":"none"})");
    }
    juce::File("out/T4/bridge-messages.json").replaceWithText(juce::JSON::toString(captured));
    std::printf("web_bridge: %d pass, %d fail, %d validated envelopes; %.3f s; snapshot %zu bytes\n",passes,fails,checked,(juce::Time::getMillisecondCounterHiRes()-started)/1000.,juce::JSON::toString((*hello.getArray())[0],true).getNumBytesAsUTF8());
#if VERB_WEB_UI_ENABLED
    // Exercise the exact embedded page, in an isolated test window/profile; no DAW is touched.
    const auto beforeSettings = settings();
    settings().prefFileOverride = juce::File("out/T4/live-editor/editor.ini");
    settings().udfRootOverride = juce::File("out/T4/live-editor/profile");
    settings().prefFileOverride.getParentDirectory().createDirectory();
    settings().prefFileOverride.getSiblingFile("web-preferences.json").replaceWithText("{\"layoutChoice\":\"A\"}");
    juce::String reason;
    const auto opened = juce::Time::getMillisecondCounterHiRes();
    auto editor = VerbWebEditor::create(p, reason);
    check(editor != nullptr, "live WebView2 editor creates: " + reason.toStdString());
    if (editor) {
        editor->addToDesktop(juce::ComponentPeer::windowIsTemporary); editor->setVisible(true);
        const auto eval = [&](const juce::String& script) {
            juce::String value; bool done = false;
            editor->testEvaluate(script, [&](juce::String v) { value = v; done = true; });
            const auto end = juce::Time::getMillisecondCounterHiRes() + 3000;
            while (!done && juce::Time::getMillisecondCounterHiRes() < end) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
            return value;
        };
        bool rendered = false; const auto end = opened + 15000;
        while (!rendered && juce::Time::getMillisecondCounterHiRes() < end) {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
            if (editor->isReady()) rendered = eval("String(document.getElementById('app')?.getAttribute('aria-busy') === 'false' && !!document.querySelector('.workspace'))") == "true";
        }
        const auto openMs = juce::Time::getMillisecondCounterHiRes() - opened;
        check(rendered, "real embedded frontend reaches first full render: " + std::to_string((int)openMs) + " ms");
        check(eval("String(document.querySelectorAll('.mock-dev,.mock-bar').length)") == "0", "real page has no mock/dev strip");
        editor->requestPing();
        const auto untilPing = juce::Time::getMillisecondCounterHiRes() + 3000;
        while (editor->getStats().pingDoneOk == 0 && juce::Time::getMillisecondCounterHiRes() < untilPing) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        check(editor->getStats().pingDoneOk > 0, "real page heartbeat round trip");
        eval("window.__t4LongTasks=0; window.__t4Observer = new PerformanceObserver(l=>{window.__t4LongTasks+=l.getEntries().reduce((s,e)=>s+e.duration,0)});window.__t4Observer.observe({type:'longtask',buffered:false});'ok'");
        const auto childCpu = [] {
            std::vector<PROCESSENTRY32W> entries;
            const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
            PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
            if(snapshot!=INVALID_HANDLE_VALUE){if(Process32FirstW(snapshot,&entry))do{entries.push_back(entry);}while(Process32NextW(snapshot,&entry));CloseHandle(snapshot);}
            std::set<DWORD> descendants{GetCurrentProcessId()}; bool changed=true;
            while(changed){changed=false;for(const auto& e:entries)if(descendants.count(e.th32ParentProcessID)&&!descendants.count(e.th32ProcessID)){descendants.insert(e.th32ProcessID);changed=true;}}
            uint64_t total=0;
            for(const auto& e:entries)if(descendants.count(e.th32ProcessID)&&_wcsicmp(e.szExeFile,L"msedgewebview2.exe")==0){
                const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,e.th32ProcessID);FILETIME c,x,k,u;
                if(process){if(GetProcessTimes(process,&c,&x,&k,&u)) total+=((uint64_t)k.dwHighDateTime<<32)|k.dwLowDateTime,total+=((uint64_t)u.dwHighDateTime<<32)|u.dwLowDateTime;CloseHandle(process);}
            }return total;
        };
        const auto webCpu0=childCpu();
        const auto initialDraws=eval("document.querySelector('.viewport').dataset.drawCount");
        FILETIME creation, exit, kernel0, user0, kernel1, user1;
        GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel0, &user0);
        const auto idleStart = juce::Time::getMillisecondCounterHiRes();
        while (juce::Time::getMillisecondCounterHiRes() - idleStart < 2000) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel1, &user1);
        const auto ticks=[](FILETIME t){return ((uint64_t)t.dwHighDateTime<<32)|t.dwLowDateTime;};
        const double cpuMs=(ticks(kernel1)-ticks(kernel0)+ticks(user1)-ticks(user0))/10000.;
        const double webCpuMs=(childCpu()-webCpu0)/10000.;
        const auto tasks=eval("String(window.__t4LongTasks)");
        std::printf("T4b initial idle viewport draws %s -> %s\n",initialDraws.toRawUTF8(),eval("document.querySelector('.viewport').dataset.drawCount").toRawUTF8());
        // T9 E4: Rays are OFF by default now, so the assertion switches them on first (the check itself is unchanged).
        eval("[...document.querySelectorAll('button')].find(e=>e.textContent === 'Rays')?.click();'ok'");
        bool engineLegend = false;
        for (int i = 0; i < 75 && !engineLegend; ++i) { juce::MessageManager::getInstance()->runDispatchLoopUntil(40); engineLegend = eval("String(document.querySelector('.ray-legend')?.textContent.includes('ENGINE RAYS'))") == "true"; }
        check(engineLegend, "live renderer receives real engine paths through store adapter");
        juce::File("out/T4/live-metrics.json").replaceWithText(juce::JSON::toString(obj({{"firstFullRenderMs",openMs},{"idleWindowMs",2000},{"hostProcessCpuMs",cpuMs},{"hostOneCoreCpuPercent",cpuMs/20.},{"browserLongTaskMs",tasks},{"browserProcessCpuMeasured",true},{"webviewProcessCpuMs",webCpuMs},{"webviewOneCoreCpuPercent",webCpuMs/20.}})));
        std::printf("LIVE: first render %.1f ms, host CPU %.1f ms / 2 s = %.2f%% one core (WebView subprocess CPU NOT included), long tasks %s ms\n",openMs,cpuMs,cpuMs/20.,tasks.toRawUTF8());
        std::printf("LIVE WebView subprocess CPU: %.1f ms / 2 s = %.2f%% one core\n",webCpuMs,webCpuMs/20.);
        if (juce::SystemStats::getEnvironmentVariable("VERB_T4B_PROFILE_ONLY",{}).isNotEmpty()) {
            eval(R"JS(window.__profile={clears:0,documentSignatures:0};const clear=CanvasRenderingContext2D.prototype.clearRect;CanvasRenderingContext2D.prototype.clearRect=function(...a){window.__profile.clears++;return clear.apply(this,a)};const stringify=JSON.stringify;JSON.stringify=function(v,...a){if(Array.isArray(v)&&v[0]?.scene&&v[0]?.sources)window.__profile.documentSignatures++;return stringify.call(this,v,...a)};'ok')JS");
            const auto warm=juce::Time::getMillisecondCounterHiRes();while(juce::Time::getMillisecondCounterHiRes()-warm<5000)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
            juce::Array<juce::var> samples;
            for(int i=0;i<3;++i){
                const auto before=childCpu();const auto count0=eval("JSON.stringify(window.__profile)");const auto t=juce::Time::getMillisecondCounterHiRes();
                while(juce::Time::getMillisecondCounterHiRes()-t<2000)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
                const auto ms=(childCpu()-before)/10000.;samples.add(obj({{"cpuMs",ms},{"oneCorePercent",ms/20.},{"before",juce::JSON::parse(count0)},{"after",juce::JSON::parse(eval("JSON.stringify(window.__profile)"))}}));
                std::printf("PROFILE window %d: %.3f ms / 2 s = %.3f%% one core; %s\n",i,ms,ms/20.,eval("JSON.stringify(window.__profile)").toRawUTF8());
            }
            const auto tag=juce::SystemStats::getEnvironmentVariable("VERB_T4B_PROFILE_ONLY",{});
            juce::File("out/T4b/profile-"+tag+".json").replaceWithText(juce::JSON::toString(obj({{"warmupMs",5000},{"windowMs",2000},{"initialOneCorePercent",webCpuMs/20.},{"samples",samples}})));
            editor.reset();settings()=beforeSettings;return fails ? 1 : 0;
        }
        // T4b: exercise the shipped page through DOM actions and the actual native backend.
        const auto waitJs = [&](const juce::String& expression, int timeout = 4000) {
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeout;
            do { juce::MessageManager::getInstance()->runDispatchLoopUntil(40); if (eval("String(" + expression + ")") == "true") return true; }
            while (juce::Time::getMillisecondCounterHiRes() < deadline);
            return false;
        };
        const auto clickText = [&](const juce::String& text) {
            eval("[...document.querySelectorAll('button')].find(e=>e.textContent === " + juce::JSON::toString(text) + ")?.click();'ok'");
        };
        const auto noMock = [&] {
            return eval(R"JS(String(!/mock|synthetic|no audio/i.test(document.body.innerText + [...document.querySelectorAll('[title],[aria-label]')].map(e=>e.title+' '+e.getAttribute('aria-label')).join(' '))))JS") == "true";
        };
        check(noMock(), "juce Main DOM and accessible labels contain no mock/synthetic/no-audio claims");
        check(eval("String(!!document.querySelector('.main-layout .rack') && !document.documentElement.dataset.layout)") == "true", "persisted layoutChoice A is harmless: only layout B is rendered");
        check(waitJs("document.querySelector('.ray-legend')?.textContent.includes('live poses') && document.querySelector('.ray-legend')?.textContent.includes('first order') && !document.querySelector('.ray-legend')?.textContent.includes('static')"), "partial engine rays carry a truthful first-order live-pose note");
        check(eval("String(document.querySelector('.rt-large')?.textContent !== 'â€”' && document.querySelector('.rt-caption')?.textContent.includes('Engine'))") == "true", "real RT60 arrives in status card");
        mix->setValueNotifyingHost(mix->convertTo0to1(37.5f));
        // The Mix knob lives in the Mix tab since the owner swapped it with Output gain in the macro row: open that tab for the two
        // readout checks and return to the tab that was active, so the rest of the test sees the page as before.
        const auto tabBefore = eval("document.querySelector('.bottom [role=tab][aria-selected=true]')?.textContent || ''");
        eval("[...document.querySelectorAll('.bottom [role=tab]')].find(e=>e.textContent==='Mix')?.click();'ok'");
        // T9 D1: the page now formats every value with one table that always carries the unit ("37.5 %"); the value shown is still the
        // plugin's own value after host automation, so the check compares against the unit-formatted plugin value.
        const auto mixText = juce::String(juce::String(mix->convertFrom0to1(mix->getValue()), 1).getDoubleValue()) + " %";
        check(waitJs("document.querySelector('[data-parameter=mix] .readout')?.value === " + juce::JSON::toString(mixText)), "live readout uses authoritative plugin text after automation");
        check(eval("String(document.querySelector('[data-parameter=mix] [role=slider]')?.getAttribute('aria-valuetext') === " + juce::JSON::toString(mixText) + ")") == "true", "accessible slider text is authoritative too");
        eval("[...document.querySelectorAll('.bottom [role=tab]')].find(e=>e.textContent===" + juce::JSON::toString(tabBefore) + ")?.click();'ok'");
        // All ids are taken from the backend catalogue, chosen in the live combobox.
        p.setTailSettings({8000,false});
        const auto rooms = verbp::builtinRooms();
        const auto waitRoom = [&](size_t roomId, bool full = false, uint64_t afterRevision = 0) {
            const auto deadline=juce::Time::getMillisecondCounterHiRes()+(full ? 15000 : 3000);
            do {juce::MessageManager::getInstance()->runDispatchLoopUntil(20);if(p.getSceneName()==juce::String(rooms[roomId].name)&&(!afterRevision || p.getRevision()!=afterRevision)&&(!full || (p.getLoadState()==2&&!p.isRoomLoadPending())))return true;}
            while(juce::Time::getMillisecondCounterHiRes()<deadline);return false;
        };
        for (size_t roomId = 0; roomId < rooms.size(); ++roomId) {
            const auto previousRevision=p.getRevision();
            const auto selector = ".dd-popup [data-value=\"builtin:" + juce::String((int)roomId) + "\"]";
            const bool option = eval("(()=>{document.querySelector('[aria-label=\"Room preset\"]')?.click();const e=document.querySelector(" + juce::JSON::toString(selector) + ");e?.dispatchEvent(new PointerEvent('pointerdown',{bubbles:true}));return String(!!e);})()") == "true";
            const bool loaded = waitRoom(roomId,false,previousRevision) && waitJs("document.querySelector('[aria-label=\"Room preset\"] .dd-text')?.textContent === " + juce::JSON::toString(juce::String(rooms[roomId].name)));
            check(option && loaded && p.getRevision()!=previousRevision && p.getSceneName() == juce::String(rooms[roomId].name), "live catalogue id " + std::to_string(roomId) + " dispatches native load " + rooms[roomId].name);
            if (!option || !loaded) std::printf("  option=%d loaded=%d native=%s status=%s DOM=%s\n",option,loaded,p.getSceneName().toRawUTF8(),p.getStatus().toRawUTF8(),eval("document.querySelector('.footer')?.innerText + ' / ' + document.querySelector('#notification')?.innerText").toRawUTF8());
        }
        // Return to a small room for editing, reload and short impulse checks.
        eval("document.querySelector('[aria-label=\"Room preset\"]')?.click();document.querySelector('.dd-popup [data-value=\"builtin:0\"]')?.dispatchEvent(new PointerEvent('pointerdown',{bubbles:true}));'ok'");
        check(waitRoom(0,true) && waitJs("document.querySelector('[aria-label=\"Room preset\"] .dd-text')?.textContent === " + juce::JSON::toString(juce::String(rooms[0].name))), "small room restored after catalogue coverage");
        eval("[...document.querySelectorAll('button')].find(e=>e.textContent.startsWith('Edit room'))?.click();'ok'"); check(waitJs("!!document.querySelector('.edit-inspector')"), "Edit page opens from real Main");
        clickText("Outliner"); check(waitJs("!!document.querySelector('.outliner')"), "real outliner flag reaches native snapshot");
        clickText("Plan");
        eval("document.querySelector('.viewport').frameAll();import('/src/viewport/camera.js').then(m=>window.__t4Project=m.project);'ok'");
        waitJs("typeof window.__t4Project==='function'");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        const auto drawScene=p.getDisplayScene();double minX=1e9,minY=1e9,maxX=-1e9,maxY=-1e9;
        for(const auto& w:drawScene.walls){minX=std::min({minX,w.ax,w.bx});maxX=std::max({maxX,w.ax,w.bx});minY=std::min({minY,w.ay,w.by});maxY=std::max({maxY,w.ay,w.by});}
        const int wallCount=(int)drawScene.walls.size();
        eval("document.querySelector('[aria-label^=\"Add wall\"]')?.click();'ok'");
        waitJs("document.querySelector('[aria-label^=\"Add wall\"]')?.getAttribute('aria-pressed')==='true'");
        const auto drawCamera=juce::JSON::toString(prop(p.getEditorSession().viewState(),"camera"),true);
        for(double x:{minX+.25*(maxX-minX),minX+.4*(maxX-minX)}){
            eval("(()=>{const c=document.querySelector('.viewport canvas'),r=c.getBoundingClientRect(),p=window.__t4Project(["+juce::String(x,6)+","+juce::String(minY+.3*(maxY-minY),6)+",0],"+drawCamera+",r.width,r.height);c.dispatchEvent(new PointerEvent('pointerdown',{bubbles:true,pointerId:1,button:0,clientX:r.x+p[0],clientY:r.y+p[1]}));})();'ok'");
        }
        const auto createdUntil=juce::Time::getMillisecondCounterHiRes()+4000;while((int)p.getDisplayScene().walls.size()==wallCount&&juce::Time::getMillisecondCounterHiRes()<createdUntil)juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        check((int)p.getDisplayScene().walls.size()==wallCount+1, "Draw wall tool issues real surface.create from viewport endpoints");
        eval("document.querySelector('[aria-label=\"Move\"]')?.click();document.querySelector('[aria-label=\"Visibility wall "+juce::String(wallCount+1)+"\"]')?.parentElement.querySelector('.object-name')?.click();'ok'");
        waitJs("!!document.querySelector('.material-picker')");
        eval("document.querySelector('.viewport').frameAll();'ok'");juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        if((int)p.getDisplayScene().walls.size()>wallCount){
            const auto wallBefore=p.getDisplayScene().walls[(size_t)wallCount];const auto moveCamera=juce::JSON::toString(prop(p.getEditorSession().viewState(),"camera"),true);
            // This assertion exercises free movement. A 9 px drag can now remain
            // at the starting grid coordinate, so hold Alt explicitly.
            eval("(()=>{const c=document.querySelector('.viewport canvas'),r=c.getBoundingClientRect(),p=window.__t4Project(["+juce::String(wallBefore.ax,6)+","+juce::String(wallBefore.ay,6)+",0],"+moveCamera+",r.width,r.height),capture=c.setPointerCapture;c.setPointerCapture=()=>{};try{for(const [type,dx] of [['pointerdown',0],['pointermove',9],['pointerup',9]])c.dispatchEvent(new PointerEvent(type,{bubbles:true,pointerId:1,button:0,altKey:true,clientX:r.x+p[0]+dx,clientY:r.y+p[1]}));}finally{c.setPointerCapture=capture;}})();'ok'");
            const auto movedUntil=juce::Time::getMillisecondCounterHiRes()+4000;while(p.getDisplayScene().walls[(size_t)wallCount].ax==wallBefore.ax&&juce::Time::getMillisecondCounterHiRes()<movedUntil)juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
            const auto moved=p.getDisplayScene().walls[(size_t)wallCount];
            check(moved.ax!=wallBefore.ax && moved.bx==wallBefore.bx && moved.by==wallBefore.by, "Move corner viewport drag sends canonical surface.moveVertex transaction");
        }else check(false,"Move corner requires created wall");
        eval("document.querySelector('[aria-label=\"Visibility wall 1\"]')?.click();'ok'");
        check(waitJs("document.querySelector('[aria-label=\"Visibility wall 1\"]')?.classList.contains('off')"), "per-object hide is canonical");
        eval("document.querySelector('[aria-label=\"Lock wall 1\"]')?.click();'ok'");
        check(waitJs("document.querySelector('[aria-label=\"Lock wall 1\"]')?.classList.contains('lit')"), "per-object lock is canonical");
        eval("document.querySelector('.outliner-row .object-name')?.click();'ok'");
        // The first row selects the floor, whose picker contains the full library.
        check(waitJs("document.querySelectorAll('.material-item').length >= 46"), "Edit material picker contains all 46 library materials");
        const auto materialName=juce::String(verb::surfacePresets()[20].name);
        eval("[...document.querySelectorAll('.material-item')].find(e=>e.title.startsWith(" + juce::JSON::toString(materialName.replaceCharacter('_',' ')+" ") + "))?.click();'ok'");
        check(waitJs("document.querySelector('.material-item.sel')?.title.startsWith("+juce::JSON::toString(materialName.replaceCharacter('_',' ')+" ")+")"), "full catalogue material selection is canonical after native command");
        check(noMock(), "juce Edit DOM and accessible labels contain no mock claims");
        clickText("Scene"); waitJs("!!document.querySelector('.acc-head')");
        eval("[...document.querySelectorAll('.acc-head')].find(e=>e.textContent.startsWith('Air'))?.click();'ok'");
        waitJs("!!document.querySelector('[aria-label=\"Temperature value\"]')");
        eval("{const e=document.querySelector('[aria-label=\"Temperature value\"]');if(e){e.value='24';e.dispatchEvent(new Event('change',{bubbles:true}));}}'ok'");
        check(waitJs("document.querySelector('[aria-label=\"Temperature value\"]')?.value === '24'") && p.getDisplayScene().tempC == 24, "Air control sends real room.setAir");
        check(waitJs("!document.querySelector('[data-action=undo]')?.disabled && document.querySelector('[data-action=undo]')?.title.length > 7"), "Undo enables with backend action label");
        eval("document.querySelector('[data-action=undo]')?.click();'ok'");
        check(waitJs("!document.querySelector('[data-action=redo]')?.disabled") && p.getDisplayScene().tempC != 24, "Undo button calls native history and enables labelled Redo");
        eval("document.querySelector('[data-action=redo]')?.click();'ok'");
        check(waitJs("document.querySelector('[data-action=redo]')?.disabled") && p.getDisplayScene().tempC == 24, "Redo button reapplies real Air edit");
        // Native layer and clutter commands, then one-time scaling from actual controls.
        eval("[...document.querySelectorAll('.layer-bar button')].find(e=>/walls$/i.test(e.textContent))?.click();'ok'");
        check(waitJs("!!document.querySelector('.layer-filters [aria-label=\"walls: locked\"] svg')") && p.getLayerModes()[0] == 1, "layer lock reaches native snapshot");
        eval("[...document.querySelectorAll('.layer-bar button')].find(e=>/walls$/i.test(e.textContent))?.click();'ok'");
        check(waitJs("!!document.querySelector('.layer-filters [aria-label=\"walls: hidden\"] svg')") && p.getLayerModes()[0] == 2, "layer hide reaches native snapshot");
        eval("[...document.querySelectorAll('.layer-bar button')].find(e=>/walls$/i.test(e.textContent))?.click();'ok'");
        eval("[...document.querySelectorAll('.acc-head')].find(e=>e.textContent==='Clutter')?.click();'ok'");
        waitJs("!!document.querySelector('[aria-label=\"Source clearance value\"]')");
        eval("{const e=document.querySelector('[aria-label=\"Source clearance value\"]');if(e){e.value='0.8';e.dispatchEvent(new Event('change',{bubbles:true}));}}'ok'");
        check(waitJs("document.querySelector('[aria-label=\"Source clearance value\"]')?.value === '0.8'") && std::abs(p.getDisplayScene().clutter.minDist-.8)<1e-5, "clutter clearance control sends real command");
        const int seedBefore = p.getDisplayScene().clutter.seed;
        eval("document.querySelector('button[aria-label=\"Randomize clutter seed\"]')?.click();'ok'");
        const bool seedChanged = waitJs("(()=>{const n=Number(document.querySelector('[aria-label=\"Seed value\"]')?.value);return n>=1 && n<=9999 && n!==" + juce::String(seedBefore) + ";})()");
        const auto randomized = p.getDisplayScene();
        check(seedChanged && randomized.clutter.seed != seedBefore && randomized.clutter.seed >= 1 &&
              randomized.clutter.seed <= 9999 && randomized.roughSeed == randomized.clutter.seed &&
              std::abs(randomized.clutter.minDist - .8) < 1e-5,
              "F1 Randomize changes the real clutter and roughness seeds together");
        eval("[...document.querySelectorAll('.acc-head')].find(e=>e.textContent.startsWith('Scale room'))?.click();'ok'");
        waitJs("!!document.querySelector('[aria-label=\"Scale factor\"]')");
        eval("document.querySelector('[aria-label=\"Lock wall 1\"]')?.click();'ok'");
        check(waitJs("!document.querySelector('[aria-label=\"Lock wall 1\"]')?.classList.contains('lit')"), "unlock before one-time room scale");
        const auto beforeScale=p.getDisplayScene().walls[0];
        const auto widthBefore=std::hypot(beforeScale.bx-beforeScale.ax,beforeScale.by-beforeScale.ay);
        const auto scaleRevision=p.getRevision();
        eval("{const e=document.querySelector('[aria-label=\"Scale factor\"]');if(e)e.value='1.1';}[...document.querySelectorAll('button')].find(e=>e.textContent==='Apply scale')?.click();'ok'");
        const auto scaleUntil=juce::Time::getMillisecondCounterHiRes()+4000;while(p.getRevision()==scaleRevision&&juce::Time::getMillisecondCounterHiRes()<scaleUntil)juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        const auto afterScale=p.getDisplayScene().walls[0];
        check(widthBefore>0 && std::abs(std::hypot(afterScale.bx-afterScale.ax,afterScale.by-afterScale.ay)-widthBefore*1.1)<1e-4, "Scale room button applies real one-time geometry scale");
        eval("document.querySelector('[aria-label=\"Lock wall 1\"]')?.click();'ok'");waitJs("document.querySelector('[aria-label=\"Lock wall 1\"]')?.classList.contains('lit')");
        const auto editedPreset = p.getRoomPresetText(); juce::String editedWhy;
        check(p.loadRoomPresetText(editedPreset,editedWhy) && ready(p), "edited scene survives full preset reload");
        check(waitJs("document.querySelector('[aria-label=\"Temperature value\"]')?.value === '24'"), "canonical Air survives automatic resync in live DOM");
        check(waitJs("document.querySelector('[aria-label=\"Visibility wall 1\"]')?.classList.contains('off') && document.querySelector('[aria-label=\"Lock wall 1\"]')?.classList.contains('lit')"), "per-object hide and lock survive preset reload/resync");
        double editedPeak=0; bool editedFinite=true;
        for(int i=0;i<32;++i){block.clear();if(i==0)block.setSample(0,0,1.f);p.processBlock(block,midi);editedPeak=std::max(editedPeak,(double)block.getMagnitude(0,256));for(int c=0;c<block.getNumChannels();++c)for(int n=0;n<block.getNumSamples();++n)editedFinite &= std::isfinite(block.getSample(c,n));}
        check(editedFinite && editedPeak>0, "edited/reloaded room emits finite nonzero short impulse");
        eval("[...document.querySelectorAll('button')].find(e=>e.textContent.endsWith('Done'))?.click();'ok'");
        check(waitJs("!!document.querySelector('.main-context .selected-card .card-title') && !document.querySelector('.material-picker')"), "Done returns to Main without surface controls");
        check(eval("String(document.querySelector('.selected-card .card-title')?.textContent === 'Scene')") == "true", "surface selection becomes Scene status on Main");
        // Cancel only native dialogs owned by THIS isolated test process; no app/process is killed.
        for(const auto* action : {"Save","Load file","Export IR"}) {
            clickText(action);
            const bool choosing=waitJs("document.querySelector('.footer-status')?.textContent.includes('Choose')");
            bool cancelled=false;const auto deadline=juce::Time::getMillisecondCounterHiRes()+4000;
            while(!cancelled && juce::Time::getMillisecondCounterHiRes()<deadline){
                EnumWindows([](HWND w,LPARAM context)->BOOL{DWORD pid=0;GetWindowThreadProcessId(w,&pid);wchar_t cls[32]{};GetClassNameW(w,cls,32);if(pid==GetCurrentProcessId()&&IsWindowVisible(w)&&wcscmp(cls,L"#32770")==0){PostMessageW(w,WM_CLOSE,0,0);*reinterpret_cast<bool*>(context)=true;}return TRUE;},reinterpret_cast<LPARAM>(&cancelled));
                juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
            }
            const bool cancelledStatus=waitJs("document.querySelector('.footer-status')?.textContent.includes('cancelled')");
            check(choosing && cancelled && cancelledStatus,std::string(action)+" opens a native chooser and renders cancellation status");
            if(!choosing || !cancelled || !cancelledStatus)std::printf("  chooser choosing=%d windowClosed=%d statusCancelled=%d DOM=%s\n",choosing,cancelled,cancelledStatus,eval("document.querySelector('.footer-status')?.textContent+' / '+document.querySelector('#notification')?.textContent").toRawUTF8());
        }
        clickText("Energy"); check(waitJs("!!document.querySelector('.analysis')"), "Energy page opens with real bridge");
        check(noMock(), "juce Energy DOM and accessible labels contain no synthetic labels");
        eval("document.querySelector('[aria-label=\"Energy resolution\"]')?.click();document.querySelector('.dd-popup [data-value=\"16\"]')?.dispatchEvent(new PointerEvent('pointerdown',{bubbles:true}));'ok'");
        waitJs("document.querySelector('[aria-label=\"Energy resolution\"]')?.value==='16'");
        eval("document.querySelector('.energy-advanced').open=true;document.querySelector('[aria-label=\"Energy quality\"]')?.click();document.querySelector('.dd-popup [data-value=\"12000\"]')?.dispatchEvent(new PointerEvent('pointerdown',{bubbles:true}));'ok'");
        waitJs("document.querySelector('[aria-label=\"Energy quality\"]')?.value==='12000'");
        eval("{const e=document.querySelector('[aria-label=\"Duration value\"]');if(e){e.value='1';e.dispatchEvent(new Event('change',{bubbles:true}));}}'ok'");
        waitJs("document.querySelector('[aria-label=\"Duration value\"]')?.value==='1'");
        const auto modeledUntil=juce::Time::getMillisecondCounterHiRes()+5000;while(!p.isWebSceneCurrent()&&juce::Time::getMillisecondCounterHiRes()<modeledUntil)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        eval("document.querySelector('[data-action=analyze]')?.click();'ok'");
        check(waitJs("/rays/.test(document.querySelector('.analysis-status')?.textContent || '')",15000), "live Energy Analyze renders real worker result");
        std::printf("Energy DOM: %s\n",eval("document.querySelector('.analysis-status')?.textContent+' / '+document.querySelector('#notification')?.textContent").toRawUTF8());
        check(noMock(), "real Energy result retains truthful labels");
        clickText("Energy");
        eval("document.querySelector('.theme-menu').open=true;'ok'");
        check(eval("String(!!document.querySelector('[data-action=classic-ui]'))") == "true" && noMock(), "Theme exposes Classic UI with no mock note");
        eval("document.querySelector('.theme-menu').open=false;'ok'");
        // Steady idle, same GetProcessTimes / child-process method and 2 s window as T4.
        const auto settle=juce::Time::getMillisecondCounterHiRes();
        while(juce::Time::getMillisecondCounterHiRes()-settle<2000)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        const auto drawsBefore=eval("document.querySelector('.viewport').dataset.drawCount");
        const auto steadyCpu0=childCpu(); const auto steadyStart=juce::Time::getMillisecondCounterHiRes();
        while(juce::Time::getMillisecondCounterHiRes()-steadyStart<2000)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        const double steadyCpuMs=(childCpu()-steadyCpu0)/10000.;
        check(eval("document.querySelector('.viewport').dataset.drawCount") == drawsBefore, "silent idle meter/status traffic causes zero viewport redraws");
        juce::File("out/T4b/after-metrics.json").replaceWithText(juce::JSON::toString(obj({{"firstFullRenderMs",openMs},{"idleWindowMs",2000},{"initialWebviewCpuMs",webCpuMs},{"initialOneCoreCpuPercent",webCpuMs/20.},{"steadyWebviewCpuMs",steadyCpuMs},{"steadyOneCoreCpuPercent",steadyCpuMs/20.},{"idleDrawsBefore",drawsBefore},{"idleDrawsAfter",eval("document.querySelector('.viewport').dataset.drawCount")}})));
        std::printf("T4b steady WebView CPU %.3f ms / 2 s = %.3f%% one core\n",steadyCpuMs,steadyCpuMs/20.);
        // E11: owned real WebView timer during three worker/gesture workloads.
        const auto heartbeatWindow=[&](const char* label,bool drag){
            double previous=juce::Time::getMillisecondCounterHiRes(),maximum=0;const double until=previous+1500;
            if(drag)bridge.parameterBatch(array({obj({{"type","begin"},{"id","absorption"}})}));
            int n=0;while(juce::Time::getMillisecondCounterHiRes()<until){
                const double now=juce::Time::getMillisecondCounterHiRes();maximum=std::max(maximum,now-previous);previous=now;
                if(drag && n++%8==0)bridge.parameterBatch(array({obj({{"type","change"},{"id","absorption"},{"value",1.+.01*(n%10)}})}));
                block.clear();p.processBlock(block,midi);bridge.tick(true,now);juce::MessageManager::getInstance()->runDispatchLoopUntil(4);
            }
            if(drag)bridge.parameterBatch(array({obj({{"type","end"},{"id","absorption"}})}));
            printf("E11 HEARTBEAT %s loop_max_gap_ms %.3f webview_timer_max_gap_ms %.3f bridge_tick_max_ms %.3f\n",label,maximum,editor->getStats().maxTimerGapMs,editor->getStats().maxBridgeTickMs);
        };
        heartbeatWindow("continuous-drag",true);
        verbp::MovementPath movement;movement.enabled=true;movement.points={{{1.,1.,1.5}},{{1.5,1.,1.5}}};p.setMovementPath(verbp::kListener,movement);
        const auto waitCurrent=[&]{const auto deadline=juce::Time::getMillisecondCounterHiRes()+5000;while(!p.isWebSceneCurrent()&&juce::Time::getMillisecondCounterHiRes()<deadline){block.clear();p.processBlock(block,midi);bridge.tick(true,juce::Time::getMillisecondCounterHiRes());juce::MessageManager::getInstance()->runDispatchLoopUntil(4);}};
        waitCurrent();check(p.startPathBake(verbp::kListener),"E11 heartbeat workload user bake accepted");heartbeatWindow("user-bake",false);p.cancelPathBake();
        waitCurrent();const auto analysisReply=command(R"({"op":"analysis.start","source":0,"resolution":24,"rays":48000,"heightM":1.2,"durationS":3,"components":7})");check(prop((*analysisReply.getArray())[0],"kind").toString()=="ack","E11 heartbeat workload energy analysis accepted");heartbeatWindow("energy-map",false);command(R"({"op":"analysis.cancel"})");
        eval("document.querySelector('[data-action=diagnostics]')?.click();'ok'");
        check(waitJs("document.querySelector('[data-ui-diagnostics]')?.textContent.includes('message-thread gap')"),"E11 Theme Diagnostics exposes native live watchdog");
        eval("document.querySelector('.theme-menu').open=false;'ok'");clickText("Settings");
        check(waitJs("document.querySelectorAll('.output-meter-row').length === " + juce::String(p.numOutputChannels())),"E12 native Settings renders output meter for every current channel");
        clickText("Energy");
        check(waitJs("!document.querySelector('.energy-scale')?.hidden"),"E12b live map layer exists before room load");
        {const auto r=verbp::builtinRooms()[5];p.loadSceneText(r.text,r.name,true);}
        check(waitJs("document.querySelector('.analysis-status')?.textContent==='No result · Analyze to start' && document.querySelector('.energy-scale')?.hidden",10000),"E12b live room LOAD clears map layer and returns analysis status to No result");

        eval("document.querySelector('[data-action=classic-ui]')?.click();'ok'");
        const auto switched=juce::Time::getMillisecondCounterHiRes();
        while(!editor->getNativeEditor() && juce::Time::getMillisecondCounterHiRes()-switched<3000)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        check(editor->getNativeEditor(), "Classic UI page action switches the live editor to native");
        editor.reset();
    }
    settings() = beforeSettings;
#endif
    // E21 B: a host channel-count change alone (no scene edit, no window reopen) must publish a fresh snapshot whose host block carries the new counts.
    {
        double nowMs = 5.0e6;
        captured.clear();
        const auto helloHost = bridge.handle(obj({{"schemaVersion",1},{"sessionId","test-live"},{"kind","hello"},{"payload",obj({{"supportedVersions",array({1})},{"ready",true}})}}));
        for (const auto& m : *helloHost.getArray()) record(m);
        for (int i = 0; i < 3; ++i) bridge.tick(true, nowMs += 40);
        const auto publishedHost = [&](int wantIn, int wantOut, bool keep = false) {
            if (!keep) captured.clear();
            bool seen = false;
            for (int i = 0; i < 60 && !seen; ++i) {
                juce::MessageManager::getInstance()->runDispatchLoopUntil(10); bridge.tick(true, nowMs += 40);
                for (const auto& m : captured) {
                    if (prop(m, "kind").toString() != "snapshot") continue;
                    const auto h = prop(prop(m, "payload"), "host");
                    if ((int)prop(h, "inputChannels") == wantIn && (int)prop(h, "outputChannels") == wantOut) seen = true;
                }
            }
            return seen;
        };
        const int in0 = p.hostInputChannels(), out0 = p.hostOutputChannels();
        check(publishedHost(in0, out0, true), "E21 B: initial snapshot carries the current host channel counts");
        for (const int outCh : {4, 9, 2}) {
            p.setPlayConfigDetails(2, outCh, 48000, 256); p.prepareToPlay(48000, 256);
            check(ready(p), "E21 B: engine ready after the host output count changes to " + std::to_string(outCh));
            check(publishedHost(2, outCh), "E21 B: UI snapshot carries host.outputChannels=" + std::to_string(outCh) + " without user action");
        }
        p.setPlayConfigDetails(6, 2, 48000, 256); p.prepareToPlay(48000, 256);
        check(ready(p), "E21 B: engine ready after the host input count changes to 6");
        check(publishedHost(6, 2), "E21 B: UI snapshot carries host.inputChannels=6 without user action");
        p.setPlayConfigDetails(2, 2, 48000, 256); p.prepareToPlay(48000, 256);
        check(ready(p), "E21 B: engine ready after the host input count returns to 2");
        check(publishedHost(2, 2), "E21 B: UI snapshot carries host.inputChannels=2 again");
    }
    {   // U1: drawing a wall chain: one click = one undo step; the derived ceiling follows; redo restores the same scene
        const auto wallsOf=[&]{return p.getDisplayScene().walls;};
        const auto sig=[&]{const auto sc=p.getDisplayScene();std::string t;for(const auto& w:sc.walls)t+=std::to_string(w.ax)+","+std::to_string(w.ay)+","+std::to_string(w.bx)+","+std::to_string(w.by)+";";
            for(const auto& z:verb::CeilingPlan(sc).polygons){t+="C";for(auto v:z.poly)t+=std::to_string(v.first)+","+std::to_string(v.second)+";";}return t;};
        command(R"({"op":"room.clear"})");
        const size_t w0=wallsOf().size();std::vector<std::string> states{sig()};
        const double P[4][2]={{1,1},{7,1},{7,5},{1,5}};
        for(int i=0;i<4;++i){const int j=(i+1)%4;
            command("{\"op\":\"surface.create\",\"kind\":\"wall\",\"a\":["+std::to_string(P[i][0])+","+std::to_string(P[i][1])+"],\"b\":["+std::to_string(P[j][0])+","+std::to_string(P[j][1])+"],\"material\":0}");
            states.push_back(sig());check(wallsOf().size()==w0+(size_t)i+1,"U1 wall click "+std::to_string(i+1)+" adds exactly one wall");}
        const auto full=sig();
        for(int i=3;i>=0;--i){command(R"({"op":"history.undo"})");check(sig()==states[(size_t)i]&&wallsOf().size()==w0+(size_t)i,"U1 undo "+std::to_string(4-i)+" returns to the state before that click (walls and derived ceiling)");}
        for(int i=1;i<=4;++i){command(R"({"op":"history.redo"})");check(sig()==states[(size_t)i],"U1 redo "+std::to_string(i)+" restores the state after that click");}
        check(sig()==full,"U1 after 4 undo + 4 redo the scene equals the drawn one");
    }
    {   // C1: source colours capability, snapshot omission when empty, normalization, live update, invalid rejection, undo/redo
        const auto caps = prop(prop(bridge.snapshot(), "payload"), "capabilities");
        const auto cs = prop(prop(caps, "features"), "colours.source");
        check((bool)prop(cs, "available") && prop(cs, "status").toString() == "available", "C1 colours.source capability is available");
        const auto doc0 = prop(prop(bridge.snapshot(), "payload"), "document");
        const auto srcs0 = prop(doc0, "sources");
        check(srcs0.isArray() && srcs0.size() >= 1 && !srcs0[0].getDynamicObject()->hasProperty("color"),
              "C1 default snapshot omits color field when automatic");
        const uint64_t load0 = p.getLoadSerial();
        const auto setCol = command(R"({"op":"source.setConfig","index":0,"color":"#D94A38"})");
        check(prop((*setCol.getArray())[0], "kind").toString() == "ack", "C1 bridge accepts source.setConfig color");
        check(p.getLoadSerial() == load0, "C1 bridge color change keeps loadSerial unchanged (no rebuild)");
        const auto srcs1 = prop(prop(prop(bridge.snapshot(), "payload"), "document"), "sources");
        check(srcs1[0].getProperty("color", {}).toString() == "#d94a38", "C1 snapshot carries normalized lowercase #rrggbb");
        const auto badMsg = obj({{"schemaVersion", 1}, {"sessionId", "test-live"}, {"kind", "command"}, {"requestId", "test-bad-color"},
                                 {"baseRevision", decimal(p.getRevision())}, {"payload", obj({{"op", "source.setConfig"}, {"index", 0}, {"color", "red"}})}});
        const auto bad = bridge.handle(badMsg);
        check(bad.isArray() && !bad.getArray()->isEmpty() && prop((*bad.getArray())[0], "kind").toString() == "error", "C1 bridge rejects invalid color");
        command(R"({"op":"history.undo"})");
        const auto srcsUndo = prop(prop(prop(bridge.snapshot(), "payload"), "document"), "sources");
        check(!srcsUndo[0].getDynamicObject()->hasProperty("color"), "C1 bridge undo removes color and omits key again");
        command(R"({"op":"history.redo"})");
        const auto srcsRedo = prop(prop(prop(bridge.snapshot(), "payload"), "document"), "sources");
        check(srcsRedo[0].getProperty("color", {}).toString() == "#d94a38", "C1 bridge redo restores #d94a38");
        command(R"({"op":"source.setConfig","index":0,"color":null})");
        const auto srcsCleared = prop(prop(prop(bridge.snapshot(), "payload"), "document"), "sources");
        check(!srcsCleared[0].getDynamicObject()->hasProperty("color"), "C1 bridge color:null restores automatic");
    }
    std::printf("FINAL web_bridge: %d pass, %d fail\n", passes, fails);
    return fails ? 1 : 0;
}
