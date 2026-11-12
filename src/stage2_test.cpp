#include "rt_engine.hpp"
#include "dsp.hpp"
#include "../plugin/RoomLibrary.h"
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace verb;
static int fails=0;
static void check(bool ok,const char* label){std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);if(!ok)++fails;}

// Independent Schroeder/T20 fit, using sample energies (no model diagnostics).
static double t20(const std::vector<double>& samples,double sr,size_t start=0){
    std::vector<double> e(samples.size()+1);
    for(size_t k=samples.size();k-->start;)e[k]=e[k+1]+samples[k]*samples[k];
    double sx=0,sy=0,sxx=0,sxy=0,n=0;
    if(e[start]<=0)return -1;
    for(size_t k=start;k<samples.size();++k){
        const double db=10*std::log10(std::max(1e-300,e[k]/e[start]));
        if(db< -25)break;if(db> -5)continue;
        const double t=k/sr;sx+=t;sy+=db;sxx+=t*t;sxy+=t*db;++n;
    }
    return n>4 ? -60*(n*sxx-sx*sx)/(n*sxy-sx*sy) : -1;
}

static std::array<double,NB> measure(const Wav& wav){
    const size_t n=nextPow2(wav.ch[0].size()*2);
    std::vector<std::vector<cd>> spectra(wav.ch.size(),std::vector<cd>(n));
    for(size_t r=0;r<wav.ch.size();++r) {
        for(size_t i=0;i<wav.ch[r].size();++i)spectra[r][i]=wav.ch[r][i];
        fft(spectra[r],false);
    }
    std::array<double,NB> result{};
    for(int b=0;b<NB;++b){
        std::vector<double> energy(wav.ch[0].size());
        const double low=kBandHz[b]/std::sqrt(2.),high=kBandHz[b]*std::sqrt(2.);
        for(const auto& spectrum:spectra) {
            auto filtered=spectrum;
            for(size_t k=0;k<n;++k){
                const double hz=(k<=n/2?k:n-k)*(double)wav.sr/n;
                const double w=std::clamp(std::min((hz-low)/(.05*low),(high-hz)/(.05*high)),0.,1.);
                filtered[k]*=w;
            }
            fft(filtered,true);
            for(size_t i=0;i<energy.size();++i)energy[i]+=std::norm(filtered[i]);
        }
        for(auto& e:energy)e=std::sqrt(e); // pool receiver energies, matching diagnostic semantics
        result[b]=t20(energy,wav.sr);
    }
    return result;
}

static Wav render(Scene scene,std::array<double,3> multipliers={{1,1,1}},bool shared=false,bool tailOnly=true){
    EngineConfig c;c.erOrder=1;c.tailThread=false;c.tailRays=48000;c.sharedTail=shared;c.decayMultipliers=multipliers;
    Engine e(scene,c);if(tailOnly)e.setSoloMask(4);else e.setEarlyDiffusion(.5);
    constexpr int block=512;
    std::vector<std::vector<float>> in(scene.sources.size(),std::vector<float>(block)),scratch(scene.receivers.size(),std::vector<float>(block));
    std::vector<const float*> ip;for(const auto& ch:in)ip.push_back(ch.data());
    std::vector<float*> op;for(auto& ch:scratch)op.push_back(ch.data());
    for(int k=0;k<8;++k)e.process(ip.data(),op.data(),block);
    Wav out;out.sr=(int)scene.sr;out.ch.resize(scene.receivers.size());
    const size_t total=(size_t)std::ceil((e.tailSeconds()+.1)*scene.sr);
    for(auto& ch:out.ch)ch.resize(total);
    for(size_t pos=0;pos<total;pos+=block){
        const int count=(int)std::min<size_t>(block,total-pos);in[0][0]=pos==0?1.f:0.f;
        e.process(ip.data(),op.data(),count);
        for(size_t r=0;r<op.size();++r)std::copy_n(op[r],count,out.ch[r].data()+pos);
    }
    auto d=e.tailDiagnostics();check(d&&d->multipliers==multipliers&&d->physicalRT60[3]>0,"diagnostics expose result, physical RT60 and applied multipliers");
    return out;
}

