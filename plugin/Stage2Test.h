// Included by PluginTest.cpp: deterministic pre-stage-2 compatibility fixture.
#include <cstdlib>
#include <new>
#include <numeric>
#include "../src/dsp.hpp"
static thread_local bool stage2AudioAllocationGuard=false;
static thread_local size_t stage2AudioAllocations=0;
void* operator new(std::size_t size) {
    if(stage2AudioAllocationGuard)++stage2AudioAllocations;
    if(auto* p=std::malloc(std::max<std::size_t>(1,size)))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
static void stage2Process(VerbProcessor& p,juce::AudioBuffer<float>& block,juce::MidiBuffer& midi){
    stage2AudioAllocationGuard=true;p.processBlock(block,midi);stage2AudioAllocationGuard=false;
}
static std::vector<float> stage2Impulse(VerbProcessor& p, double seconds = 4.0) {
    juce::AudioBuffer<float> block(2, 256); juce::MidiBuffer midi;
    // Allow the first asynchronous pose/directivity refresh to run before recording.
    // Then wait for the actual audio slots, rather than trusting an older diagnostics pointer.
    const auto warmUntil=juce::Time::getMillisecondCounterHiRes()+600;
    do {block.clear();stage2Process(p,block,midi);juce::MessageManager::getInstance()->runDispatchLoopUntil(20);}
    while(juce::Time::getMillisecondCounterHiRes()<warmUntil);
    const auto readyUntil=juce::Time::getMillisecondCounterHiRes()+120000;
    while(!p.isTailCurrent()&&juce::Time::getMillisecondCounterHiRes()<readyUntil){
        block.clear();stage2Process(p,block,midi);juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    }
    check(p.isTailCurrent(),"IR capture waits for the current audio tail slots");
    // Flush convolution history and finish the existing level/pose ramps.
    for (int k = 0; k < 4000; ++k) { block.clear(); stage2Process(p,block,midi); }
    std::vector<float> result((size_t)(seconds * 48000) * 2);
    for (size_t pos = 0; pos < result.size() / 2; pos += 256) {
        block.clear(); if (pos == 0) block.setSample(0, 0, 1.f);
        stage2Process(p,block,midi);
        for (size_t i = 0; i < 256 && pos + i < result.size() / 2; ++i)
            for (size_t ch = 0; ch < 2; ++ch) result[2 * (pos + i) + ch] = block.getSample((int)ch, (int)i);
    }
    return result;
}

static int captureStage2Legacy() {
    if (juce::File::getCurrentWorkingDirectory().getChildFile("plugin/fixtures/stage2/legacy.state").existsAsFile()) {
        std::fprintf(stderr,"Refusing to overwrite pre-change fixture\n"); return 1;
    }
    VerbProcessor p; p.setPlayConfigDetails(2, 2, 48000, 256); p.prepareToPlay(48000, 256);
    if (!waitReady(p, 90)) return 1;
    p.setTailSettings({48000, false});
    p.loadSceneText(juce::File::getCurrentWorkingDirectory().getChildFile("scenes/demo_box.txt").loadFileAsString(), "legacy stage2 box", true);
    if (!waitReady(p, 90)) return 1;
    setP(p, "early", -9); setP(p, "tail", -3); setP(p, "diffusion", .5); setP(p, "doppler", .7);
    auto dir = juce::File::getCurrentWorkingDirectory().getChildFile("plugin/fixtures/stage2"); dir.createDirectory();
    juce::MemoryBlock state; p.getStateInformation(state);
    dir.getChildFile("legacy.state").replaceWithData(state.getData(), state.getSize());
    dir.getChildFile("legacy.verbroom").replaceWithText(p.getRoomPresetText());
    const auto ir = stage2Impulse(p);
    dir.getChildFile("legacy-ir.f32").replaceWithData(ir.data(), ir.size() * sizeof(float));
    dir.getChildFile("README.txt").replaceWithText("Captured before stage 2 DSP/processor edits, MSVC Release AVX2, 48000 Hz, stereo interleaved float32, 4 seconds, seed from demo_box.txt; early=-9, tail=-3, diffusion=0.5, doppler=0.7, 48000 rays non-adaptive. Compare peak absolute sample error <= 2e-6.\n");
    std::printf("Captured legacy state, preset and %zu samples\n", ir.size());
    return 0;
}

static double stage2Error(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size()!=b.size()) return 1e10;
    double e=0; for (size_t i=0;i<a.size();++i) e=std::max(e,(double)std::abs(a[i]-b[i])); return e;
}

