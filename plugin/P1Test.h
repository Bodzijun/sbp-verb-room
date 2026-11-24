#pragma once
#include "EditorSession.h"
#include "P1dTest.h"
struct P1Access {
    static juce::MemoryBlock legacyState(VerbProcessor& p) {
        juce::MemoryBlock data;p.getStateInformation(data);auto xml=p.getXmlFromBinary(data.getData(),(int)data.getSize());auto st=juce::ValueTree::fromXml(*xml);
        st.removeChild(st.getChildWithName("MovementPaths"),nullptr);
        for(int o=0;o<=verbp::kMaxSources;++o)st.removeChild(st.getChildWithProperty("id",VerbProcessor::pathParamId(o)),nullptr);
        data.reset();auto legacy=st.createXml();p.copyXmlToBinary(*legacy,data);return data;
    }
    static double rawX(VerbProcessor& p,int o) {return p.prevRaw[o][0];}
    static double velocity(VerbProcessor& p,int o) {return p.hVel[o][0];}
};
static int testP1() {
    using namespace verbp;
    const int before=fails;
    testP1dGeometry();
    MovementPath raw;raw.points={PathPoint{0,0,1.5},PathPoint{1,0,1.5},PathPoint{15,0,1.5}};
    CompiledPath table;table.build(raw);
    check(std::abs(table.length()-15)<1e-10,"P1 unequal straight segments length");
    for(bool smooth:{false,true}) {
        raw.smooth=smooth;table.build(raw);double worst=0;
        for(int i=1;i<=1000;++i)worst=std::max(worst,std::abs(pathDistance(table.evaluate((i-1)/1000.0),table.evaluate(i/1000.0))/(table.length()/1000)-1));
        std::printf("P1 speed error %.9f\n",worst);check(worst<.01,"P1 straight/smooth uniform speed within 1 percent");
    }
    raw.points={PathPoint{1,1,1},PathPoint{2,5,2},PathPoint{9,6,1}};raw.smooth=true;table.build(raw);
    double worstCurve=0;for(int i=1;i<=1000;++i)worstCurve=std::max(worstCurve,std::abs(pathDistance(table.evaluate((i-1)/1000.0),table.evaluate(i/1000.0))/(table.length()/1000)-1));
    std::printf("P1 curved speed error %.9f\n",worstCurve);check(worstCurve<.01,"P1 curved smooth uniform speed within 1 percent");
    const auto sharedVectors=juce::JSON::parse(juce::File::getCurrentWorkingDirectory().getChildFile("web-ui/tests/data/p1-vectors.json").loadFileAsString());
    if(auto vectors=sharedVectors.getArray()) {
        for(auto v:*vectors){MovementPath path;path.smooth=(bool)v.getProperty("smooth",true);path.closed=(bool)v.getProperty("closed",false);path.laps=(int)v.getProperty("laps",1);for(auto pt:*v.getProperty("points",{}).getArray()){auto* q=pt.getArray();path.points.push_back({(double)(*q)[0],(double)(*q)[1],(double)(*q)[2]});}CompiledPath t;t.build(path);auto q=t.evaluate((double)v.getProperty("u",0));auto* expected=v.getProperty("expected",{}).getArray();check(pathDistance(q,{(double)(*expected)[0],(double)(*expected)[1],(double)(*expected)[2]})<1e-9,"P1 C++/JS shared curve vector");}
    } else check(false,"P1 shared vectors present");
    raw.closed=true;raw.orient=true;raw.smooth=false;table.build(raw);double yaw0=0,yaw1=0;auto seam0=table.evaluate(0,&yaw0),seam1=table.evaluate(1,&yaw1);
    check(pathDistance(seam0,seam1)<1e-10 && yaw0==yaw1,"P1 closed orient endpoint has identical pose across exact 1 to 0");
    raw.closed=false;raw.orient=false;
    VerbProcessor p;p.setPlayConfigDetails(2,2,48000,256);p.prepareToPlay(48000,256);check(waitReady(p,90),"P1 processor ready without web editor");
    check(p.getParameters().size()==283+verbp::kMaxModHostOuts && p.apvts.getParameter("makeup")->getParameterIndex()==249 && p.apvts.getParameter("s1path")->getParameterIndex()==250 && p.apvts.getParameter("lpath")->getParameterIndex()==282 && p.modOutParameter(0) && p.modOutParameter(0)->getParameterIndex()==283 && p.modOutParameter(15) && p.modOutParameter(15)->getParameterIndex()==298,"First 283 parameter indices unchanged + 16 appended Mod Out");
    raw.smooth=false;raw.points={PathPoint{1,3,1.5},PathPoint{15,3,1.5}};check(p.setMovementPath(0,raw),"P1 set path");
    juce::MemoryBlock saved;p.getStateInformation(saved);
    setP(p,"s1path",.75);p.setMovementPath(0,{});p.setStateInformation(saved.getData(),(int)saved.getSize());check(waitReady(p,90),"P1 state reload ready");
    check(p.getMovementPath(0)==raw && p.apvts.getRawParameterValue("s1path")->load()==0,"P1 state path round trip");
    auto preset=p.getRoomPresetText();juce::String error;check(p.loadRoomPresetText(preset,error)&&waitReady(p,90)&&p.getMovementPath(0)==raw,"P1 verbroom round trip");
    auto& session=p.getEditorSession();auto result=session.run(juce::JSON::parse(R"({"op":"path.movePoint","object":0,"index":1,"point":[14,3,1.5]})"));
    check(result.ok && p.getMovementPath(0).points[1][0]==14,"P1 move command");
    check(session.undo().ok && p.getMovementPath(0)==raw,"P1 scoped path undo");
    juce::AudioBuffer<float> block(2,256);juce::MidiBuffer midi;
    block.clear();p.processBlock(block,midi);
    setP(p,"s1path",1);block.clear();stage2Process(p,block,midi);setP(p,"s1path",0);block.clear();stage2Process(p,block,midi);
    check(std::abs(P1Access::rawX(p,0)-1)<1e-6 && P1Access::velocity(p,0)==0,"P1 open restart resets E8 tracker");
    raw.closed=true;p.setMovementPath(0,raw);setP(p,"s1path",1);block.clear();p.processBlock(block,midi);const double atEnd=P1Access::rawX(p,0);
    setP(p,"s1path",0);block.clear();p.processBlock(block,midi);check(P1Access::rawX(p,0)==atEnd,"P1 closed wrap identical raw position");
    const auto allocations=stage2AudioAllocations;
    for(int i=0;i<100;++i){setP(p,"s1path",i/100.0);block.clear();stage2Process(p,block,midi);}
    check(stage2AudioAllocations==allocations,"P1 zero audio allocations during movement");
    // Compare the same processor from the same reset state, avoiding worker scheduling noise (Direct only).
    auto render=[&](bool path) {
        p.setStateInformation(saved.getData(),(int)saved.getSize());waitReady(p,90);if(!path)p.setMovementPath(0,{});setP(p,"soloDirect",1);
        std::vector<float> audio;audio.reserve(192000*2);
        for(int b=0;b<750;++b){const float u=(float)b/749; p.apvts.getRawParameterValue("s1path")->store(u);p.apvts.getRawParameterValue("s1x")->store(1+14.0f*u);p.apvts.getRawParameterValue("s1y")->store(3);
            for(int i=0;i<256;++i)block.setSample(0,i,(float)(.05*std::sin((b*256+i)*.071)));block.clear(1,0,256);stage2Process(p,block,midi);
            for(int i=0;i<256;++i)for(int c=0;c<2;++c)audio.push_back(block.getSample(c,i));}
        return audio;
    };
    const auto a=render(true),b=render(false);double signal=0,diff=0;for(size_t i=0;i<a.size();++i){signal+=a[i]*a[i];const double d=a[i]-b[i];diff+=d*d;}
    const double db=10*std::log10(std::max(1e-30,diff)/std::max(1e-30,signal));std::printf("P1 14m/4s path vs XYZ %.3f dB\n",db);check(db<=-60,"P1 Doppler equivalence <= -60dB");
    // A legacy state must clear both optional metadata and the new parameter.
    p.setMovementPath(0,{});setP(p,"s1path",0);auto legacy=P1Access::legacyState(p);p.setMovementPath(0,raw);setP(p,"s1path",.8);p.setStateInformation(legacy.getData(),(int)legacy.getSize());check(waitReady(p,90)&&p.getMovementPath(0).points.empty()&&p.apvts.getRawParameterValue("s1path")->load()==0,"P1 legacy metadata and PARAM absence clears paths and position");

    // P1b: multi-lap closed path evaluation, E8 zero-jump across laps, state/preset laps round-trip, indexed insert, energy scene evaluation.
    MovementPath loop3;loop3.closed=true;loop3.smooth=true;loop3.orient=true;loop3.laps=3;
    loop3.points={{2,2,1.5},{6,2,1.5},{6,5,1.5},{2,5,1.5}};
    table.build(loop3);
    double y0=0,y1=0,y2=0,y3=0;
    const auto p0=table.evaluate(0.0,&y0),p1=table.evaluate(1.0/3.0,&y1),p2=table.evaluate(2.0/3.0,&y2),p3=table.evaluate(1.0,&y3);
    check(pathDistance(p0,p1)<1e-9 && pathDistance(p0,p2)<1e-9 && pathDistance(p0,p3)<1e-9,"P1b closed laps=3 identical start/wrap/end positions");
    check(std::abs(y0-y1)<1e-9 && std::abs(y0-y2)<1e-9 && std::abs(y0-y3)<1e-9,"P1b closed laps=3 identical start/wrap/end yaw");
    check(pathDistance(table.evaluate(0.5/3.0),p0)>1.0,"P1b closed laps=3 moves mid-lap");
    check(p.setMovementPath(0,loop3),"P1b set laps=3 path");
    juce::MemoryBlock savedLaps;p.getStateInformation(savedLaps);
    p.setMovementPath(0,{});p.setStateInformation(savedLaps.getData(),(int)savedLaps.getSize());
    check(waitReady(p,90) && p.getMovementPath(0)==loop3,"P1b state round-trip preserves laps=3");
    auto presetLaps=p.getRoomPresetText();juce::String lapsErr;
    p.setMovementPath(0,{});
    check(p.loadRoomPresetText(presetLaps,lapsErr)&&waitReady(p,90)&&p.getMovementPath(0)==loop3,"P1b verbroom round-trip preserves laps=3");
    // Energy scene uses enabled movement path position.
    setP(p,"s1path",0.5/3.0);
    const auto es=p.getEnergyScene(0);
    const auto expectedMid=table.evaluate(0.5/3.0);
    check(es.sources.size()==1 && std::hypot(es.sources[0].pos.x-expectedMid[0],es.sources[0].pos.y-expectedMid[1])<1e-4,"P1b getEnergyScene reflects enabled source movement path");
    // Indexed insert & laps flag command + undo
    auto ins=session.run(juce::JSON::parse(R"({"op":"path.addPoint","object":0,"index":1,"point":[4,1.5,2]})"));
    check(ins.ok && p.getMovementPath(0).points.size()==5 && std::abs(p.getMovementPath(0).points[1][0]-4)<1e-9,"P1b indexed path.addPoint inserts at index 1");
    auto lapsCmd=session.run(juce::JSON::parse(R"({"op":"path.setFlags","object":0,"laps":5})"));
    check(lapsCmd.ok && p.getMovementPath(0).laps==5,"P1b path.setFlags updates laps");
    check(session.undo().ok && p.getMovementPath(0).laps==3,"P1b undo restores laps");
    check(session.undo().ok && p.getMovementPath(0).points.size()==4,"P1b undo removes inserted point");
    // room.scale (includeObjects:true) scales movement paths & undo restores them
    {
        auto add2=session.run(juce::JSON::parse(R"({"op":"source.add"})"));
        check(add2.ok,"P1b source.add for s2path test");
        check(waitReady(p,90),"P1b asynchronous source.add engine is ready before rendering second source");
        MovementPath sp2;sp2.enabled=true;sp2.points={{2,2,1.5},{10,2,1.5}};
        MovementPath lp;lp.enabled=true;lp.points={{3,3,1.5},{7,3,1.5}};
        check(p.setMovementPath(1,sp2) && p.setMovementPath(verbp::kListener,lp),"P1b set s2path and lpath");
        auto scCmd=session.run(juce::JSON::parse(R"({"op":"room.scale","factor":1.5,"includeObjects":true})"));
        check(scCmd.ok,"P1b room.scale with includeObjects succeeds");
        check(std::abs(p.getMovementPath(verbp::kListener).points[0][0]-lp.points[0][0])>1e-3 || std::abs(p.getMovementPath(verbp::kListener).points[1][0]-lp.points[1][0])>1e-3,"P1b room.scale scales listener movement path");
        check(std::abs(p.getMovementPath(1).points[1][2]-2.25)<1e-6,"P1b room.scale scales path Z by factor");
        check(session.undo().ok && p.getMovementPath(verbp::kListener)==lp && p.getMovementPath(1)==sp2,"P1b undo restores movement paths after room.scale");
        auto waitAppliedSize=[&](){const double deadline=juce::Time::getMillisecondCounterHiRes()+90000;do{block.clear();stage2Process(p,block,midi);juce::MessageManager::getInstance()->runDispatchLoopUntil(5);if(p.isWebSceneCurrent())return true;}while(juce::Time::getMillisecondCounterHiRes()<deadline);return false;};
        // Size macro round trip 100 -> 50 -> 100 returns exact coordinates without cumulative shrink
        setP(p,"s2path",0.5);setP(p,"size",100);check(waitAppliedSize(),"P1b initial Size 100 applied");const auto es100=p.getEnergyScene(1);
        setP(p,"size",50);check(waitAppliedSize(),"P1b Size 50 applied");const auto es50=p.getEnergyScene(1);
        setP(p,"size",100);check(waitAppliedSize(),"P1b restored Size 100 applied");const auto esBack=p.getEnergyScene(1);
        check(!es100.sources.empty() && !es50.sources.empty() && std::abs(es50.sources[0].pos.x-es100.sources[0].pos.x*0.5)<1e-4,"P1b Size=50 scales s2path position by 0.5");
        check(std::abs(esBack.sources[0].pos.x-es100.sources[0].pos.x)<1e-6 && std::abs(esBack.sources[0].pos.y-es100.sources[0].pos.y)<1e-6,"P1b Size 100->50->100 returns identical coordinates");
        // Host -> plugin automation of s2path and lpath updates processor E8/audio positions
        setP(p,"s2path",0.25);setP(p,"lpath",0.75);
        block.clear();stage2Process(p,block,midi);
        check(std::abs(P1Access::rawX(p,1)-4.0)<1e-4 && std::abs(P1Access::rawX(p,verbp::kListener)-6.0)<1e-4,"P1b host automation of s2path and lpath updates rendered object positions");
        setP(p,"s2path",0);setP(p,"lpath",0);
    }

    std::printf("P1 failures %d\n",fails-before);return fails==before?0:1;
}