static Scene hall(){
    for(const auto& room:verbp::builtinRooms())if(room.name=="Concert hall (audience zone)"){
        std::istringstream in(room.text);auto s=parseScene(in,"stage2 hall");s.sources.resize(1);return s;
    }
    throw std::runtime_error("hall missing");
}

// LEGACY_MOTION_BEGIN -- also compiled against the untouched archived engine to capture the fixture.
static std::vector<float> legacyMotionRender(int chunk=128,int mask=0){
    auto s=loadScene("scenes/demo_box.txt");s.sources.resize(1);
    EngineConfig config;config.erOrder=2;config.tailRays=4000;config.tailThread=false;
    Engine e(s,config);e.setSoloMask(mask);e.setEarlyLevel(-9);e.setTailLevel(-3);e.setEarlyDiffusion(.5);e.setDopplerStrength(.7);
    std::vector<float> input(128),left(128),right(128),result(144000*2);
    const float* ip[]={input.data()};float* op[]={left.data(),right.data()};
    for(int k=0;k<8;++k)e.process(ip,op,128);
    for(int pos=0;pos<144000;pos+=128){
        const double t=pos/48000.;
        e.setSourcePosition(0,2+.6*std::sin(t),3+.5*(std::cos(t)-1),1.5+.05*std::sin(2*t));
        e.setListener(6-.3*std::sin(.4*t),3+.2*std::sin(.7*t),1.5,20*t);
        const int n=std::min(128,144000-pos);
        for(int i=0;i<n;++i)input[i]=(float)(.1*std::sin(2*kPi*440*(pos+i)/48000.)+.05*std::sin(2*kPi*881*(pos+i)/48000.));
        for(int offset=0;offset<n;offset+=chunk){
            const float* partIn[]={input.data()+offset};float* partOut[]={left.data()+offset,right.data()+offset};
            e.process(partIn,partOut,std::min(chunk,n-offset));
        }
        for(int i=0;i<n;++i){result[(pos+i)*2]=left[i];result[(pos+i)*2+1]=right[i];}
    }
    return result;
}
// LEGACY_MOTION_END

