#include "rt_engine.hpp"
#include "geometry.hpp"
#include "../plugin/RoomLibrary.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <numeric>

using namespace verb;
static int failures=0, passes=0;
static void check(bool ok,const char* label) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);ok?++passes:++failures;
}
static Scene scene(const verbp::BuiltinRoom& room,bool fullSetup=false) {
    std::istringstream in(room.text);auto s=parseScene(in,room.name);
    s.sr=48000;if(s.receivers.size()>2)s.receivers.resize(2);
    if(!fullSetup){s.sources.resize(1);return s;}
    auto setup=room.setup;
    if(setup.empty())for(size_t i=0;i<s.sources.size();++i){verbp::SourceCfg c;c.firstIn=(int)i;c.name=s.sources[i].name;setup.push_back(c);}
    if(setup.size()!=s.sources.size())throw std::runtime_error("builtin setup/source count mismatch");
    const auto points=verbp::buildPoints(setup);const auto authored=s.sources;s.sources.clear();
    bool budget=setup.size()<=verbp::kMaxSources&&points.size()<=verbp::kMaxPoints;
    for(const auto& c:setup)budget&=c.firstIn>=0&&c.firstIn+c.effectiveChannels()<=verbp::kMaxInputChannels;
    check(budget,"complete builtin setup fits source/point/input budgets");
    for(const auto& pm:points) {
        const auto& c=setup[(size_t)pm.source];const auto& center=authored[(size_t)pm.source];
        const double az=center.azimuthDeg*kPi/180.;double x=0,y=0,z=0;
        if(c.freePoints&&pm.channel>=0&&pm.channel<(int)c.ox.size()&&pm.channel<(int)c.oy.size()) {
            x=c.ox[(size_t)pm.channel];y=c.oy[(size_t)pm.channel];if(pm.channel<(int)c.oz.size())z=c.oz[(size_t)pm.channel];
        } else {x=pm.radius*std::cos(pm.angleDeg*kPi/180.);y=pm.radius*std::sin(pm.angleDeg*kPi/180.);}
        Transducer t;t.name=c.name;t.azimuthDeg=center.azimuthDeg;t.pattern=c.pattern;
        t.pos={center.pos.x+std::cos(az)*x-std::sin(az)*y,center.pos.y+std::sin(az)*x+std::cos(az)*y,
            std::clamp(center.pos.z+z,0.,s.ceilMat<0?30.:s.height)};s.sources.push_back(t);
    }
    if(room.micPattern>=0)applyMicPattern(s.receivers,(MicPattern)room.micPattern,room.micPattern==(int)MicPattern::HumanEars?.0875:.1);
    return s;
}
struct Result {double peak=0;std::vector<double> sourcePeaks;bool finite=true;TailDiagnostics diag;};
static Result render(const Scene& s,std::array<double,3> m) {
    EngineConfig cfg;cfg.erOrder=1;cfg.tailRays=48000;cfg.tailThread=false;cfg.decayMultipliers=m;
    cfg.sharedTail=s.sources.size()>4;cfg.maxEarlyDelaySec=cfg.sharedTail?.4:.75;
    Engine e(s,cfg);e.setEarlyDiffusion(.5);
    std::vector<float> input(512),zero(512);std::vector<const float*> ip(s.sources.size(),zero.data());
    std::vector<std::vector<float>> output(s.receivers.size(),std::vector<float>(512));
    std::vector<float*> op;for(auto& ch:output)op.push_back(ch.data());
    for(int k=0;k<16;++k)e.process(ip.data(),op.data(),512);
    Result result;result.diag=*e.tailDiagnostics();
    const size_t n=(size_t)std::ceil((e.tailSeconds()+.05)*s.sr);
    std::vector<size_t> probes{0};if(s.sources.size()>1)probes.push_back(s.sources.size()-1);
    for(size_t source:probes) {
        std::fill(ip.begin(),ip.end(),zero.data());
        if(!result.sourcePeaks.empty())for(size_t pos=0;pos<n+1024;pos+=512)e.process(ip.data(),op.data(),512);
        ip[source]=input.data();double peak=0;
        for(size_t pos=0;pos<n;pos+=512) {
            input[0]=pos==0?1.f:0.f;
            e.process(ip.data(),op.data(),(int)std::min<size_t>(512,n-pos));
            for(const auto& ch:output)for(float x:ch){result.finite&=std::isfinite(x);peak=std::max(peak,(double)std::abs(x));}
        }
        input[0]=0;result.sourcePeaks.push_back(peak);result.peak=std::max(result.peak,peak);
    }
    check(e.diffractionCapacityMisses()==0,"prepared diffraction scratch never loses capacity");
    return result;
}
static void extremes() {
    std::filesystem::create_directories("out/tail-fix");
    std::ofstream csv("out/tail-fix/decay-extremes.csv");
    csv<<"room,points,probes,group,multiplier,unity_peak,peak,worst_peak_ratio,finite,band,physical_rt60,result_rt60,effective_multiplier,status,error\n";
    double worst=0;
    for(const auto& room:verbp::builtinRooms()) {
        auto s=scene(room,true);const auto unity=render(s,{{1,1,1}});
        bool normal=room.name=="Bedroom"||room.name=="Living room"||room.name.find("Bathroom")!=std::string::npos
            ||room.name.find("Studio")!=std::string::npos||room.name=="Corridor"||room.name.find("hall")!=std::string::npos||room.name.find("VSL")!=std::string::npos;
        auto run=[&](int group,double multiplier) {
            std::array<double,3> m{{1,1,1}};m[group]=multiplier;const auto result=render(s,m);
            std::printf("%s points %zu probes %zu group %d x%.2f peak %.9g unity %.9g\n",room.name.c_str(),s.sources.size(),result.sourcePeaks.size(),group,multiplier,result.peak,unity.peak);
            // +1 dB full-IR peak margin includes summation/interference of the seven bands.
            bool bounded=result.finite;double peakRatio=0;
            for(size_t i=0;i<result.sourcePeaks.size();++i) {
                bounded&=result.sourcePeaks[i]<=unity.sourcePeaks[i]*std::pow(10.,1./20.)+1e-6;
                if(unity.sourcePeaks[i]>0)peakRatio=std::max(peakRatio,result.sourcePeaks[i]/unity.sourcePeaks[i]);
            }
            check(bounded,"complete setup first/last point IRs finite; each peak <= its unity +1 dB +1e-6 FS");
            for(int b=0;b<NB;++b)if((b<2?0:b<5?1:2)==group) {
                const auto& d=result.diag;
                const double target=d.physicalRT60[b]*d.effectiveMultipliers[b];
                const double error=target>0?std::abs(d.rt60[b]/target-1.):0;
                csv<<'"'<<room.name<<'"'<<','<<s.sources.size()<<','<<result.sourcePeaks.size()<<','<<group<<','<<multiplier<<','<<unity.peak<<','<<result.peak<<','<<peakRatio<<','<<result.finite<<','<<b<<','
                    <<d.physicalRT60[b]<<','<<d.rt60[b]<<','<<d.effectiveMultipliers[b]<<','<<d.reshapeStatus[b]<<','<<error<<'\n';csv.flush();
                if(normal&&d.reshapeStatus[b]==0){worst=std::max(worst,error);check(error<=.005,"normal-room T20 error <=0.5% for an applied decay");}
                check(d.reshapeStatus[b]>=0&&d.reshapeStatus[b]<=4&&std::isfinite(d.effectiveMultipliers[b]),"every band has finite effective multiplier and explicit reshape status");
            }
        };
        for(int g=0;g<3;++g)for(double m:{.3,3.})run(g,m);
        if(room.name.find("Vocal booth")!=std::string::npos)for(double m:{1.8,1.9,2.})run(1,m);
        // Cathedral low x3 and MotorSynth mid x3 are already part of the exhaustive matrix.
    }
    std::printf("Worst applied normal-room T20 error %.6f%%\n",worst*100);
}
static void lengthAndCancellation() {
    for(const auto& room:verbp::builtinRooms())if(room.name=="Bedroom"||room.name.find("MotorSynth vehicle")!=std::string::npos) {
        auto s=scene(room);std::vector<double> lengths;
        for(double scale:{.99,1.,1.01}) {
            EngineConfig c;c.erOrder=1;c.tailThread=false;c.tailRays=0;c.absorptionScale=scale;
            Engine e(s,c);lengths.push_back(e.tailSeconds());
        }
        std::printf("Length %s .99/1/1.01 %.6f %.6f %.6f\n",room.name.c_str(),lengths[0],lengths[1],lengths[2]);
        check(std::abs(lengths[0]/lengths[1]-1)<.12&&std::abs(lengths[2]/lengths[1]-1)<.12,"authored-scene length continuous around absorption unity (<12% for 1% scale step)");
        EngineConfig c;c.erOrder=1;c.tailRays=0;c.tailThread=false;c.absorptionScale=1.;Engine e(s,c);
        const auto start=std::chrono::steady_clock::now();
        check(e.setScene(s,1.),"cached scene update accepted");
        const double cached=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        check(cached<.5,"repeated scene/absorption length probe is cached (<0.5 s, excluding unrelated IR synthesis)");
        auto edited=s;edited.humidity+=.314159;
        const auto begin=std::chrono::steady_clock::now();bool cancelled=false;
        try {e.setScene(edited,.973,[&]{return std::chrono::steady_clock::now()-begin>std::chrono::milliseconds(30);});}
        catch(const std::runtime_error&){cancelled=true;}
        const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        std::printf("Probe cancellation %.6f s; cached construction %.6f s\n",elapsed,cached);
        check(cancelled&&elapsed<.25,"in-flight geometry length trace cancels within 250 ms");
        check(e.setScene(s,.7)&&e.setScene(s),"setScene overload retains current absorption");
        const auto model=e.sceneSnapshot();auto reference=withAbsorptionScale(s,.7);
        bool same=model->mats.size()==reference.mats.size();
        for(size_t i=0;i<std::min(model->mats.size(),reference.mats.size());++i)for(int b=0;b<NB;++b)same&=model->mats[i].alpha[b]==reference.mats[i].alpha[b];
        check(same,"geometry-only setScene does not reset absorption");
    }
    auto s=scene(verbp::builtinRooms()[1]);Geo g(s);DiffractionScratch scratch;DiffPath path;
    diffractedPath(g,s.sources[0].pos,s.receivers[0].pos,speedOfSound(s.tempC),path,false,&scratch);
    check(scratch.capacityMisses==1,"undersized external diffraction scratch reports capacity miss");
    TailHist h;h.binDur=.001;h.nBins=5;h.h.resize(1);h.reliableEnd.resize(1);h.reliableEnd[0].fill(5);
    for(auto& band:h.h[0])band={0,1.,.0001,0,0};
    auto before=h.h;reshapeTailDecay(h,{{.3,.3,.3}});
    check(h.h==before&&std::all_of(h.reshapeStatus.begin(),h.reshapeStatus.end(),[](int v){return v==1||v==2;}),"unmeasurable/sub-bin decay stays unity with diagnostics");
    auto silent=scene(verbp::builtinRooms().front());silent.sources.push_back(silent.sources.front());
    EngineConfig config;config.erOrder=1;config.tailThread=false;config.tailRays=0;config.decayMultipliers={{3,3,3}};
    Engine engine(silent,config);const auto diagnostic=engine.tailDiagnostics();
    check(diagnostic&&diagnostic->sourceReshapeStatus.size()==2&&diagnostic->sourceEffectiveMultipliers.size()==2
        &&diagnostic->sourceReshapeStatus[0][3]==1&&diagnostic->sourceReshapeStatus[1][3]==1,
        "unmeasurable independent bands report the status of every source, not just source zero");
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {if(argc<2||std::string(argv[1])!="length")extremes();lengthAndCancellation();}
    catch(const std::exception& e){std::printf("Exception: %s\n",e.what());++failures;}
    std::printf("Stage2 fixes: %d PASS / %d FAIL\n",passes,failures);return failures?1:0;
}
