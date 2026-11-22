#pragma once
// Test/reference only (E2): the E1 granular TailHold, kept verbatim so that tests and the
// listening renderer can compare the new TailFreeze with it on identical input. Not used by the engine.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
namespace verb { namespace legacy {
// Prepared on the loader. No resize, heap, locks, RNG library or FFT in the audio path.
// Four long, randomly relocated, overlapping grains read a stationary band-normalised
// history. While held the history is immutable; there is no fixed repeating loop. Identical grain
// positions in all channels retain inter-channel relationships.
class TailHold {
    static constexpr int bands=7, heads=4;
    struct Channel {
        std::vector<float> history;
        std::array<double,6> lp{};
        std::array<double,7> power{},fastPower{},holdPower{};
        std::array<float,7> inv{}, level{}, release{},holdGain{},holdTarget{},oldLevel{},oldGain{};
    };
    std::vector<Channel> channels;
    std::vector<float> window;
    std::array<double,6> alpha{};
    std::array<int,heads> read{}, age{},oldRead{},oldAge{};
    std::array<float,heads> sign{{1,1,1,1}},oldSign{};
    int oldGrain=0;bool bridge=false;float engageLive=1;
    std::array<float,7> decay{};
    int length=0, grain=0, baseGrain=0, cursor=0, filled=0, activeSamples=0, available=0, captureEnd=0;
    uint64_t phase=0;
    uint32_t rng=0x31b973a5;
    double sr=48000, envA=0,fastA=0,holdA=0,gainA=0;
    float blend=0, blendStep=0, live=1, releaseBlend=0;
    bool on=false, releasing=false;
    uint32_t random() {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
    void relocate(int h) {
        const int span=std::max(1,available-grain);
        read[h]=(captureEnd-available+(int)(random()%(uint32_t)span)+length)%length;
        sign[h]=(random()&1)?1.f:-1.f;
    }
    void record(Channel& c,float x) {
        double previous=0;
        for(int b=0;b<bands;++b) {
            double v;
            if(b<6){c.lp[b]+=alpha[b]*(x-c.lp[b]);v=c.lp[b]-previous;previous=c.lp[b];}else v=x-previous;
            c.power[b]+=envA*(v*v-c.power[b]);c.fastPower[b]+=fastA*(v*v-c.fastPower[b]);
            c.history[(size_t)cursor*bands+b]=(float)std::clamp(v*c.inv[b],-8.,8.);
        }
    }
public:
    void prepare(double rate,int count) {
        sr=rate;length=(int)std::ceil(2*sr);grain=(int)std::round(.8*sr);grain-=grain%4;baseGrain=grain;
        channels.resize((size_t)count);
        for(auto& c:channels) {c.history.assign((size_t)length*bands,0);c.inv.fill(1);}
        window.resize((size_t)grain);
        // Four sine windows: sum(window^2)=1, including at each grain boundary.
        for(int i=0;i<grain;++i)window[i]=(float)(std::sin(3.141592653589793*(i+.5)/grain)/std::sqrt(2.));
        for(int b=0;b<6;++b)alpha[b]=1-std::exp(-2*3.141592653589793*125*std::pow(2.,b)*std::sqrt(2.)/sr);
        envA=1-std::exp(-1/(.125*sr));fastA=1-std::exp(-1/(.02*sr));holdA=1-std::exp(-1/(.05*sr));gainA=1-std::exp(-1/(.01*sr));blendStep=(float)(1/(.02*sr));setRelease({{2,2,2,2,2,2,2}});
    }
    void setRelease(const std::array<double,7>& rt) {
        for(int b=0;b<bands;++b)decay[b]=(float)std::pow(10.,-3/(std::clamp(rt[b],.1,20.)*sr));
    }
    void begin(bool enabled) {
        if(enabled!=on) {
            const bool wasReleasing=releasing;
            on=enabled;
            if(on) {
                bridge=wasReleasing;engageLive=wasReleasing?live:1;blend=0;
                if(bridge){oldRead=read;oldAge=age;oldSign=sign;oldGrain=grain;for(auto& c:channels)for(int b=0;b<bands;++b){c.oldLevel[b]=c.level[b]*c.release[b]*releaseBlend;c.oldGain[b]=c.holdGain[b];}}
                available=std::min(filled,activeSamples);grain=std::max(4,std::min(baseGrain,(int)(available*.6)/4*4));captureEnd=cursor;releasing=false;
                for(auto& c:channels)for(int b=0;b<bands;++b){c.level[b]=(float)std::sqrt(c.fastPower[b]);c.release[b]=1;c.holdPower[b]=1;c.holdGain[b]=c.holdTarget[b]=1;}
                for(int h=0;h<heads;++h){relocate(h);age[h]=h*grain/heads;read[h]=(read[h]+age[h])%length;}
            } else {releasing=available>=grain;releaseBlend=blend;}
        }
        blend=on?std::min(1.f,blend+blendStep):std::max(0.f,blend-blendStep);
        live=1-blend;
        if((phase++&127)==0&&!on)for(auto& c:channels)for(int b=0;b<bands;++b)c.inv[b]=(float)(1/std::sqrt(std::max(1e-20,c.power[b])));
    }
    float process(int channel,float x) {
        auto& c=channels[(size_t)channel];
        if(!on&&!releasing){record(c,x);return x;}
        if(available<grain) return x*live;
        float frozen=0;
        for(int b=0;b<bands;++b) {
            float v=0;for(int h=0;h<heads;++h)v+=sign[h]*window[(size_t)age[h]*baseGrain/grain]*c.history[(size_t)read[h]*bands+b];
            // The overlap of coherent/tonal grains is not constant-power even
            // though the windows are. A bounded envelope normaliser holds each
            // captured band level, with smoothed gains and no history writes while held.
            c.holdPower[b]+=holdA*(v*v-c.holdPower[b]);
            if((phase&127)==0)c.holdTarget[b]=(float)std::clamp(1/std::sqrt(std::max(1e-8,c.holdPower[b])),.25,4.);
            c.holdGain[b]+=(float)gainA*(c.holdTarget[b]-c.holdGain[b]);
            frozen+=v*c.holdGain[b]*c.level[b]*c.release[b];
            if(!on)c.release[b]*=decay[b];
        }
        // On release the held layer decays with the room's octave RT60; live tail
        // input resumes independently. Blend avoids a step at both transitions.
        if(on) {
            float previousHold=0;
            if(bridge&&blend<1)for(int b=0;b<bands;++b){float v=0;for(int h=0;h<heads;++h)if(oldAge[h]<oldGrain)v+=oldSign[h]*window[(size_t)oldAge[h]*baseGrain/oldGrain]*c.history[(size_t)oldRead[h]*bands+b];previousHold+=v*c.oldGain[b]*c.oldLevel[b];c.oldLevel[b]*=decay[b];}
            return (x*engageLive+previousHold)*(1-blend)+frozen*blend;
        }
        const float result=x*live+frozen*releaseBlend;
        // While released, record the actually audible tail for a fresh capture.
        // On has immutable history, so new input never enters a held sound.
        record(c,result);return result;
    }
    void end() {
        if(bridge&&blend<1)for(int h=0;h<heads;++h){oldRead[h]=(oldRead[h]+1)%length;++oldAge[h];}
        if(!on){cursor=(cursor+1)%length;filled=std::min(filled+1,length);bool signal=false;for(const auto& c:channels)for(double p:c.power)signal|=p>1e-18;activeSamples=signal?std::min(activeSamples+1,length):0;}
        if(on||releasing)for(int h=0;h<heads;++h){read[h]=(read[h]+1)%length;if(++age[h]>=grain){age[h]=0;relocate(h);}}
        if(releasing){bool quiet=true;for(const auto& c:channels)for(float r:c.release)quiet&=r<1e-5f;if(quiet){releasing=false;}}
    }
    size_t memoryBytes() const {return (size_t)length*bands*channels.size()*sizeof(float)+window.size()*sizeof(float);}
};

} } // namespace verb::legacy
