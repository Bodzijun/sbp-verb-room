#pragma once
#include "E9bTest.h"
static int testE9Plugin() {
    if(std::getenv("VERB_E9B_REPRO"))return testE9bRepro();
    if(std::getenv("VERB_E9B_CLOSED"))return testE9bClosed();
    const int before=fails;VerbProcessor p;p.setPlayConfigDetails(2,2,48000,128);p.prepareToPlay(48000,128);check(waitReady(p,90),"E9 processor ready");
    verbp::MovementPath path;double x=p.apvts.getRawParameterValue("lx")->load(),y=p.apvts.getRawParameterValue("ly")->load(),z=p.apvts.getRawParameterValue("lz")->load();
    path.points={verbp::PathPoint{x,y,z},verbp::PathPoint{x+.3,y,z}};check(p.setMovementPath(verbp::kListener,path),"E9 listener path accepted");auto& session=p.getEditorSession();auto revision=p.getRevision();auto undo=session.undoDepth();check(session.run(juce::JSON::parse(R"({"op":"path.bake","object":32})")).ok,"E9b bake command starts worker");check(p.getRevision()==revision&&session.undoDepth()==undo,"E9b bake command changes neither document revision nor undo");
    juce::AudioBuffer<float> buffer(2,128);juce::MidiBuffer midi;const double deadline=juce::Time::getMillisecondCounterHiRes()+90000;
    while(p.pathBakePercent()<100&&juce::Time::getMillisecondCounterHiRes()<deadline){buffer.clear();p.processBlock(buffer,midi);juce::MessageManager::getInstance()->runDispatchLoopUntil(2);juce::Thread::sleep(1);}
    check(p.pathBakePercent()==100,"E9 loader publishes complete worker bake progress");
    setP(p,"lpath",.8);buffer.clear();p.processBlock(buffer,midi);check(buffer.getMagnitude(0,128)==0,"E9 silent input remains silent during baked path motion");
    auto disabled=path;disabled.enabled=false;check(p.setMovementPath(verbp::kListener,disabled),"E9 path can disable without changing preset format");
    double sx=p.apvts.getRawParameterValue("s1x")->load(),sy=p.apvts.getRawParameterValue("s1y")->load(),sz=p.apvts.getRawParameterValue("s1z")->load();verbp::MovementPath source;source.points={verbp::PathPoint{sx,sy,sz},verbp::PathPoint{sx+.3,sy,sz}};check(p.setMovementPath(0,source),"E9 source path accepted");check(p.startPathBake(0),"E9b source bake action accepted");
    const double sourceDeadline=juce::Time::getMillisecondCounterHiRes()+90000;double start=juce::Time::getMillisecondCounterHiRes();
    do {buffer.clear();p.processBlock(buffer,midi);juce::MessageManager::getInstance()->runDispatchLoopUntil(2);juce::Thread::sleep(1);}while((juce::Time::getMillisecondCounterHiRes()-start<500||p.pathBakePercent()<100)&&juce::Time::getMillisecondCounterHiRes()<sourceDeadline);
    if(p.pathBakePercent()!=100)std::printf("E9b source-switch status %s\n",juce::JSON::toString(p.pathBakeStatus()).toRawUTF8());
    check(p.pathBakePercent()==100,"E9 source path worker bake completes");
    // P4 (owner 2026-11-18 overrides E9 session-only): the state now carries the baked table as a BakeCache child.
    {auto e=E9bAccess::engine(p);auto ts=e->bakedTablesSnapshot();auto sg=e->bakeContentSignatures();std::printf("E9 DIAG before save: tables=%zu tableSig=%016llx liveSig=%016llx groups=%zu\n",ts.size(),ts.empty()||!ts[0]?0ull:(unsigned long long)ts[0]->contentSig,sg.empty()?0ull:(unsigned long long)sg[0],sg.size());}
    juce::MemoryBlock state;p.getStateInformation(state);auto xml=p.getXmlFromBinary(state.getData(),(int)state.getSize());check(xml&&xml->toString().contains("BakeCache"),"E9 project state contains path and the P4 baked-table cache");
    p.setStateInformation(state.getData(),(int)state.getSize());check(waitReady(p,90),"E9 project state reload rebuilds engine");auto restored=p.getMovementPath(0);
    if(!(restored==source)){std::printf("E9 reload mismatch points %zu/%zu flags %d%d%d%d/%d%d%d%d laps %d/%d\n",restored.points.size(),source.points.size(),restored.enabled,restored.closed,restored.smooth,restored.orient,source.enabled,source.closed,source.smooth,source.orient,restored.laps,source.laps);for(size_t j=0;j<std::min(restored.points.size(),source.points.size());++j)for(int k=0;k<3;++k)std::printf("  point%zu axis%d %.17g -> %.17g delta %.17g\n",j,k,source.points[j][k],restored.points[j][k],restored.points[j][k]-source.points[j][k]);if(xml)if(auto*paths=xml->getChildByName("MovementPaths"))std::printf("%s\n",paths->toString().toRawUTF8());}
    check(restored==source,"E9 source path survives project reload");
    {const double rd=juce::Time::getMillisecondCounterHiRes()+20000;while(p.pathBakePercent()<100&&juce::Time::getMillisecondCounterHiRes()<rd){buffer.clear();p.processBlock(buffer,midi);juce::MessageManager::getInstance()->runDispatchLoopUntil(2);juce::Thread::sleep(1);}
     {auto e=E9bAccess::engine(p);auto sg=e->bakeContentSignatures();std::printf("E9 DIAG after reload: liveSig=%016llx groups=%zu\n",sg.empty()?0ull:(unsigned long long)sg[0],sg.size());}
     if(p.pathBakePercent()!=100)std::printf("E9 DIAG restore status %s\n",juce::JSON::toString(p.pathBakeStatus()).toRawUTF8());}
    check(p.pathBakePercent()==100,"E9b reopened project restores the baked table (P4 persistence)");
    check(p.pathBakeStatus()["state"].toString()=="baked"||p.pathBakeStatus()["state"].toString()=="approximate","E9b reopened project reports baked after restore");
    p.releaseResources();if(fails!=before)return 1;const int openResult=testE9bRepro();const int closedResult=testE9bClosed();return openResult|closedResult;
}
