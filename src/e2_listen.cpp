// E2 listening renderer (test tool, never part of the plugin).
// Renders the same synthetic material through the whole engine (direct + early + tail)
// with Freeze engaged at 3 s, held, new material during the hold, then released.
// Built once against the pre-E2 sources (granular Freeze, "A") and once against the
// E2 sources ("B"); the source file and timings are identical.
#include "rt_engine.hpp"
#include "layouts.hpp"
#include "../plugin/RoomLibrary.h"
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <vector>
using namespace verb;
static constexpr double SR=48000, PI=3.141592653589793;

static void writeWav24(const std::string& path,const std::vector<float>& l,const std::vector<float>& r){
    FILE* f=std::fopen(path.c_str(),"wb");if(!f){std::printf("cannot write %s\n",path.c_str());return;}
    const uint32_t n=(uint32_t)l.size(),data=n*2*3;auto u32=[&](uint32_t v){std::fwrite(&v,4,1,f);};auto u16=[&](uint16_t v){std::fwrite(&v,2,1,f);};
    std::fwrite("RIFF",1,4,f);u32(36+data);std::fwrite("WAVEfmt ",1,8,f);u32(16);u16(1);u16(2);u32((uint32_t)SR);u32((uint32_t)SR*6);u16(6);u16(24);std::fwrite("data",1,4,f);u32(data);
    for(uint32_t i=0;i<n;++i)for(float v:{l[i],r[i]}){const double c=std::max(-1.,std::min(1.,(double)v));const int32_t s=(int32_t)std::lround(c*8388607.);const uint8_t b[3]={(uint8_t)(s&255),(uint8_t)((s>>8)&255),(uint8_t)((s>>16)&255)};std::fwrite(b,1,3,f);}
    std::fclose(f);
}
// ---- synthetic material (all deterministic) ----
static double saw(double ph){ph-=std::floor(ph);return 2*ph-1;}
static void pad(std::vector<float>& y,double t0,double t1,double transpose,double amp){
    const double notes[]={110,130.81,164.81,220,261.63};double lp=0;std::mt19937 rng(5);std::uniform_real_distribution<double> u(0,1);
    double ph[5][3];for(auto& a:ph)for(auto& b:a)b=u(rng);
    for(size_t i=(size_t)(t0*SR);i<y.size()&&i<(size_t)(t1*SR+.06*SR);++i){const double t=i/SR-t0;double env=std::min(1.,t/.35)*std::min(1.,std::max(0.,(t1-t0-t)/.05+1));if(t>t1-t0)env=std::max(0.,1-(t-(t1-t0))/.05);
        double s=0;for(int n=0;n<5;++n)for(int d=0;d<3;++d){const double f=notes[n]*transpose*std::pow(2.,((d-1)*7+3*std::sin(2*PI*(4.8+.3*n)*t+n))/1200.);ph[n][d]+=f/SR;s+=saw(ph[n][d]);}
        lp+=(1-std::exp(-2*PI*1800/SR))*(s-lp);y[i]+=(float)(amp*env*lp/15);}
}
static void flute(std::vector<float>& y,double t0,double transpose,double amp){
    const double notes[]={587.33,659.26,739.99,880,783.99,739.99,659.26};const double dur[]={.35,.35,.35,.6,.4,.35,.55};
    std::mt19937 rng(9);std::normal_distribution<double> g(0,1);double ph=0,bp1=0,bp2=0,t=t0;
    for(int k=0;k<7;++k){const size_t a=(size_t)(t*SR),b=(size_t)((t+dur[k])*SR);for(size_t i=a;i<b&&i<y.size();++i){const double tt=(i-a)/SR,env=std::min(1.,tt/.04)*std::min(1.,(dur[k]-tt)/.03);
        const double f=notes[k]*transpose*(1+.006*std::sin(2*PI*5.5*tt)*std::min(1.,tt/.2));ph+=f/SR;
        const double n=g(rng);bp1+=.25*(n-bp1);bp2+=.25*(bp1-bp2);
        y[i]+=(float)(amp*env*(std::sin(2*PI*ph)+.25*std::sin(4*PI*ph)+.08*std::sin(6*PI*ph)+.12*(bp1-bp2)));}
        t+=dur[k];}
}
static void piano(std::vector<float>& y,double t0,double amp){
    const double notes[]={130.81,164.81,196,261.63,329.63};std::mt19937 rng(3);std::normal_distribution<double> g(0,1);
    for(double f0:notes)for(int k=1;k<=12;++k){const double f=k*f0*std::sqrt(1+.0004*k*k);if(f>12000)break;const double a=amp/std::pow(k,1.2)/5,tau=4.5/std::pow(k,.7)/6.91;
        for(size_t i=(size_t)(t0*SR);i<y.size();++i){const double t=i/SR-t0;const double e=std::exp(-t/tau);if(e<1e-5)break;y[i]+=(float)(a*e*std::min(1.,t/.002)*std::sin(2*PI*f*t+k));}}
    for(int i=0;i<(int)(.006*SR);++i)y[(size_t)(t0*SR)+i]+=(float)(amp*.3*g(rng)*std::exp(-i/(.0015*SR)));
}
static void drums(std::vector<float>& y,double amp){
    const double hits[]={.3,.9,1.5,2.1,2.55};std::mt19937 rng(7);std::normal_distribution<double> g(0,1);int k=0;
    for(double h:hits){const bool kick=(k++%2)==0;for(int i=0;i<(int)(.5*SR);++i){const double t=i/SR;double s;
        if(kick)s=std::sin(2*PI*(50*t+60*(1-std::exp(-t/.03))*.03))*std::exp(-t/.12)+.2*g(rng)*std::exp(-t/.004);
        else s=.6*std::sin(2*PI*185*t)*std::exp(-t/.06)+.8*g(rng)*std::exp(-t/.09);
        y[(size_t)(h*SR)+i]+=(float)(amp*s);}}
}
struct Material{const char* name;void(*make)(std::vector<float>&);};
int main(int argc,char** argv){
    if(argc<3){std::printf("usage: e2_listen <outdir> <suffix> [long]\n");return 2;}
    const std::string dir=argv[1],suffix=argv[2];const bool longOnly=argc>3;
    const char* rooms[][2]={{"Living room","small-room"},{"Concert hall (audience zone)","concert-hall"},{"Cathedral (long tail)","cathedral"}};
    auto lib=verbp::builtinRooms();
    const char* sources[]={"pad","flute","piano","drums"};
    for(auto& rp:rooms){
        auto it=std::find_if(lib.begin(),lib.end(),[&](const auto& r){return r.name==rp[0];});std::istringstream text(it->text);auto sc=parseScene(text,"e2");
        sc.sources.resize(1);sc.receivers=makeReceiverArray(Layout::StereoORTF,sc.listenerPos,.1);
        for(const char* src:sources){
            const bool isLong=longOnly;if(isLong&&(std::string(rp[1])!="concert-hall"||std::string(src)!="pad"))continue;
            const double hold=isLong?120:20,tailSec=std::string(rp[1])=="cathedral"?12:8,total=3+hold+tailSec;const size_t N=(size_t)(total*SR);
            std::vector<float> in(N,0.f);
            const std::string s=src;
            if(s=="pad")pad(in,0,3,1,.35);else if(s=="flute")flute(in,.2,1,.18);else if(s=="piano")piano(in,.3,.6);else drums(in,.45);
            // New material during the hold: 7..12 s (a different, transposed phrase).
            if(s=="flute")pad(in,7,12,1.5,.3);else flute(in,7,.75,.16);
            EngineConfig cfg;cfg.erOrder=1;cfg.tailRays=20000;cfg.tailThread=false;Engine e(sc,cfg);
            std::vector<float> L(N),R(N);std::vector<float> zi(128),l(128),r(128);
            for(size_t pos=0;pos<N;pos+=128){const int n=(int)std::min<size_t>(128,N-pos);const double t=pos/SR;const bool frozen=t>=3&&t<3+hold;
                e.setCreativeWet(0,100,frozen,0,300);std::copy_n(in.data()+pos,n,zi.data());const float* ip[]={zi.data()};float* op[]={l.data(),r.data()};e.process(ip,op,n);
                std::copy_n(l.data(),n,L.data()+pos);std::copy_n(r.data(),n,R.data()+pos);}
            float peak=0;for(size_t i=0;i<N;++i)peak=std::max(peak,std::max(std::abs(L[i]),std::abs(R[i])));
            const std::string path=dir+"/"+rp[1]+"_"+src+(isLong?"_2min":"")+"_"+suffix+".wav";writeWav24(path,L,R);
            std::printf("%s peak %.4f (%.2f dBFS) %.1f s\n",path.c_str(),peak,20*std::log10(std::max(1e-9f,peak)),total);
        }
    }
    return 0;
}
