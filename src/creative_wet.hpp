#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "freeze_network.hpp"

namespace verb {
// E2: the granular TailHold was replaced by TailFreeze (freeze_network.hpp).
class WetPredelay {
    std::vector<float> ring;
    int length=0,channels=0,pos=0,from=0,to=0,remaining=0,fade=1;
    double sr=48000;
public:
    void prepare(double rate,int count){sr=rate;channels=count;length=(int)std::ceil(.25*sr)+1;ring.assign((size_t)length*count,0);fade=std::max(1,(int)(.02*sr));}
    void begin(double ms){const int want=std::clamp((int)std::lround(ms*.001*sr),0,length-1);if(remaining==0&&want!=to){from=to;to=want;remaining=fade;}}
    float process(int ch,float wet){
        ring[(size_t)ch*length+pos]=wet;
        if(from==0&&to==0)return wet;
        const float a=ring[(size_t)ch*length+(pos-from+length)%length];
        const float b=ring[(size_t)ch*length+(pos-to+length)%length];
        return remaining? a+(b-a)*(1-(float)remaining/fade):b;
    }
    void end(){pos=(pos+1)%length;if(remaining&&--remaining==0)from=to;}
};
// Unit operator norm at width <= 100%; <= 2 at 200%. Headroom normalisation
// limits stereo pair RMS amplification to sqrt(2), with no limiter/nonlinearity.
inline void widthPair(float& l,float& r,float width) {
    if(width==1.f)return;
    const float m=(l+r)*.5f,s=(l-r)*.5f;
    const float gain=width>1?std::sqrt(2.f/(1.f+width*width)):1.f;
    l=gain*(m+width*s);r=gain*(m-width*s);
}
class WetDuck {
    double sr=48000,envelope=0,gainDb=0,attack=0,release=0,amountCurrent=0;
public:
    void prepare(double rate){sr=rate;attack=1-std::exp(-1/(.01*sr));}
    void settings(double ms){release=1-std::exp(-1/(std::clamp(ms,50.,2000.)*.001*sr));}
    float process(float detector,float amount){
        const double target=std::min(1.,std::abs((double)detector)/.1);
        envelope+=(target>envelope?attack:release)*(target-envelope);
        const double step=24/(.02*sr);
        amountCurrent=std::abs(amount-amountCurrent)<=step?amount:amountCurrent+(amount>amountCurrent?step:-step);
        gainDb=-amountCurrent*envelope;
        if(amountCurrent==0){gainDb=0;return 1;}
        return (float)std::pow(10.,gainDb/20);
    }
    float reductionDb() const {return (float)-gainDb;}
};
} // namespace verb

