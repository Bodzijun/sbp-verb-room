#pragma once
#include <thread>
#include <atomic>
#include <sstream>
#include "../src/layouts.hpp"
struct E9bAccess {
    static std::shared_ptr<verb::Engine> engine(VerbProcessor& p){juce::SpinLock::ScopedLockType lock(p.engineLock);return p.bundle?p.bundle->engine:nullptr;}
};
static int testE9bRepro(){
    const int before=fails;VerbProcessor p;p.setPlayConfigDetails(2,2,48000,128);p.prepareToPlay(48000,128);check(waitReady(p,90),"E9b initial processor ready");juce::String error;
    auto file=juce::File::getCurrentWorkingDirectory().getChildFile("out/E9b/owner-repro-demo-motion.verbroom");
    check(p.loadRoomPresetText(file.loadFileAsString(),error)&&waitReady(p,90),"E9b owner repro loads through processor");
    check(p.pathBakePercent()==0,"E9b project load does not auto-bake");check(p.startPathBake(0),"E9b manual owner bake accepted");
    juce::AudioBuffer<float>buffer(2,128);juce::MidiBuffer midi;const auto start=std::chrono::steady_clock::now();int last=-1;auto engine=E9bAccess::engine(p);
    const auto allocations=stage2AudioAllocations;uint64_t blocks=0;bool bakedDuringDrain=false;
    while(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<360){buffer.clear();stage2Process(p,buffer,midi);if(++blocks%16==0)juce::MessageManager::getInstance()->runDispatchLoopUntil(1);juce::Thread::sleep(1);auto s=engine->bakeStats();auto m=engine->motionStats();bakedDuringDrain|=s.stations>0&&m.transitionsStarted>m.transitionsDone;int second=(int)std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();if(second!=last){last=second;std::printf("E9b t%d web%d engine%d reason%d stations%llu resets%llu cancelled%llu load%llu tailCurrent%d\n",second,p.pathBakePercent(),s.percent,s.waitReason,(unsigned long long)s.stations,(unsigned long long)s.resets,(unsigned long long)s.cancelled,(unsigned long long)p.getLoadSerial(),p.isWebSceneCurrent());}if(p.pathBakePercent()==100)break;}
    check(bakedDuringDrain,"E9b baking progresses while completed settled IR drains");
    check(stage2AudioAllocations==allocations,"E9b full-power bake allocates nothing in processBlock");
    check(p.pathBakePercent()==100,"E9b owner path bake reaches completion");auto stats=engine->bakeStats();auto table=engine->bakedTableSnapshot();std::printf("E9B_BAKE open ms%.3f workers%d nodes%zu residual%d hash%016llx\n",stats.milliseconds,stats.workers,table?table->u.size():0,stats.discontinuities,(unsigned long long)stats.hash);if(stats.discontinuities)check(stats.state==verb::Engine::BakeState::Approximate,"E9b residual intervals are published as baked approx");p.releaseResources();return fails==before?0:1;
}
static int testE9bClosed(){
    const int before=fails;auto file=juce::File::getCurrentWorkingDirectory().getChildFile("out/E9b/owner-repro-demo-motion.verbroom");auto xml=juce::parseXML(file.loadFileAsString());if(!xml){check(false,"E9b fixture XML present");return 1;}std::istringstream text(xml->getChildByName("VerbState")->getStringAttribute("sceneText").toStdString());auto scene=verb::parseScene(text,"closed regression");int mat=scene.walls[0].mat;scene.walls={{2,0,4,0,mat},{4,0,4,8,mat},{4,8,2,8,mat},{2,8,2,0,mat}};
    VerbProcessor p;p.setTailSettings({8000,false});p.setPlayConfigDetails(2,2,48000,128);p.prepareToPlay(48000,128);p.loadSceneText(verb::serializeScene(scene),"E9b closed corridor",true);check(waitReady(p,90),"E9b derived closed corridor loads");
    verbp::MovementPath path;path.smooth=true;path.points={{{2.734,6.248,1.5}},{{3.4,.8,1.5}},{{2.7,5.5,1.5}},{{3.2,2.834,1.5}}};verbp::CompiledPath compiled;compiled.build(path);bool inside=true;for(auto q:compiled.points)inside&=q[0]>2.02&&q[0]<3.98&&q[1]>.02&&q[1]<7.98;check(inside&&compiled.length()>6.2,"E9b four-point smooth source path stays inside and needs33 initial nodes");check(p.setMovementPath(0,path)&&p.startPathBake(0),"E9b closed corridor manual bake starts");auto engine=E9bAccess::engine(p);auto serial=p.getLoadSerial();
    std::atomic<bool>analysisRunning{true};std::atomic<int>analyses{0};std::thread analysis([&]{while(analysisRunning.load()){auto s=p.getEnergyScene(0);s.receivers=verb::makeReceiverArray(verb::Layout::StereoORTF,scene.listenerPos,.1,2);if(!s.sources.empty()&&!s.receivers.empty()){auto e=verb::traceTailEnergy(s,s.sources[0],s.receivers,1,128,s.seed);(void)e;++analyses;}std::this_thread::sleep_for(std::chrono::milliseconds(2));}});
    juce::AudioBuffer<float>buffer(2,128);juce::MidiBuffer midi;auto alloc=stage2AudioAllocations;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);int blocks=0;
    while(p.pathBakePercent()<100&&std::chrono::steady_clock::now()<deadline){buffer.clear();stage2Process(p,buffer,midi);if(++blocks%16==0)juce::MessageManager::getInstance()->runDispatchLoopUntil(1);juce::Thread::sleep(1);}analysisRunning=false;analysis.join();
    check(p.pathBakePercent()==100&&analyses.load()>0&&p.getLoadSerial()==serial,"E9b closed corridor completes while analysis jobs keep running");check(stage2AudioAllocations==alloc,"E9b closed bake processBlock has no allocation");auto stats=engine->bakeStats();auto table=engine->bakedTableSnapshot();std::printf("E9B_BAKE closed ms%.3f workers%d nodes%zu residual%d analyses%d hash%016llx\n",stats.milliseconds,stats.workers,table?table->u.size():0,stats.discontinuities,analyses.load(),(unsigned long long)stats.hash);
    double energy=0;bool finite=true;setP(p,"soloDirect",1);for(int j=0;j<24;++j){buffer.clear();if(j==0)buffer.setSample(0,0,1);stage2Process(p,buffer,midi);for(int c=0;c<2;++c)for(int k=0;k<128;++k){auto v=buffer.getSample(c,k);finite&=std::isfinite(v);energy+=v*v;}}check(finite&&energy>0,"E9b closed fixture impulse is finite and reaches output");
    auto preset=p.getRoomPresetText();juce::String error;check(p.loadRoomPresetText(preset,error)&&waitReady(p,90)&&p.getMovementPath(0)==path&&p.getDisplayScene().walls.size()==4&&p.pathBakePercent()==0,"E9b derived fixture preset reload preserves geometry/path and needs manual bake");p.releaseResources();return fails==before?0:1;
}
