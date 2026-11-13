// Focused, read-only T7 regression. Run with VERB_T7_ONLY=1 (no audio/stress suites).
static int runT7RaysTest(VerbProcessor& p) {
    juce::Array<juce::var> events;
    EditorBridge bridge(p, [&](const juce::String& channel, const juce::var& value) {
        if (channel == "verb.message" && prop(prop(value,"payload"),"event").toString() == "diagnostics.paths") events.add(prop(value,"payload"));
    });
    bridge.handle(obj({{"schemaVersion",1},{"sessionId","t7"},{"kind","hello"},{"payload",obj({{"supportedVersions",array({1})},{"ready",true}})}}));
    const auto serial=p.getSceneSerial(), revision=p.getRevision(), geometry=p.getWebGeometrySerial();
    const auto set=[&](const char* id,double v) { auto* param=p.apvts.getParameter(id); param->setValueNotifyingHost(param->convertTo0to1((float)v)); };
    double worst=0; const auto started=juce::Time::getMillisecondCounterHiRes();
    for(int i=0;i<25;++i) {
        set("lx",.7+i*.01); set("s1x",1.1+i*.01);
        bridge.tick(true,10000.+i*34.);
    }
    check(events.size()>=10 && events.size()<=14,"T7 continuous pose changes produce 10–20 Hz rays before release");
    bool exact=true, timing=true;
    for(const auto& e:events) {
        const auto ms=prop(e,"queryTimeMs"); timing &= ms.isDouble() && (double)ms>=0.; worst=std::max(worst,(double)ms);
        std::string why; if(!schemas.validate(schemas.files["event.schema.json"],e,"event.schema.json",why,"$")) { exact=false;std::printf("T7 schema: %s\n",why.c_str()); }
    }
    const auto last=events.getLast(); bool direct=false;
    juce::File("out/T7/focused-paths.json").replaceWithText(juce::JSON::toString(last));
    const auto lastPaths=prop(last,"paths");
    for(const auto& path:*lastPaths.getArray()) if((int)prop(path,"order")==0 && !(bool)prop(path,"diffracted")) {
        direct=true; const auto points=prop(path,"pointsM");
        exact &= std::abs((double)points[points.size()-1][0]-p.apvts.getRawParameterValue("lx")->load())<1e-5 && std::abs((double)points[0][0]-p.getEnergyScene(0,true).sources[0].pos.x)<1e-5;
    }
    check(exact && direct,"T7 query uses latest listener AND source APVTS preview with valid schema");
    check(timing,"T7 every ray event reports query duration");
    const int count=events.size();bridge.tick(true,10900.);bridge.tick(true,11900.);
    check(events.size()==count,"T7 idle has no pending ray work and emits no duplicate paths");
    set("lx",1.123);bridge.tick(true,12000.);bridge.tick(true,12080.);
    const auto finalPaths=prop(events.getLast(),"paths");
    bool final=false; for(const auto& path:*finalPaths.getArray()) if((int)prop(path,"order")==0 && !(bool)prop(path,"diffracted")) {const auto points=prop(path,"pointsM");final=std::abs((double)points[points.size()-1][0]-p.apvts.getRawParameterValue("lx")->load())<1e-5;}
    check(final,"T7 exact final pose replaces last preview within the throttle interval");
    check(serial==p.getSceneSerial() && revision==p.getRevision() && geometry==p.getWebGeometrySerial(),"T7 rays do not mutate/rebuild scene, revision or modeled geometry");
    p.setSourcePattern(0,.5);
    const auto request=obj({{"sourceIndex",0},{"maxPaths",128},{"maxOrder",1}});
    set("s1az",0.);const auto az0=bridge.reflectionPaths(request);
    set("s1az",90.);const auto az90=bridge.reflectionPaths(request);
    check(juce::JSON::toString(prop(az0,"paths"))!=juce::JSON::toString(prop(az90,"paths")),"T7 source rotation changes directional ray energy");
    const auto beforeYaw=bridge.reflectionPaths(request);set("laz",90.);const auto afterYaw=bridge.reflectionPaths(request);
    check(juce::JSON::toString(prop(beforeYaw,"paths"))==juce::JSON::toString(prop(afterYaw,"paths")),"T7 listener yaw does not affect listener-centre ray geometry/energy");
    std::printf("T7 worst query %.4f ms; focused test %.2f ms; %d pass, %d fail\n",worst,juce::Time::getMillisecondCounterHiRes()-started,passes,fails);
    return fails?1:0;
}
