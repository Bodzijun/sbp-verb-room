// Focused T9 checks only; no audio/stress suite. All mutations use an isolated processor.
static int runT9ReviewTest(VerbProcessor& p) {
    const auto set=[&](const char* id,double value){auto* q=p.apvts.getParameter(id);q->setValueNotifyingHost(q->convertTo0to1((float)value));};
    const auto finite=[](const juce::var& value,auto&& self)->bool{
        if(value.isDouble())return std::isfinite((double)value);
        if(auto* a=value.getArray())for(const auto& v:*a)if(!self(v,self))return false;
        if(auto* o=value.getDynamicObject())for(const auto& v:o->getProperties())if(!self(v.value,self))return false;
        return true;
    };
    const auto bad=obj({{"x",std::numeric_limits<double>::infinity()},{"a",array({std::nan(""),-std::numeric_limits<double>::infinity()})}});
    const auto safe=finiteJson(bad.clone());
    check(finite(safe,finite)&&juce::JSON::parse(juce::JSON::toString(safe)).isObject(),"T9 serialization boundary removes every nested NaN/Inf");
    juce::Array<juce::var> events;
    EditorBridge bridge(p,[&](const juce::String& name,const juce::var& value){if(name=="verb.message")events.add(prop(value,"payload"));});
    bridge.handle(obj({{"schemaVersion",1},{"sessionId","t9"},{"kind","hello"},{"payload",obj({{"supportedVersions",array({1})},{"ready",true}})}}));
    bridge.handle(obj({{"schemaVersion",1},{"sessionId","t9"},{"kind","command"},{"requestId","status"},{"baseRevision",decimal(p.getRevision())},{"payload",obj({{"op","telemetry.subscribe"},{"topics",array({"status"})}})}}));
    set("mode",1);set("decayLow",1.201);
    bridge.tick(true,10000);bridge.tick(true,10160);bridge.tick(true,10800);
    bool busy=false,cleared=false;
    for(const auto& e:events)if(prop(e,"event").toString()=="status.load"){busy|=(bool)prop(e,"rebuilding");cleared=!(bool)prop(e,"rebuilding");}
    check(busy&&cleared,"T9 stopped-host requested decay mismatch clears activity by 750 ms without consuming a block");
    const auto request=obj({{"sourceIndex",0},{"maxPaths",256},{"maxOrder",1}});
    double worst=0.;bool valid=true,origins=true;
    for(const auto format:{1,2,5}){
        auto cfg=p.getSources();cfg[0].format=format;cfg[0].sum=false;cfg[0].spread=.3;p.setSources(cfg);check(ready(p),"T9 multichannel format loads "+std::to_string(format));
        const auto expected=p.getEnergyScene(0,true);const auto event=bridge.reflectionPaths(request);worst=std::max(worst,(double)prop(event,"queryTimeMs"));
        std::string why;valid&=schemas.validate(schemas.files["event.schema.json"],event,"event.schema.json",why,"$")&&finite(event,finite);
        std::set<int> seen;const auto paths=prop(event,"paths");for(const auto& path:*paths.getArray()){
            const int pi=(int)prop(path,"pointIndex");const auto points=prop(path,"pointsM");
            if(pi<0||pi>=(int)expected.sources.size()){origins=false;continue;}
            const auto want=expected.sources[(size_t)pi].pos;
            origins&=std::abs((double)points[0][0]-want.x)<1e-8&&std::abs((double)points[0][1]-want.y)<1e-8&&std::abs((double)points[0][2]-want.z)<1e-8;
            if(!(bool)prop(path,"diffracted")&&(int)prop(path,"order")==0)seen.insert(pi);
        }
        origins&=seen.size()==expected.sources.size();
    }
    check(valid&&origins,"T9 Stereo/LCR/5.1 rays cover every rendered channel point, with exact origins and finite valid schema (LFE excluded)");
    auto cfg=p.getSources();cfg[0].sum=true;p.setSources(cfg);check(ready(p),"T9 Sum loads");
    const auto summed=bridge.reflectionPaths(request);check((int)prop(summed,"pointCount")==1,"T9 Sum has exactly one source-centre spatial point");
    cfg[0].sum=false;cfg[0].format=2;p.setSources(cfg);check(ready(p),"T9 coincidence LCR loads");
    const auto sc=p.getEnergyScene(0,true);
    for(const auto& pos:{sc.sources[0].pos,sc.sources[1].pos,verb::Vec3{0,0,0},verb::Vec3{-2,-2,1.5},verb::Vec3{5,4,1.5}}){
        set("lx",pos.x);set("ly",pos.y);set("lz",pos.z);const auto event=bridge.reflectionPaths(request);std::string why;
        valid&=finite(event,finite)&&schemas.validate(schemas.files["event.schema.json"],event,"event.schema.json",why,"$");worst=std::max(worst,(double)prop(event,"queryTimeMs"));
    }
    check(valid,"T9 coincident source/channel, floor corner, wall corner and outside receiver never serialize NaN/Inf");
    const auto capped=bridge.reflectionPaths(obj({{"sourceIndex",0},{"maxPaths",2},{"maxOrder",1}}));
    check(prop(capped,"paths").size()<=2&&(bool)prop(capped,"truncated"),"T9 global ray cap is enforced and reported");
    const verb::Geo geo(sc);verb::DiffPath path;int calls=0;
    check(!boundedDiffractedPath(geo,sc.sources[0].pos,{10,10,1.5},343,path,false,[&]{++calls;return true;})&&calls==1,"T9 diffraction search aborts immediately at its deadline");
    {
        // E2: a rebuild ends native gestures first; the page's late change/end for that gesture is ignored, not an error toast.
        verbweb::WebParameterBindings bindings(p);
        const auto act=[&](const char* type,double value=0.){return bindings.apply(array({obj({{"type",type},{"id","mix"},{"value",value}})}));};
        check((bool)prop(act("begin"),"ok"),"T9 gesture opens");bindings.endAll();
        check((bool)prop(act("change",.4),"ok")&&(bool)prop(act("end"),"ok"),"T9 late change/end after a native rebuild reset are ignored without error");
        check(prop(act("end"),"error").toString()=="Gesture is not open"||(bool)prop(act("end"),"ok"),"T9 repeated end stays well-defined");
        bindings.endAll();bindings.apply(array({obj({{"type","begin"},{"id","mix"}}),obj({{"type","change"},{"id","mix"},{"value",.6}}),obj({{"type","end"},{"id","mix"}})}));
        check(prop(act("change",.7),"error").toString().isNotEmpty(),"T9 a change with no open gesture and no rebuild reset is still rejected");
    }
    std::printf("T9 worst focused ray query %.4f ms; %d pass, %d fail\n",worst,passes,fails);return fails?1:0;
}
