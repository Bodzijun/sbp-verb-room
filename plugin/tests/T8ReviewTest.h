// Short focused T8 regression. No audio/stress suite.
static int runT8ReviewTest(VerbProcessor& p) {
    juce::Array<juce::var> events;
    EditorBridge bridge(p,[&](const juce::String& name,const juce::var& m){if(name=="verb.message")events.add(prop(m,"payload"));});
    bridge.handle(obj({{"schemaVersion",1},{"sessionId","t8"},{"kind","hello"},{"payload",obj({{"supportedVersions",array({1})},{"ready",true}})}}));
    bridge.handle(obj({{"schemaVersion",1},{"sessionId","t8"},{"kind","command"},{"requestId","subscribe"},{"baseRevision",decimal(p.getRevision())},{"payload",obj({{"op","telemetry.subscribe"},{"topics",array({"status"})}})}}));
    check(p.editScene([](verb::Scene& s){s.clutter.count=18;s.clutter.seed=103;s.clutter.minDist=1.1;}),"T8 clutter edit accepted");
    const auto start=juce::Time::getMillisecondCounterHiRes();const auto early=bridge.clutterObjects();
    const double ms=juce::Time::getMillisecondCounterHiRes()-start;
    check((bool)prop(early,"preview")&&!(bool)prop(early,"stale"),"T8 deterministic placement published before acoustic rebuild");
    const auto until=start+10000;
    while(!p.isWebSceneCurrent()&&juce::Time::getMillisecondCounterHiRes()<until)juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    const auto applied=bridge.clutterObjects();
    check(p.isWebSceneCurrent()&&!(bool)prop(applied,"preview")&&juce::JSON::toString(prop(early,"objects"))==juce::JSON::toString(prop(applied,"objects")),"T8 early placement equals rebuilt engine exactly (seed, minDist, geometry)");
    check(prop(early,"objects").size()>0&&prop(early,"objects").size()<=18&&ms<100,"T8 populated early placement within 100 ms and wall budget");
    std::string why;
    check(schemas.validate(schemas.files["event.schema.json"],early,"event.schema.json",why,"$"),"T8 additive preview event validates");
    const auto set=[&](const char* id,double v){auto* q=p.apvts.getParameter(id);q->setValueNotifyingHost(q->convertTo0to1((float)v));};
    set("mode",1);set("decayLow",1.7);
    check(!p.isWebTailCurrent(),"T8 requested decay mismatch immediately invalidates applied tail");
    bridge.tick(true,10000);bridge.tick(true,10100);bridge.tick(true,10160);
    bool delayed=false,shown=false,valid=true;
    for(const auto& e:events)if(prop(e,"event").toString()=="status.load"){
        delayed|=!(bool)prop(e,"rebuilding");shown|=(bool)prop(e,"rebuilding");
        valid&=schemas.validate(schemas.files["event.schema.json"],e,"event.schema.json",why,"$");
    }
    check(delayed&&shown&&valid,"T8 real rebuild indication delayed by 150 ms with valid status events");
    std::printf("T8 placement %.3f ms, %d objects; %d pass, %d fail\n",ms,prop(early,"objects").size(),passes,fails);
    return fails?1:0;
}
