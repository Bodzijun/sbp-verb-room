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
#endif
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI init;
    const auto started = juce::Time::getMillisecondCounterHiRes();
    schemas.load(juce::File("web-ui/schema"));
    VerbProcessor p; p.setTailSettings({8000, false}); p.prepareToPlay(48000, 256);
    p.loadSceneText(boxText(5,4,3,0,true), "Bridge test box", true); check(ready(p), "tiny test room loads");
    if (juce::SystemStats::getEnvironmentVariable("VERB_T7_ONLY", {}).isNotEmpty()) return runT7RaysTest(p);
    if (juce::SystemStats::getEnvironmentVariable("VERB_T8_ONLY", {}).isNotEmpty()) return runT8ReviewTest(p);
    if (juce::SystemStats::getEnvironmentVariable("VERB_T9_ONLY", {}).isNotEmpty()) return runT9ReviewTest(p);
#if VERB_WEB_UI_ENABLED
    if (juce::SystemStats::getEnvironmentVariable("VERB_T6_LIVE", {}).isNotEmpty()) return runLivePointerHarness(p);
#endif
    EditorBridge bridge(p, [](const juce::String& channel, const juce::var& value) { if (channel == "verb.message") record(value); });
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
    check((int)prop(prop(prop(snap,"parameters"),"descriptors"),"count") == 250, "snapshot contains all 250 live APVTS descriptors (legacy 243 + E1 6)");
    juce::File("out/T4/live-snapshot.json").replaceWithText(juce::JSON::toString((*hello.getArray())[0]));
    auto* mix = p.apvts.getParameter("mix"); HostListener listener; mix->addListener(&listener);
    const auto gesture = bridge.parameterBatch(array({obj({{"type","begin"},{"id","mix"}}),obj({{"type","change"},{"id","mix"},{"value",37.5}}),obj({{"type","end"},{"id","mix"}})}));
    check((bool)prop(gesture,"ok") && listener.begins == 1 && listener.changes == 1 && listener.ends == 1, "gesture reaches host begin/change/end exactly once");
    mix->setValueNotifyingHost(mix->convertTo0to1(20.f)); const int changes = listener.changes; bridge.tick(true,1000); bridge.tick(true,1035);
    check(listener.changes == changes && (double)prop(prop(prop(bridge.snapshot(),"payload"),"parameters"),"values").getProperty("mix",{}) == 20, "host automation reflected without echo");
    mix->removeListener(&listener);
    const auto bad = bridge.parameterBatch(array({obj({{"type","begin"},{"id","early"}}),obj({{"type","change"},{"id","early"},{"value",-6}}),obj({{"type","end"},{"id","early"}})}));
    check(prop(bad,"error").isString(), "Physical mode rejects Creative-only change atomically");
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
    command(R"({"op":"analysis.cancel"})"); command(R"({"op":"rt60.cancel"})");
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
        check(eval("String(document.querySelector('.rt-large')?.textContent !== '—' && document.querySelector('.rt-caption')?.textContent.includes('Engine'))") == "true", "real RT60 arrives in status card");
        mix->setValueNotifyingHost(mix->convertTo0to1(37.5f));
        // T9 D1: the page now formats every value with one table that always carries the unit ("37.5 %"); the value shown is still the
        // plugin's own value after host automation, so the check compares against the unit-formatted plugin value.
        const auto mixText = juce::String(juce::String(mix->convertFrom0to1(mix->getValue()), 1).getDoubleValue()) + " %";
        check(waitJs("document.querySelector('[data-parameter=mix] .readout')?.value === " + juce::JSON::toString(mixText)), "live readout uses authoritative plugin text after automation");
        check(eval("String(document.querySelector('[data-parameter=mix] [role=slider]')?.getAttribute('aria-valuetext') === " + juce::JSON::toString(mixText) + ")") == "true", "accessible slider text is authoritative too");
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
            eval("(()=>{const c=document.querySelector('.viewport canvas'),r=c.getBoundingClientRect(),p=window.__t4Project(["+juce::String(wallBefore.ax,6)+","+juce::String(wallBefore.ay,6)+",0],"+moveCamera+",r.width,r.height),capture=c.setPointerCapture;c.setPointerCapture=()=>{};try{for(const [type,dx] of [['pointerdown',0],['pointermove',9],['pointerup',9]])c.dispatchEvent(new PointerEvent(type,{bubbles:true,pointerId:1,button:0,clientX:r.x+p[0]+dx,clientY:r.y+p[1]}));}finally{c.setPointerCapture=capture;}})();'ok'");
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
        eval("document.querySelector('[data-action=classic-ui]')?.click();'ok'");
        const auto switched=juce::Time::getMillisecondCounterHiRes();
        while(!editor->getNativeEditor() && juce::Time::getMillisecondCounterHiRes()-switched<3000)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        check(editor->getNativeEditor(), "Classic UI page action switches the live editor to native");
        editor.reset();
    }
    settings() = beforeSettings;
#endif
    std::printf("FINAL web_bridge: %d pass, %d fail\n", passes, fails);
    return fails ? 1 : 0;
}
