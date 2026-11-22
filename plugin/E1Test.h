// E1 neutral compatibility captures. Built before any DSP edit, then reused unchanged.
static int e1Null(bool capture) {
    auto dir=juce::File::getCurrentWorkingDirectory().getChildFile("out/E1/reference");dir.createDirectory();
    for(const auto& room:verbp::builtinRooms()) {
        if(room.name!="Bedroom" && room.name!="Vocal booth (dead)" && room.name!="Concert hall (audience zone)") continue;
        VerbProcessor p;p.setPlayConfigDetails(2,2,48000,256);p.prepareToPlay(48000,256);check(waitReady(p,90),"E1 baseline processor ready");
        p.setTailSettings({48000,false});p.loadRoomWithSetup(room.text,room.name,room.setup,room.micPattern);check(waitReady(p,90),"E1 built-in ready");
        auto stem=juce::String(room.name).retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
        auto f=dir.getChildFile(stem+".f32");auto ir=stage2Impulse(p,6.0);
        if(capture) {
            if(f.existsAsFile()){std::fprintf(stderr,"Refusing reference overwrite\n");return 1;}
            f.replaceWithData(ir.data(),ir.size()*sizeof(float));
            juce::MemoryBlock st;p.getStateInformation(st);dir.getChildFile(stem+".state").replaceWithData(st.getData(),st.getSize());
            dir.getChildFile(stem+".verbroom").replaceWithText(p.getRoomPresetText());
            std::printf("REFERENCE %s %zu samples\n",room.name.c_str(),ir.size());
        } else {
            juce::MemoryBlock data;check(f.loadFileAsData(data)&&data.getSize()==ir.size()*sizeof(float),"E1 reference exists with correct length");
            double peak=0;if(data.getSize()==ir.size()*sizeof(float))for(size_t i=0;i<ir.size();++i)peak=std::max(peak,std::abs((double)ir[i]-((const float*)data.getData())[i]));else peak=1;
            std::printf("NULL %s peak %.12g FS (%.3f dBFS)\n",room.name.c_str(),peak,20*std::log10(std::max(1e-30,peak)));
            check(peak<=1e-6,"E1 all neutral built-in IR difference <= -120 dBFS");
        }
    }
    return fails?1:0;
}
#include "WebParameterBindings.h"
static bool e1Settle(VerbProcessor& p) {
    juce::AudioBuffer<float> b(std::max(p.hostInputChannels(),p.hostOutputChannels()),256);juce::MidiBuffer midi;
    const auto end=juce::Time::getMillisecondCounterHiRes()+90000;
    do {b.clear();stage2Process(p,b,midi);juce::MessageManager::getInstance()->runDispatchLoopUntil(15);
        if(p.isTailCurrent()&&p.isWebSceneCurrent()){for(int k=0;k<80;++k){b.clear();stage2Process(p,b,midi);}return true;}
    }while(juce::Time::getMillisecondCounterHiRes()<end);
    return false;
}
struct E1HostSpy:juce::AudioProcessorParameter::Listener {
    int begin=0,end=0,changes=0;
    void parameterValueChanged(int,float) override{++changes;}
    void parameterGestureChanged(int,bool starting) override{starting?++begin:++end;}
};
static int e1Processor() {
    VerbProcessor p;p.setPlayConfigDetails(2,2,48000,256);p.prepareToPlay(48000,256);check(waitReady(p,90),"E1 processor starts");p.setTailSettings({8000,false});check(e1Settle(p),"E1 initial tail current");
    verbweb::WebParameterBindings binding(p);
    const char* ids[]={"size","predelay","stereoWidth","freeze","duck","duckRelease"};
    const double values[]={120,73,154,1,12,600};E1HostSpy spies[6];
    for(int i=0;i<6;++i){auto* parameter=p.apvts.getParameter(ids[i]);parameter->addListener(&spies[i]);parameter->beginChangeGesture();setP(p,ids[i],values[i]);parameter->endChangeGesture();check(spies[i].begin==1&&spies[i].end==1&&spies[i].changes>=1,"host automation reports one begin/change/end for E1 parameter");parameter->removeListener(&spies[i]);}
    auto snapshot=binding.snapshot(false).getProperty("effective",{});
    check((double)snapshot.getProperty("predelayMs",-1)==0&&(double)snapshot.getProperty("stereoWidthPercent",-1)==100&&!(bool)snapshot.getProperty("freeze",true)&&(double)snapshot.getProperty("duckDb",-1)==0&&(double)snapshot.getProperty("duckReleaseMs",-1)==300,"Physical exposes neutral effective values for all Creative E1 controls");
    check(e1Settle(p),"Size settles in Physical");const auto original=p.getDisplayScene();const auto modeled=p.getWebModeledScene();
    check(modeled&&std::abs(modeled->height-original.height*1.2)<1e-6,"Size effective height scales; authored scene does not");
    const auto energy=p.getEnergyScene();check(energy.sources.size()>0&&std::abs(energy.sources[0].pos.x-p.apvts.getRawParameterValue("s1x")->load()*1.2)<1e-6,"Energy uses scaled source positions and geometry");
    setP(p,"mode",1);snapshot=binding.snapshot(false).getProperty("effective",{});
    check(std::abs((double)snapshot.getProperty("predelayMs",-1)-73)<.002&&std::abs((double)snapshot.getProperty("stereoWidthPercent",-1)-154)<.002&&(bool)snapshot.getProperty("freeze",false)&&std::abs((double)snapshot.getProperty("duckDb",-1)-12)<.002,"Creative restores stored E1 overrides");
    juce::MemoryBlock state;p.getStateInformation(state);auto xml=juce::AudioProcessor::getXmlFromBinary(state.getData(),(int)state.getSize());auto tree=juce::ValueTree::fromXml(*xml);
    check((double)tree.getChildWithProperty("id","freeze").getProperty("value")==0&&p.apvts.getRawParameterValue("freeze")->load()==1,"serialising Freeze off does not mutate live stored state");
    VerbProcessor q;q.setPlayConfigDetails(2,2,48000,256);q.prepareToPlay(48000,256);q.setStateInformation(state.getData(),(int)state.getSize());check(waitReady(q,90)&&e1Settle(q),"E1 project reload prepares effective scene");
    check(std::abs(q.apvts.getRawParameterValue("size")->load()-120)<.002&&std::abs(q.apvts.getRawParameterValue("predelay")->load()-73)<.002&&q.apvts.getRawParameterValue("freeze")->load()==0,"E1 project values round trip; Freeze always starts off");
    juce::File::getCurrentWorkingDirectory().getChildFile("out/E1/new.verbroom").replaceWithText(p.getRoomPresetText());
    juce::String why;check(q.loadRoomPresetText(p.getRoomPresetText(),why)&&waitReady(q,90)&&e1Settle(q),"E1 full preset reload passes parser and engine preparation");
    check(std::abs(q.getDisplayScene().height-original.height)<1e-9&&q.apvts.getRawParameterValue("freeze")->load()==0,"full preset stores unscaled geometry and Freeze off");
    // Missing E1 IDs explicitly reset a previously configured instance.
    const auto legacy=juce::File::getCurrentWorkingDirectory().getChildFile("out/E1/reference/Bedroom.verbroom").loadFileAsString();
    check(q.loadRoomPresetText(legacy,why)&&waitReady(q,90)&&e1Settle(q),"pre-E1 full preset loads after non-neutral E1 state");
    bool neutral=true;const double defaults[]={100,0,100,0,0,300};for(int i=0;i<6;++i)neutral&=std::abs(q.apvts.getRawParameterValue(ids[i])->load()-defaults[i])<.002;check(neutral,"old preset resets every E1 parameter to neutral");
    // Size is not destructive, and its traced physical RT60 increases monotonically.
    setP(q,"size",50);check(e1Settle(q),"Size 50 tail current");auto smallRt=q.getTailDiagnostics()->physicalRT60;
    setP(q,"size",100);check(e1Settle(q),"Size 100 tail current");auto medium=q.getTailDiagnostics()->physicalRT60;
    setP(q,"size",160);check(e1Settle(q),"Size 160 tail current");auto big=q.getTailDiagnostics()->physicalRT60;
    bool monotonic=true;for(int b=0;b<7;++b){std::printf("  Size octave %d physical RT60 %.5f / %.5f / %.5f s\n",b,smallRt[b],medium[b],big[b]);if(smallRt[b]>0&&medium[b]>0&&big[b]>0)monotonic&=smallRt[b]<medium[b]&&medium[b]<big[b];}check(monotonic,"traced per-octave RT60 grows with Size");
    setP(p,"freeze",0);setP(p,"mode",1);juce::AudioBuffer<float> block(2,256);juce::MidiBuffer midi;
    stage2AudioAllocations=0;for(int n=0;n<300;++n){setP(p,"predelay",n%251);setP(p,"stereoWidth",n%201);setP(p,"freeze",n%67<30?1:0);setP(p,"duck",n%25);block.clear();for(int i=0;i<256;++i)block.setSample(0,i,.1f*(float)std::sin((n*256+i)*.1));stage2Process(p,block,midi);}
    std::printf("  E1 C++ callback allocations: %zu\n",stage2AudioAllocations);check(stage2AudioAllocations==0,"all E1 automation/switches allocate zero in processBlock");
    check(p.getLatencySamples()==0,"E1 adds zero plugin latency");
    for(const char* id:{"predelay","stereoWidth","freeze","duck"})setP(q,id,juce::String(id)=="stereoWidth"?100:0);
    setP(q,"size",120);check(e1Settle(q),"roundtrip impulse settles");auto before=stage2Impulse(q,1);
    const auto preset=q.getRoomPresetText();check(q.loadRoomPresetText(preset,why)&&waitReady(q,90)&&e1Settle(q),"Size preset repeated load current");auto after=stage2Impulse(q,1);const auto error=stage2Error(before,after);std::printf("  Size preset impulse reload max error %.12g FS\n",error);check(error<=1e-6,"Size impulse matches after preset reload <= -120 dBFS");
    q.editScene([](verb::Scene& sc){sc.ceilMat=-1;});setP(q,"s1z",30);setP(q,"lz",30);setP(q,"size",160);
    check(e1Settle(q),"Size open room high points settle");const auto open=q.getEnergyScene();
    check(!open.sources.empty()&&std::abs(open.sources[0].pos.z-48)<1e-5,"Size open room permits author30m -> effective48m source height");
    return fails?1:0;
}