static std::vector<float> stage2Motion(VerbProcessor& p) {
    juce::AudioBuffer<float> block(2,256);juce::MidiBuffer midi;
    const double x=p.apvts.getRawParameterValue("s1x")->load();setP(p,"soloDirect",1);
    for(int k=0;k<100;++k){block.clear();stage2Process(p,block,midi);}
    std::vector<float> result(256*200);
    for(int k=0;k<200;++k){
        setP(p,"s1x",x+.6*k*256/48000.);
        block.clear();for(int i=0;i<256;++i)block.setSample(0,i,.1f*(float)std::sin(2*verb::kPi*440*(k*256+i)/48000.));
        stage2Process(p,block,midi);std::copy_n(block.getReadPointer(0),256,result.data()+k*256);
    }
    setP(p,"s1x",x);setP(p,"soloDirect",0);return result;
}

static bool stage2Settle(VerbProcessor& p, std::array<double,3> multipliers, double absorption=1.) {
    juce::AudioBuffer<float> block(2,256); juce::MidiBuffer midi;
    const auto until=juce::Time::getMillisecondCounterHiRes()+120000;
    while (juce::Time::getMillisecondCounterHiRes()<until) {
        block.clear(); stage2Process(p,block,midi);
        const auto d=p.getTailDiagnostics();
        // Wait for absorption's loader update, plus a fully built diagnostic snapshot.
        if (d && d->multipliers==multipliers && p.isTailCurrent()) {
            const auto estimate=p.getRT60Estimate();
            if (estimate[1]>0 && (absorption==1. || std::abs(p.apvts.getRawParameterValue("absorption")->load()-absorption)<.001)) {
                // Let the loader observe geometry/absorption, then drain any pending crossfade.
                juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
                for(int i=0;i<100;++i) {block.clear();stage2Process(p,block,midi);}
                const auto after=p.getTailDiagnostics();
                if(after && after->multipliers==multipliers && p.isTailCurrent()) return true;
            }
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    }
    return false;
}

static void stage2LegacyCompatibility(VerbProcessor& p,const std::vector<float>& actual,const std::vector<float>& expected) {
    // Approved transition fix changes the statistical tail inside 0..650 ms.
    // Keep the original sample tolerance outside the FIR-expanded changed region.
    double outside=0;
    for(size_t i=0;i<actual.size();++i)if(i/2<480||i/2>=32640)outside=std::max(outside,(double)std::abs(actual[i]-expected[i]));
    std::printf("  Legacy IR outside transition max error %.9g FS\n",outside);
    // The fixture was captured with MSVC. Other compilers/standard libraries round the same float maths
    // differently (measured in CI: 5.6e-6 FS on macOS, 2.8e-5 FS on Linux), so only MSVC keeps the 2e-6 bound;
    // elsewhere the bound is 1e-4 FS (-80 dBFS), still far below any audible or structural change.
#if defined(_MSC_VER)
    const double legacyTolerance=2e-6;
#else
    const double legacyTolerance=1e-4;
#endif
    check(actual.size()==expected.size()&&outside<=legacyTolerance,"legacy IR outside 10..680 ms retains original sample tolerance (2e-6 FS MSVC, 1e-4 FS elsewhere)");
    setP(p,"soloDirect",1);setP(p,"soloEarly",1);setP(p,"soloTail",0);
    const auto directEarly=stage2Impulse(p);
    setP(p,"soloDirect",0);setP(p,"soloEarly",0);
    bool energyOK=true,rtOK=true;
    const size_t length=actual.size()/2,n=verb::nextPow2(length*2);
    for(int b=0;b<verb::NB;++b) {
        std::vector<double> oldEnergy(length),newEnergy(length);
        const double low=verb::kBandHz[b]/std::sqrt(2.),high=verb::kBandHz[b]*std::sqrt(2.);
        for(int channel=0;channel<2;++channel)for(int variant=0;variant<2;++variant) {
            std::vector<verb::cd> spectrum(n);
            for(size_t i=0;i<length;++i)spectrum[i]=(variant?actual:expected)[i*2+channel]-directEarly[i*2+channel];
            verb::fft(spectrum,false);
            for(size_t k=0;k<n;++k) {
                const double hz=(k<=n/2?k:n-k)*48000./n;
                const double edge=std::clamp(std::min((hz-low)/(.05*low),(high-hz)/(.05*high)),0.,1.);
                spectrum[k]*=.5-.5*std::cos(verb::kPi*edge);
            }
            verb::fft(spectrum,true);auto& energy=variant?newEnergy:oldEnergy;
            for(size_t i=0;i<length;++i)energy[i]+=std::norm(spectrum[i]);
        }
        auto t20=[](const std::vector<double>& e) {
            const double total=std::accumulate(e.begin(),e.end(),0.);double remaining=total,sx=0,sy=0,sxx=0,sxy=0,count=0;
            for(size_t k=0;k<e.size();++k) {
                const double db=10*std::log10(std::max(1e-300,remaining/total));if(db< -25)break;
                if(db<= -5){const double t=k/48000.;sx+=t;sy+=db;sxx+=t*t;sxy+=t*db;++count;}remaining-=e[k];
            }
            return count>4?-60*(count*sxx-sx*sx)/(count*sxy-sx*sy):-1.;
        };
        const double db=10*std::log10(std::accumulate(newEnergy.begin(),newEnergy.end(),0.)/std::accumulate(oldEnergy.begin(),oldEnergy.end(),0.));
        const double error=t20(newEnergy)/t20(oldEnergy)-1.;
        std::printf("  Legacy octave %.0f late energy %+.5f dB, T20 %+.4f%%\n",verb::kBandHz[b],db,error*100);
        energyOK&=std::abs(db)<=.2;rtOK&=std::abs(error)<=.03;
    }
    check(energyOK,"legacy state/preset late octave energy remains within original physical level +/-0.2 dB");
    check(rtOK,"legacy state/preset late octave T20 remains within +/-3% of 0.1.15");
}

static void stage2ExportAndLoader(VerbProcessor& p) {
    p.setTailSettings({48000,false});
    auto exportScene=verb::loadScene("scenes/demo_box.txt");exportScene.sources.resize(1);
    p.loadSceneText(verb::serializeScene(exportScene),"export-live",true);
    check(waitReady(p,90),"export/live room loads");
    setP(p,"early",-16);setP(p,"tail",-9);setP(p,"diffusion",.8);setP(p,"doppler",.35);
    setP(p,"decayLow",.8);setP(p,"decayMid",1.4);setP(p,"decayHigh",.7);
    setP(p,"mix",100);setP(p,"gain",-3);setP(p,verbp::paramId(0,4),-2);
    const std::array<double,3> storedDecay{{p.apvts.getRawParameterValue("decayLow")->load(),p.apvts.getRawParameterValue("decayMid")->load(),p.apvts.getRawParameterValue("decayHigh")->load()}};
    auto compareExport=[&](int mode) {
        setP(p,"mode",mode);
        check(stage2Settle(p,mode?storedDecay:std::array<double,3>{{1,1,1}}),"export mode tail settles");
        // A static IR compares the stationary transfer, after Doppler's 10 s
        // reference-delay memory has settled (60 s of offline silence). A live
        // startup/moving delay trajectory cannot be encoded in a static WAV.
        juce::AudioBuffer<float> settle(2,256);juce::MidiBuffer settleMidi;
        for(int k=0;k<12000;++k){settle.clear();stage2Process(p,settle,settleMidi);}
        const auto live=stage2Impulse(p,4.);
        const auto dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("verb-stage2-export-"+juce::String(mode));
        dir.deleteRecursively();p.startExportIR(dir);
        const auto until=juce::Time::getMillisecondCounterHiRes()+120000;
        while(p.getExportState()==1&&juce::Time::getMillisecondCounterHiRes()<until)juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        const auto files=dir.findChildFiles(juce::File::findFiles,false,"*.wav");
        std::printf("  Export state %d, files %d: %s\n",p.getExportState(),files.size(),p.getExportMessage().toRawUTF8());
        check(p.getExportState()==2&&files.size()==1,"IR export finishes with one file per point");
        if(files.size()==1) {
            juce::WavAudioFormat format;std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(files[0].createInputStream().release(),true));
            std::vector<float> exported(live.size(),0.f);
            if(reader) {
                juce::AudioBuffer<float> data(2,(int)std::min<int64_t>(reader->lengthInSamples,(int64_t)live.size()/2));
                reader->read(&data,0,data.getNumSamples(),0,true,true);
                for(int i=0;i<data.getNumSamples();++i)for(int r=0;r<2;++r)exported[2*i+r]=data.getSample(r,i);
            }
            const double error=stage2Error(live,exported);
            std::printf("  Export/live mode %d maximum error %.9g FS\n",mode,error);
            check(reader&&error<=2e-6,"export matches live impulse <=2e-6 FS in each mode including non-default acoustic controls/gains");
        }
        dir.deleteRecursively();
    };
    for(int mode:{0,1})compareExport(mode);
    // A furniture/material edit plus sub-refresh source motion must retain the
    // actual modeled obstacles and current audible IR, not a regenerated room.
    auto furnished=verb::loadScene("scenes/demo_furnished.txt");furnished.sources.resize(1);
    p.loadSceneText(verb::serializeScene(furnished),"furnished export",true);check(waitReady(p,90),"furnished export geometry loads");
    setP(p,"absorption",1.4);setP(p,"s1x",p.apvts.getRawParameterValue("s1x")->load()+.01);
    check(stage2Settle(p,storedDecay,1.4),"furnished absorption reaches live tail before export");
    setP(p,"doppler",1); // compare static geometry, without a prior motion-reference trajectory
    compareExport(1);
    setP(p,"absorption",1);setP(p,"mode",0);
    // A geometry load is deliberately interrupted by a saved Physical project.
    check(stage2Settle(p,{{1,1,1}}),"Physical room settles before loader race test");
    juce::MemoryBlock physical;p.getStateInformation(physical);
    for(int i=0;i<3;++i) {
        setP(p,"mode",1);p.loadSceneText(p.getRoomText(),"superseded Creative geometry",true);
        p.setStateInformation(physical.getData(),(int)physical.getSize());
        check(waitReady(p,90)&&p.apvts.getRawParameterValue("mode")->load()==0,"restored Physical survives a finishing geometry loader");
    }
    // Supersede an expensive hall geometry/absorption trace with a cached small room.
    const auto rooms=verbp::builtinRooms();
    const auto responseRoom=std::find_if(rooms.begin(),rooms.end(),[](const auto& r){return r.name.find("Vocal booth")!=std::string::npos;});
    p.setTailSettings({8000,false});p.loadSceneText(responseRoom->text,"cached response probe",true);
    check(waitReady(p,90),"small response probe loads at minimum ray budget");
    juce::MemoryBlock responseState;p.getStateInformation(responseState);
    p.setStateInformation(responseState.getData(),(int)responseState.getSize());
    check(waitReady(p,90),"response probe's exact restored coordinates are cached");
    auto hall=std::find_if(rooms.begin(),rooms.end(),[](const auto& r){return r.name.find("Cathedral")!=std::string::npos;});
    p.loadSceneText(hall->text,"expensive superseded room",true);
    juce::Thread::sleep(60);
    const auto start=juce::Time::getMillisecondCounterHiRes();
    p.setStateInformation(responseState.getData(),(int)responseState.getSize());
    const bool ready=waitReady(p,5);
    const double elapsed=(juce::Time::getMillisecondCounterHiRes()-start)*.001;
    std::printf("  Loader supersession %.6f s\n",elapsed);
    check(ready&&elapsed<2.,"new cached scene supersedes an active expensive trace within 2 seconds");
}