int main(){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    {
        std::ifstream file("plugin/fixtures/stage2/legacy-motion.f32",std::ios::binary);
        const auto actual=legacyMotionRender();std::vector<float> expected(actual.size());
        file.read(reinterpret_cast<char*>(expected.data()),(std::streamsize)(expected.size()*sizeof(float)));
        double error=0;for(size_t i=0;i<actual.size();++i)error=std::max(error,(double)std::abs(actual[i]-expected[i]));
        std::printf("Legacy full moving waveform difference (intentional tail transition fix) %.9g\n",error);
        const auto directEarly=legacyMotionRender(128,3),tail=legacyMotionRender(128,4);
        std::vector<float> legacyDirectEarly(actual.size()),legacyTail(actual.size());
        std::ifstream directFile("plugin/fixtures/stage2/legacy-motion-direct-early.f32",std::ios::binary),tailFile("plugin/fixtures/stage2/legacy-motion-tail.f32",std::ios::binary);
        directFile.read((char*)legacyDirectEarly.data(),legacyDirectEarly.size()*sizeof(float));tailFile.read((char*)legacyTail.data(),legacyTail.size()*sizeof(float));
        double directError=0,oldSumError=0,newSumError=0;
        for(size_t i=0;i<actual.size();++i) {
            directError=std::max(directError,(double)std::abs(directEarly[i]-legacyDirectEarly[i]));
            oldSumError=std::max(oldSumError,(double)std::abs(expected[i]-legacyDirectEarly[i]-legacyTail[i]));
            newSumError=std::max(newSumError,(double)std::abs(actual[i]-directEarly[i]-tail[i]));
        }
        std::printf("Legacy moving Direct/Early error %.9g FS; fixture sum %.9g; new component sum %.9g\n",directError,oldSumError,newSumError);
        check(file.good()&&directFile.good()&&tailFile.good()&&directError<=2e-6,"moving Direct/Early Doppler/diffusion waveform remains within original 2e-6 tolerance of untouched 0.1.15");
        check(oldSumError<=2e-6&&newSumError<=2e-6,"isolated legacy fixtures and new components each reconstruct their complete waveform within 2e-6");
        const auto chopped=legacyMotionRender(37);double blockError=0;
        for(size_t i=0;i<actual.size();++i)blockError=std::max(blockError,(double)std::abs(actual[i]-chopped[i]));
        std::printf("Moving waveform block 128 vs 37 error %.9g\n",blockError);
        check(blockError<=2e-6,"moving interpolation remains independent of odd host block segmentation");
    }
    // Analytic energy is represented by deterministic sample amplitudes, including an extrapolated region.
    TailHist physical;physical.nBins=3001;physical.binDur=.001;physical.h.resize(1);physical.reliableEnd.resize(1);physical.reliableEnd[0].fill(300);
    for(int b=0;b<NB;++b){physical.h[0][b].resize(physical.nBins);for(size_t k=20;k<physical.nBins;++k)physical.h[0][b][k]=std::pow(10.,-6.*(k-20)*.001/(.8+.1*b));}
    for(int group=0;group<3;++group)for(double m:{.5,1.,2.}){
        auto h=physical;std::array<double,3> ms{{1,1,1}};ms[group]=m;reshapeTailDecay(h,ms);
        bool fit=true,other=true,onset=true;
        for(int b=0;b<NB;++b){
            std::vector<double> x(h.nBins);for(size_t k=0;k<h.nBins;++k)x[k]=std::sqrt(h.h[0][b][k]);
            const double actual=t20(x,1000.),expected=(.8+.1*b)*((b<2?0:b<5?1:2)==group?m:1.);
            if((b<2?0:b<5?1:2)==group)fit&=std::abs(actual/expected-1)<.1;else other&=std::abs(actual/expected-1)<.05;
            onset&=std::abs(10*std::log10(h.h[0][b][20]/physical.h[0][b][20]))<.5;
        }
        check(fit&&other&&onset,"analytic rendered amplitudes: decay factor +/-10%, other bands +/-5%, onset within 0.5 dB");
        if(m==2)check(h.nBins>physical.nBins,"boost extends beyond original tail/reliableEnd");
    }
    auto capped=physical;capped.nBins=20002;
    for(auto& rc:capped.h)for(auto& b:rc)b.resize(capped.nBins,1e-100);
    reshapeTailDecay(capped,{{3,3,3}});bool bounded=capped.nBins<=20000;
    for(const auto& rc:capped.h)for(const auto& b:rc)for(double e:b)bounded&=std::isfinite(e);
    check(bounded,"20 second input plus maximum boost stays bounded and finite");
    auto room=hall();
    const auto reference=render(room);const auto rt=measure(reference);
    for(int group=0;group<3;++group)for(double m:{.5,1.,2.}){
        std::array<double,3> ms{{1,1,1}};ms[group]=m;
        const auto wav=m==1?reference:render(room,ms);const auto actual=m==1?rt:measure(wav);
        bool fit=true,other=true;double worstFit=0,worstOther=0;
        for(int b=0;b<NB;++b){
            const bool affected=(b<2?0:b<5?1:2)==group;
            const double error=std::abs(actual[b]/rt[b]/(affected?m:1.)-1.);
            if(affected){fit&=error<=.10;worstFit=std::max(worstFit,error);}else{other&=error<=.05;worstOther=std::max(worstOther,error);}
        }
        std::printf("Group %d multiplier %.1f: affected error %.2f%%; other error %.2f%%; RT60",group,m,worstFit*100,worstOther*100);
        for(double x:actual)std::printf(" %.4f",x);std::printf("\n");
        size_t onset=0;while(onset<reference.ch[0].size()&&std::abs(reference.ch[0][onset])<1e-9)++onset;
        double e0=0,e1=0;
        for(size_t k=onset;k<std::min(reference.ch[0].size(),onset+(size_t)(.002*room.sr));++k)
            for(size_t r=0;r<wav.ch.size();++r){e0+=reference.ch[r][k]*reference.ch[r][k];e1+=wav.ch[r][k]*wav.ch[r][k];}
        const double onsetDb=10*std::log10(e1/e0);std::printf("  Rendered onset RMS difference %.3f dB\n",onsetDb);
        check(std::abs(onsetDb)<.5,"rendered tail onset level stays within 0.5 dB");
        check(fit,"rendered hall octave-band T20 follows requested multiplier within 10%");
        check(other,"rendered hall other octave-band T20 remains within 5%");
    }
    // All geometry and generated clutter must conserve alpha + tau under scaling.
    auto raw=loadScene("scenes/demo_furnished.txt");raw.roughness=.4;raw.clutter.count=5;
    const auto modeled=withClutter(withRoughness(raw),128),scaled=withAbsorptionScale(raw,3.);
    bool conserved=modeled.mats.size()==scaled.mats.size();
    for(size_t i=0;i<scaled.mats.size()&&i<modeled.mats.size();++i)for(int b=0;b<NB;++b)
        conserved&=scaled.mats[i].tau[b]==modeled.mats[i].tau[b]&&std::abs(scaled.mats[i].alpha[b]-std::min(1.-modeled.mats[i].tau[b],3.*modeled.mats[i].alpha[b]))<1e-12;
    check(conserved,"absorption includes roughened surfaces/generated clutter and preserves transmission");
    raw.sources[0].pos.x+=.31; // point geometry differs from the authored/default centre
    EngineConfig physicalConfig;physicalConfig.erOrder=1;physicalConfig.tailThread=false;physicalConfig.tailRays=1000;
    Engine scaleEngine(raw,physicalConfig);const auto unscaledGeometry=scaleEngine.sceneSnapshot();
    auto edit=raw;edit.sources[0].pos.x+=2.;
    check(scaleEngine.setScene(edit,.6),"live absorption scene update accepted");
    const auto scaledGeometry=scaleEngine.sceneSnapshot();bool fixed=unscaledGeometry->walls.size()==scaledGeometry->walls.size();
    for(size_t i=0;i<unscaledGeometry->walls.size()&&i<scaledGeometry->walls.size();++i){
        const auto& a=unscaledGeometry->walls[i];const auto& b=scaledGeometry->walls[i];
        fixed&=a.ax==b.ax&&a.ay==b.ay&&a.bx==b.bx&&a.by==b.by&&a.z0==b.z0&&a.z1==b.z1;
    }
    check(fixed,"absorption automation never regenerates clutter at changed source/default positions");
    // Shared field and regular field have matching onset scaling for a single source.
    auto small=loadScene("scenes/demo_box.txt");small.sources.resize(1);small.receivers.resize(1);small.sr=24000;
    auto a=render(small,{{2,1,1}}),b=render(small,{{2,1,1}},true);
    const auto ar=measure(a),br=measure(b);bool shared=true;
    for(int band=0;band<NB;++band)shared&=std::abs(ar[band]/br[band]-1)<.1;
    check(shared,"shared tail preserves creative decay shape");
    // Listening artifacts: full engine output, including direct/early, at 48 kHz.
    std::filesystem::create_directories("out/stage2");
    std::ofstream csv("out/stage2/rt60.csv");csv<<"variant,125,250,500,1000,2000,4000,8000\n";
    struct Variant{const char* name;double absorption;std::array<double,3> m;};
    for(const auto& v:std::vector<Variant>{{"physical-default",1.,{{1,1,1}}},{"absorption-0.6",.6,{{1,1,1}}},{"absorption-1.6",1.6,{{1,1,1}}},{"decayLow-2.0",1.,{{2,1,1}}},{"decayHigh-0.5",1.,{{1,1,.5}}}}){
        auto s=withAbsorptionScale(hall(),v.absorption);
        const auto wav=render(s,v.m,false,false);const auto measured=measure(wav);
        writeWav(std::string("out/stage2/")+v.name+".wav",wav);csv<<v.name;for(double rt:measured)csv<<","<<rt;csv<<"\n";
    }
    std::printf("Stage2 engine: %d failures\n",fails);return fails?1:0;
}