static int e1Performance() {
    const int blk=std::getenv("VERB_E2_BLOCK")?std::clamp(std::atoi(std::getenv("VERB_E2_BLOCK")),64,512):64;
    std::vector<double> times;times.reserve(4800);
    VerbProcessor p;p.setPlayConfigDetails(64,12,44100,blk);p.prepareToPlay(44100,blk);check(waitReady(p,90),"E1 performance initial processor");
    auto room=verbp::vslGrandHall(false);p.setTailSettings({48000,false});verbp::OutputCfg o;o.layout=(int)verb::Layout::Surround714;p.setOutputCfg(o);
    setP(p,"size",160);setP(p,"mode",1);setP(p,"predelay",250);setP(p,"stereoWidth",200);setP(p,"duck",24);setP(p,"duckRelease",50);
    p.loadRoomWithSetup(room.text,room.name,room.setup,room.micPattern);check(waitReady(p,120)&&e1Settle(p),"largest VSL hall, Size160 and 7.1.4 prepare");
    juce::AudioBuffer<float> b(64,blk);juce::MidiBuffer midi;bool finite=true;double worst=0,total=0;size_t blocks=0;const double memory=privateMB();
    stage2AudioAllocations=0;
    for(int k=0;k<4800;++k) {
        if(k==1500)setP(p,"freeze",1);
        for(int ch=0;ch<64;++ch)for(int i=0;i<blk;++i)b.setSample(ch,i,.001f*(float)std::sin(.137*(k*blk+i)+ch));
        const auto start=std::chrono::steady_clock::now();stage2Process(p,b,midi);const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(k>=1500){worst=std::max(worst,ms);total+=ms;++blocks;times.push_back(ms);}
        for(int ch=0;ch<12;++ch)for(int i=0;i<blk;++i)finite&=std::isfinite(b.getSample(ch,i));
    }
    const double budget=1000.*blk/44100.;std::printf("E1 PERF %s: %d points, 64 input / 12 output, 44.1kHz / %d; all five active; Size160, 48000 rays, shared tail\n",room.name.c_str(),p.numPoints(),blk);
    std::printf("E1 PERF mean %.6f ms (%.2f%% of core), worst %.6f ms vs %.6f ms budget; private %.2f MiB; allocations %zu; deadline %s\n",total/blocks,100*total/blocks/budget,worst,budget,memory,stage2AudioAllocations,worst<=budget?"MET":"EXCEEDED");
    std::sort(times.begin(),times.end());std::printf("E2 PLUGIN block=%d p99=%.6f ms\n",blk,times[times.size()*99/100]);
    check(finite&&stage2AudioAllocations==0,"worst-case E1 host processing stays finite and allocation-free");
    return fails?1:0;
}