static int testStage2Processor() {
    const auto dir=juce::File::getCurrentWorkingDirectory().getChildFile("plugin/fixtures/stage2");
    juce::MemoryBlock state, raw;
    check(dir.getChildFile("legacy.state").loadFileAsData(state) && dir.getChildFile("legacy-state-ir.f32").loadFileAsData(raw),"pre-change binary fixtures exist");
    std::vector<float> expected(raw.getSize()/sizeof(float)); std::memcpy(expected.data(),raw.getData(),raw.getSize());
    VerbProcessor p; p.setPlayConfigDetails(2,2,48000,256); p.prepareToPlay(48000,256);
    check(waitReady(p,90),"stage2 processor loads");
    check(p.apvts.getRawParameterValue("mode")->load()==0 && std::abs(p.apvts.getRawParameterValue("early")->load())<1e-4,"new instance: Physical, early 0 dB");
    p.loadSceneText(p.getRoomText(),"geometry Physical",true);
    check(waitReady(p,90)&&p.apvts.getRawParameterValue("mode")->load()==0,"geometry import preserves Physical");
    setP(p,"mode",1);p.loadSceneText(p.getRoomText(),"geometry Creative",true);
    check(waitReady(p,90)&&p.apvts.getRawParameterValue("mode")->load()==1,"geometry import preserves Creative");
    {auto old=juce::AudioProcessor::getXmlFromBinary(state.getData(),(int)state.getSize());
        old->removeChildElement(old->getChildByAttribute("id","early"),true);
        juce::MemoryBlock missing;juce::AudioProcessor::copyXmlToBinary(*old,missing);
        p.setStateInformation(missing.getData(),(int)missing.getSize());
        const auto* early=p.apvts.getParameter("early");
        const float storedMinusNine=early->convertFrom0to1(early->convertTo0to1(-9.f));
        check(waitReady(p,90)&&p.apvts.getRawParameterValue("mode")->load()==1&&p.apvts.getRawParameterValue("early")->load()==storedMinusNine,"legacy missing early restores exact JUCE representation of old -9 dB default");}
    check(p.setTargetRT60(0)<0&&p.getTargetRT60State()==5,"invalid RT60 target has a separate diagnostic");
    for(const char* id:{"absorption","decayLow","decayMid","decayHigh"}) {
        auto* param=p.apvts.getParameter(id);
        check(std::abs(param->convertFrom0to1(.5f)-1.f)<1e-5,"new multiplier range centres unity");
    }
    p.setStateInformation(state.getData(),(int)state.getSize());
    check(waitReady(p,90) && p.apvts.getRawParameterValue("mode")->load()==1 && std::abs(p.apvts.getRawParameterValue("early")->load()+9)<1e-4,"legacy state explicitly restores Creative and stored early -9");
    const auto legacy=stage2Impulse(p); double error=stage2Error(legacy,expected);
    std::printf("  Legacy state IR max absolute error %.9g\n",error);
    stage2LegacyCompatibility(p,legacy,expected);
    juce::String message;
    check(p.loadRoomPresetText(dir.getChildFile("legacy.verbroom").loadFileAsString(),message) && waitReady(p,90),"pre-change portable preset loads");
    raw.setSize(0); dir.getChildFile("legacy-preset-ir.f32").loadFileAsData(raw); expected.resize(raw.getSize()/sizeof(float));std::memcpy(expected.data(),raw.getData(),raw.getSize());
    const auto presetLegacy=stage2Impulse(p);error=stage2Error(presetLegacy,expected); std::printf("  Legacy preset IR difference (transition fix) %.9g\n",error);
    stage2LegacyCompatibility(p,presetLegacy,expected);
    check(p.apvts.getRawParameterValue("mode")->load()==1,"legacy preset restores Creative");
    {
        VerbProcessor a,b;
        for(auto* processor:{&a,&b}){processor->setPlayConfigDetails(2,2,48000,256);processor->prepareToPlay(48000,256);processor->setStateInformation(state.getData(),(int)state.getSize());check(waitReady(*processor,90),"moving-source mode processor loads");}
        setP(a,"mode",0);setP(a,"doppler",0);setP(b,"mode",1);setP(b,"doppler",1);
        const auto physicalMotion=stage2Motion(a),defaultMotion=stage2Motion(b);
        const double motionError=stage2Error(physicalMotion,defaultMotion);
        std::printf("  Physical/default Creative moving-source error %.9g\n",motionError);
        check(motionError<2e-6,"Physical enforces physical Doppler during source motion despite a stored zero");
        setP(b,"doppler",0);
        check(stage2Error(defaultMotion,stage2Motion(b))>1e-5,"Creative zero Doppler changes the moving-source render");
    }
    setP(p,"mode",0); check(stage2Settle(p,{{1,1,1}}),"Physical tail ready");
    const auto physical=stage2Impulse(p);
    setP(p,"mode",1);setP(p,"early",0);setP(p,"tail",0);setP(p,"doppler",1);
    check(stage2Settle(p,{{1,1,1}}),"default Creative tail settles");
    check(stage2Error(physical,stage2Impulse(p))<2e-6,"Physical equals Creative with explicit physical defaults");
    setP(p,"mode",0);
    setP(p,"early",-24);setP(p,"tail",-12);setP(p,"doppler",0);
    setP(p,"decayLow",2);setP(p,"decayMid",.5);setP(p,"decayHigh",2);
    error=stage2Error(physical,stage2Impulse(p));
    std::printf("  Bypassed overrides IR error %.9g\n",error);
    check(error<2e-6,"Physical ignores stored early/tail/doppler/decay overrides");
    setP(p,"mode",1);check(stage2Settle(p,{{2,.5,2}}),"Creative applies stored decay overrides through worker");
    const auto creative=stage2Impulse(p);
    check(stage2Error(creative,physical)>1e-4,"Creative restores audible overrides");
    // Independent Early/Tail click checks. Natural-step bound comes from an otherwise
    // identical moving-source control run; stored Doppler deliberately differs from 1.
    { // Isolate the moving Doppler history from the static round-trip fixture below.
    juce::MemoryBlock movingState;p.getStateInformation(movingState);
    VerbProcessor moving;moving.setPlayConfigDetails(2,2,48000,256);moving.prepareToPlay(48000,256);
    moving.setStateInformation(movingState.getData(),(int)movingState.getSize());check(waitReady(moving,90),"moving isolated-mode processor loads");
    auto& p=moving;
    juce::AudioBuffer<float> block(2,256);juce::MidiBuffer midi;
    const double sourceX=p.apvts.getRawParameterValue("s1x")->load();
    for(const char* solo:{"soloEarly","soloTail"}) {
        setP(p,solo,1);setP(p,"doppler",.35);
        auto sweep=[&](int initialMode,bool switchMode) {
            setP(p,"mode",initialMode);setP(p,"s1x",sourceX);
            check(stage2Settle(p,initialMode?std::array<double,3>{{2,.5,2}}:std::array<double,3>{{1,1,1}}),"mode click control tail settles before capture");
            for(int k=0;k<4000;++k){block.clear();stage2Process(p,block,midi);}
            // Measure natural steps of an established sine response, rather than
            // an under-filled first second that could hide the steady tail level.
            for(int k=-2000;k<0;++k){
                block.clear();for(int i=0;i<256;++i)block.setSample(0,i,.1f*(float)std::sin(2*verb::kPi*220*(k*256+i)/48000.));
                stage2Process(p,block,midi);
            }
            double step=0.;float last=0;
            int switchPhase=0;bool physicalConsumed=false,creativeConsumed=false;
            const auto until=juce::Time::getMillisecondCounterHiRes()+120000;
            for(int k=0;k<1800||(switchMode&&!creativeConsumed);++k) {
                if(juce::Time::getMillisecondCounterHiRes()>until)break;
                if(switchMode&&k==60){setP(p,"mode",0);switchPhase=1;}
                if(switchMode&&switchPhase==1&&k>=120) {
                    const auto d=p.getTailDiagnostics();
                    if(p.isTailCurrent()&&d&&d->multipliers==std::array<double,3>{{1,1,1}}){physicalConsumed=true;setP(p,"mode",1);switchPhase=2;}
                }
                // Bounded continuous movement also during a slow worker refresh.
                setP(p,"s1x",sourceX+.08*std::sin(k*256/48000.));
                block.clear();for(int i=0;i<256;++i)block.setSample(0,i,.1f*(float)std::sin(2*verb::kPi*220*(k*256+i)/48000.));
                stage2Process(p,block,midi);
                for(int i=0;i<256;++i){const float x=block.getSample(0,i);if(k>20)step=std::max(step,(double)std::abs(x-last));last=x;}
                // Controls and switches get the same wall-time opportunity for
                // position-triggered worker updates over the full bounded path.
                juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
                if(switchMode) {
                    const auto d=p.getTailDiagnostics();
                    if(switchPhase==2&&p.isTailCurrent()&&d&&d->multipliers==std::array<double,3>{{2,.5,2}})creativeConsumed=true;
                }
            }
            if(switchMode)check(physicalConsumed&&creativeConsumed,"isolated moving mode test consumes both new worker IR crossfades");
            return step;
        };
        // Physical can be louder than Creative, so include a Physical control baseline.
        const double creativeStep=sweep(1,false);
        const double physicalStep=sweep(0,false);
        // Until a new IR is ready, the existing Creative field also plays through
        // Physical's 0 dB level. Include this valid intermediate field in the control.
        setP(p,"early",0);setP(p,"tail",0);const double hybridStep=sweep(1,false);
        setP(p,"early",-24);setP(p,"tail",-12);
        const double switched=sweep(1,true),natural=std::max({creativeStep,physicalStep,hybridStep});
        std::printf("  %s moving mode steps: natural %.8g switched %.8g limit %.8g FS\n",solo,natural,switched,natural*1.25+1e-6);
        check(switched<natural*1.25+1e-6,"moving isolated mode switch within 1.25 times measured natural step");
        setP(p,solo,0);
    }
    }
    check(stage2Settle(p,{{2,.5,2}}),"Creative tail ready after switching");
    const double restoredError=stage2Error(creative,stage2Impulse(p));std::printf("  Restored Creative impulse error %.9g\n",restoredError);
    check(restoredError<2e-6,"returning to Creative restores the original override impulse");
    setP(p,"absorption",1.6);
    // Force a complete rebuild so the saved/loaded comparison starts from the same physical trace.
    juce::MemoryBlock saved;p.getStateInformation(saved);p.setStateInformation(saved.getData(),(int)saved.getSize());
    check(waitReady(p,90),"stage2 absorption state rebuilds");
    const auto before=stage2Impulse(p);p.getStateInformation(saved);const auto preset=p.getRoomPresetText();
    VerbProcessor q;q.setPlayConfigDetails(2,2,48000,256);q.prepareToPlay(48000,256);q.setStateInformation(saved.getData(),(int)saved.getSize());
    check(waitReady(q,90),"stage2 state reloads");
    error=stage2Error(before,stage2Impulse(q));std::printf("  Stage2 state IR error %.9g\n",error);
    check(error<2e-6,"state round-trip preserves rendered impulse");
    check(q.loadRoomPresetText(preset,message)&&waitReady(q,90),"stage2 portable preset reloads");
    error=stage2Error(before,stage2Impulse(q));std::printf("  Stage2 preset IR error %.9g\n",error);
    check(error<2e-6,"portable preset round-trip preserves rendered impulse");
    for(const char* id:{"mode","absorption","decayLow","decayMid","decayHigh"})check(std::abs(p.apvts.getRawParameterValue(id)->load()-q.apvts.getRawParameterValue(id)->load())<1e-6,"stage2 parameter round-trip");
    auto xml=juce::parseXML(preset);auto* payload=xml->getChildByName("VerbState");
    payload->removeChildElement(payload->getChildByAttribute("id","mode"),true);
    setP(q,"mode",0);check(q.loadRoomPresetText(xml->toString(),message)&&waitReady(q,90)&&q.apvts.getRawParameterValue("mode")->load()==1,"preset without mode always switches to Creative");
    payload->removeChildElement(payload->getChildByAttribute("id","early"),true);
    check(q.loadRoomPresetText(xml->toString(),message)&&waitReady(q,90)
        &&q.apvts.getRawParameterValue("early")->load()==q.apvts.getParameter("early")->convertFrom0to1(q.apvts.getParameter("early")->convertTo0to1(-9.f)),
        "legacy portable preset missing mode and early restores exact old -9 dB default");
    std::unique_ptr<juce::AudioProcessorEditor> editor(q.createEditor());auto* ed=dynamic_cast<VerbEditor*>(editor.get());
    for(int width:{900,1160}) {
        ed->setSize(width,width==900?600:760);ed->showPage(2,-1);setP(q,"mode",0);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
        bool bounds=true,disabled=false;
        for(auto* child:ed->getChildren())if(child->isVisible()&&child->getX()>width-350) {
            bounds &= ed->getLocalBounds().contains(child->getBounds());
            if(auto* slider=dynamic_cast<juce::Slider*>(child))if(slider->getTooltip().contains("Creative only"))disabled|=!slider->isEnabled();
        }
        check(bounds&&disabled,"Mix controls fit and Creative controls disable at both editor sizes");
        auto output=juce::File::getCurrentWorkingDirectory().getChildFile("out/stage2");output.createDirectory();
        juce::FileOutputStream image(output.getChildFile("mix-"+juce::String(width)+".png"));juce::PNGImageFormat png;
        if(image.openedOk()){image.setPosition(0);image.truncate();png.writeImageToStream(ed->createComponentSnapshot(ed->getLocalBounds()),image);}

    }
    // Assistant: a box and a larger built-in hall, using targets generated independently by a physical trace.
    for(int sceneIndex=0;sceneIndex<2;++sceneIndex) {
        verb::Scene scene;
        if(sceneIndex==0)scene=verb::loadScene("scenes/demo_box.txt");
        else { for(const auto& room:verbp::builtinRooms())if(room.name=="Concert hall (audience zone)"){std::istringstream in(room.text);scene=verb::parseScene(in,"hall");break;} }
        setP(q,"absorption",1);setP(q,"decayLow",1);setP(q,"decayMid",1);setP(q,"decayHigh",1);
        scene.sources.resize(1);q.loadSceneText(verb::serializeScene(scene),"target scene",true);check(waitReady(q,120),"target scene loads");
        setP(q,"mode",0);
        auto probe=verb::withAbsorptionScale(q.getDisplayScene(),1.4);probe=verb::withClutter(verb::withRoughness(probe),128);probe.length=20.;
        verb::Transducer receiver;receiver.pos={q.apvts.getRawParameterValue("lx")->load(),q.apvts.getRawParameterValue("ly")->load(),q.apvts.getRawParameterValue("lz")->load()};probe.receivers={receiver};
        const auto diag=verb::analyzeTail(verb::traceTail(probe,probe.sources[0],probe.receivers,1,48000,probe.seed));
        const double target=(diag.rt60[2]+diag.rt60[3]+diag.rt60[4])/3.;
        const double achieved=q.setTargetRT60(target);
        std::printf("  Target scene %d requested %.6f achieved %.6f scale %.3f state %d\n",sceneIndex,target,achieved,q.apvts.getRawParameterValue("absorption")->load(),q.getTargetRT60State());
        check(q.getTargetRT60State()==2&&std::abs(achieved/target-1)<.07,"RT60 assistant reaches independently traced target within 7%");
        juce::MemoryBlock targetState;q.getStateInformation(targetState);q.setStateInformation(targetState.getData(),(int)targetState.getSize());
        check(waitReady(q,120),"target absorption reloads as a physical room");
        const double live=q.getRT60Estimate()[1];
        std::printf("  Target scene %d live diagnostic %.6f (error %.3f%%)\n",sceneIndex,live,100*std::abs(live/target-1));
        check(std::abs(live/target-1)<.07,"live tail diagnostics confirm achieved RT60 within 7%");
        const auto targetIR=stage2Impulse(q);const auto targetPreset=q.getRoomPresetText();
        check(q.loadRoomPresetText(targetPreset,message)&&waitReady(q,120),"target room portable preset reloads");
        check(stage2Error(targetIR,stage2Impulse(q))<2e-6,"target room reload impulse matches, including Physical mode");
        const double closest=q.setTargetRT60(100.);
        check(q.getTargetRT60State()==3&&closest>0&&closest<100.,"unreachable target explicitly reported with closest achieved RT60");
    }
    { // Furniture placement must stay frozen when a source is moved before targeting RT60.
        auto scene=verb::loadScene("scenes/demo_furnished.txt");scene.sources.resize(1);scene.clutter.count=8;scene.clutter.seed=7;scene.roughness=.3;
        setP(q,"absorption",1);setP(q,"decayLow",1);setP(q,"decayMid",1);setP(q,"decayHigh",1);
        q.loadSceneText(verb::serializeScene(scene),"furnished target",true);check(waitReady(q,120),"furnished target scene loads");setP(q,"mode",0);
        auto model=q.getEnergyScene();bool moved=false;
        for(const auto& wall:model.walls)if(model.mats[(size_t)wall.mat].name=="Clutter"){
            const double length=std::hypot(wall.bx-wall.ax,wall.by-wall.ay);
            setP(q,"s1x",(wall.ax+wall.bx)/2-.15*(wall.by-wall.ay)/length);
            setP(q,"s1y",(wall.ay+wall.by)/2+.15*(wall.bx-wall.ax)/length);
            setP(q,"s1z",std::max(.2,wall.z1*.5));moved=true;break;
        }
        check(moved&&stage2Settle(q,{{1,1,1}}),"live source moves next to existing furniture");
        auto probe=verb::withAbsorptionScale(q.getEnergyScene(0),1.3);probe.length=20.;
        verb::Transducer receiver;receiver.pos={q.apvts.getRawParameterValue("lx")->load(),q.apvts.getRawParameterValue("ly")->load(),q.apvts.getRawParameterValue("lz")->load()};probe.receivers={receiver};
        const auto reference=verb::analyzeTail(verb::traceTail(probe,probe.sources[0],probe.receivers,1,48000,probe.seed));
        const double target=(reference.rt60[2]+reference.rt60[3]+reference.rt60[4])/3.;
        const double achieved=q.setTargetRT60(target);
        std::printf("  Furnished moving-source target %.6f achieved %.6f\n",target,achieved);
        check(q.getTargetRT60State()==2&&std::abs(achieved/target-1)<.07,"assistant targets the actual frozen clutter geometry after source motion");
        check(stage2Settle(q,{{1,1,1}}),"furnished absorption reaches the audio convolver");
        const auto after=q.getEnergyScene();bool same=after.walls.size()==model.walls.size();
        for(size_t i=0;i<std::min(model.walls.size(),after.walls.size());++i)
            same&=model.walls[i].ax==after.walls[i].ax&&model.walls[i].ay==after.walls[i].ay&&model.walls[i].bx==after.walls[i].bx&&model.walls[i].by==after.walls[i].by;
        check(same,"RT60 targeting never relocates existing furniture");
    }
    q.startTargetRT60(1.);q.cancelTargetRT60();
    const auto until=juce::Time::getMillisecondCounterHiRes()+10000;
    while(q.getTargetRT60State()==1&&juce::Time::getMillisecondCounterHiRes()<until)juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    check(q.getTargetRT60State()==4,"assistant worker can be cancelled");
    setP(q,"absorption",.6);setP(q,"decayLow",2);
    q.setStateInformation(state.getData(),(int)state.getSize());
    check(waitReady(q,90)&&q.apvts.getRawParameterValue("mode")->load()==1
        &&q.apvts.getRawParameterValue("absorption")->load()==1&&q.apvts.getRawParameterValue("decayLow")->load()==1,
        "legacy state clears a previous stage2 room's absorption and decay overrides");
    setP(q,"mode",0);q.loadSceneText("wall 0 0 1 1 missing_material","bad mode import",true);
    const auto badUntil=juce::Time::getMillisecondCounterHiRes()+10000;
    while(q.getLoadState()==1&&juce::Time::getMillisecondCounterHiRes()<badUntil)juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    check(q.getLoadState()==3&&q.apvts.getRawParameterValue("mode")->load()==0,"invalid .txt never activates stored Creative overrides");
    q.loadSceneText(q.getRoomText(),"legacy without position defaults.txt",false);
    check(waitReady(q,90)&&q.apvts.getRawParameterValue("mode")->load()==0,"valid .txt preserves Physical when position defaults are retained");
    std::printf("  Audio callback C++ allocations: %zu\n",stage2AudioAllocations);
    check(stage2AudioAllocations==0,"processBlock / engine process allocate no C++ memory during renders and mode switches");
    stage2ExportAndLoader(q);
    return fails?1:0;
}
