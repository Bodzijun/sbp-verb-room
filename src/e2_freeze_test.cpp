// E2 focused Freeze tests: the TailFreeze network hold, compared with the E1 granular hold
// (legacy::TailHold, unchanged copy) on identical input where the brief asks for both numbers.
#include "freeze_network.hpp"
#include "e2_granular_reference.hpp"
#include "dsp.hpp"
#include "rt_engine.hpp"
#include "layouts.hpp"
#include "../plugin/RoomLibrary.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <sstream>
using namespace verb;

static std::atomic<long long> allocations{0};
static std::atomic<bool> countAllocations{false};
void* operator new(size_t n){if(countAllocations.load(std::memory_order_relaxed))allocations.fetch_add(1);if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}

static int failed=0,passed=0;
static void check(bool ok,const char* msg){std::printf("[%s] %s\n",ok?"PASS":"FAIL",msg);ok?++passed:++failed;}
static constexpr double PI=3.141592653589793;
using Chan=std::vector<float>;
using Multi=std::vector<Chan>;
static const double centres[7]={125,250,500,1000,2000,4000,8000};

// ---------- signals ----------
// Stationary "tail": shaped Gaussian noise; channels share a common part (zero-lag correlation rho).
static Multi noiseTail(int n,int R,unsigned seed,double rho,double sr){
    std::mt19937 rng(seed);std::normal_distribution<double> g(0,1);Multi x(R,Chan(n));
    std::vector<double> lp(R,0),hp(R,0);const double a=1-std::exp(-2*PI*3500/sr),h=1-std::exp(-2*PI*60/sr);
    for(int i=0;i<n;++i){const double common=g(rng);for(int r=0;r<R;++r){double v=std::sqrt(rho)*common+std::sqrt(1-rho)*g(rng);lp[r]+=a*(v-lp[r]);hp[r]+=h*(lp[r]-hp[r]);x[r][i]=(float)(.05*(lp[r]-hp[r]+.3*v));}}
    return x;
}
// Free decay of a stationary tail: every octave band (brick-wall FFT bands, so the reference level
// of each octave at any time is exact) decays with its own RT60 from sample `from`.
static void applyDecay(Multi& x,int from,const std::array<double,7>& rt,double sr){
    for(auto& c:x){const size_t N=nextPow2(c.size());std::vector<cd> a(N);for(size_t i=0;i<c.size();++i)a[i]=c[i];fft(a,false);std::vector<double> y(c.size(),0.);
        for(int b=0;b<7;++b){std::vector<cd> z(N);for(size_t k=0;k<N;++k){const size_t kk=k<=N/2?k:N-k;const double f=kk*sr/N;const double lo=b==0?0:125*std::pow(2.,b)/std::sqrt(2.),hi=b==6?1e9:125*std::pow(2.,b)*std::sqrt(2.);if(f>=lo&&f<hi)z[k]=a[k];}
            fft(z,true);for(size_t i=0;i<c.size();++i){const double t=std::max<double>(0,(double)i-from)/sr;y[i]+=z[i].real()*std::pow(10.,-3*t/rt[b]);}}
        for(size_t i=0;i<c.size();++i)c[i]=(float)y[i];}
}
// ---------- drivers ----------
using Schedule=bool(*)(double);
static Multi runNew(const Multi& in,double sr,const std::array<double,7>& rt,bool(*frozen)(double),int block=128,TailFreeze* keep=nullptr){
    const int R=(int)in.size(),n=(int)in[0].size();TailFreeze local;TailFreeze& f=keep?*keep:local;if(!keep){f.prepare(sr,R,block);f.setRoomDecay(rt);}
    Multi out(R,Chan(n));std::vector<float> t((size_t)R*block),h((size_t)R*block);
    for(int p=0;p<n;p+=block){const int m=std::min(block,n-p);for(int r=0;r<R;++r)std::copy_n(&in[r][p],m,&t[(size_t)r*block]);
        const bool active=f.process(t.data(),h.data(),block,R,m,frozen(p/sr));const float* src=active?h.data():t.data();for(int r=0;r<R;++r)std::copy_n(src+(size_t)r*block,m,&out[r][p]);}
    return out;
}
static Multi runOld(const Multi& in,double sr,const std::array<double,7>& rt,bool(*frozen)(double)){
    const int R=(int)in.size(),n=(int)in[0].size();legacy::TailHold h;h.prepare(sr,R);h.setRelease(rt);Multi out(R,Chan(n));
    for(int i=0;i<n;++i){h.begin(frozen(i/sr));for(int r=0;r<R;++r)out[r][i]=h.process(r,in[r][i]);h.end();}
    return out;
}
// ---------- analysis ----------
static std::vector<cd> spectrum(const Chan& x,int start,int len){const size_t N=nextPow2((size_t)len);std::vector<cd> a(N);for(int i=0;i<len;++i)a[i]=x[(size_t)start+i];fft(a,false);return a;}
// Octave-band analytic signal of a segment (FFT brick-wall, zero-phase).
static std::vector<cd> bandAnalytic(const Chan& x,int start,int len,double hz,double sr){
    auto a=spectrum(x,start,len);const size_t N=a.size();for(size_t k=0;k<N;++k){const double f=k*sr/N;const bool in=k<N/2&&f>=hz/std::sqrt(2.)&&f<hz*std::sqrt(2.);a[k]=in?a[k]*2.:cd(0);}fft(a,true);a.resize((size_t)len);return a;
}
static std::vector<double> windowPowers(const Chan& x,int start,int len,double hz,double sr,int win){
    auto z=bandAnalytic(x,start,len,hz,sr);std::vector<double> p;for(int w=0;w+win<=len;w+=win){double e=0;for(int i=w;i<w+win;++i)e+=std::norm(z[(size_t)i]);p.push_back(e/win);}return p;
}
static double bandPower(const Chan& x,int start,int len,double hz,double sr){auto p=windowPowers(x,start,len,hz,sr,len);return p.empty()?0:p[0];}
// Envelope modulation in 0.5..20 Hz: PSD of the band envelope (Welch, 2 s Hann), integrated and
// normalised by mean envelope^2; plus the largest PSD bin relative to the median (periodic peaks).
struct Mod{double index=0,peakDb=0;};
static Mod modulation(const Chan& x,int start,int len,double hz,double sr){
    auto z=bandAnalytic(x,start,len,hz,sr);const int D=(int)(sr/400);std::vector<double> e;for(int i=0;i+D<=len;i+=D){double s=0;for(int k=i;k<i+D;++k)s+=std::abs(z[(size_t)k]);e.push_back(s/D);}
    double mean=0;for(double v:e)mean+=v;mean/=e.size();const int seg=800;std::vector<double> psd(seg/2,0);int count=0;
    for(int s=0;s+seg<=(int)e.size();s+=seg/2){std::vector<cd> a(1024);for(int i=0;i<seg;++i){const double w=.5-.5*std::cos(2*PI*i/(seg-1));a[i]=(e[(size_t)s+i]-mean)*w;}fft(a,false);for(int k=0;k<seg/2;++k)psd[k]+=std::norm(a[k]);++count;}
    Mod m;std::vector<double> sel;const double df=400./1024;for(int k=0;k<seg/2;++k){const double f=k*df;if(f>=.5&&f<=20){m.index+=psd[k];sel.push_back(psd[k]);}}
    double norm=0;for(int i=0;i<seg;++i){const double w=.5-.5*std::cos(2*PI*i/(seg-1));norm+=w*w;}m.index=std::sqrt(m.index/count/norm/(1024./400)*df*2)/mean;
    std::vector<double> sorted=sel;std::sort(sorted.begin(),sorted.end());m.peakDb=10*std::log10(*std::max_element(sel.begin(),sel.end())/sorted[sorted.size()/2]);
    return m;
}
// Short-time spectral flux: RMS change (dB) of the 7 octave energies between consecutive 50 ms frames.
static double flux(const Chan& x,int start,int len,double sr){
    const int F=(int)(.05*sr);std::vector<std::array<double,7>> fr;for(int s=start;s+F<=start+len;s+=F){auto a=spectrum(x,s,F);const size_t N=a.size();std::array<double,7> e{};
        for(size_t k=1;k<N/2;++k){const double f=k*sr/N;for(int b=0;b<7;++b)if(f>=centres[b]/std::sqrt(2.)&&f<centres[b]*std::sqrt(2.))e[b]+=std::norm(a[k]);}for(auto& v:e)v=10*std::log10(v+1e-30);fr.push_back(e);}
    double s=0;int c=0;for(size_t i=1;i<fr.size();++i)for(int b=0;b<7;++b){const double d=fr[i][b]-fr[i-1][b];s+=d*d;++c;}return std::sqrt(s/c);
}
static double correlation(const Chan& a,const Chan& b,int start,int len){double xy=0,xx=0,yy=0;for(int i=start;i<start+len;++i){xy+=(double)a[i]*b[i];xx+=(double)a[i]*a[i];yy+=(double)b[i]*b[i];}return xy/std::sqrt(xx*yy);}
// RT60 from the Schroeder backward integral of the octave-band power (T20: -5..-25 dB, x3).
// 10 ms octave powers from `start`: two cascaded constant-gain band-pass biquads (Q = sqrt 2, an octave
// filter of order 4 in the ISO 3382 manner; its own ringing is far shorter than any RT60 tested), run
// from up to 1 s before `start`. A brick-wall FFT band leaks the steady part into the decay.
static std::vector<double> decayPowers(const Chan& x,int start,int len,double hz,double sr){
    const double w=2*PI*hz/sr,al=std::sin(w)/(2*std::sqrt(2.)),a0=1+al;const double b0=al/a0,b2=-al/a0,a1=-2*std::cos(w)/a0,a2=(1-al)/a0;
    double z[4][2]={};const int win=(int)(.01*sr);std::vector<double> p((size_t)(len/win),0.);
    for(int i=std::max(0,start-(int)sr);i<start+len;++i){double v=x[(size_t)i];for(auto& s2:z){const double y=b0*v+s2[0];s2[0]=-a1*y+s2[1];s2[1]=b2*v-a2*y;v=y;}
        const int k=(i-start)/win;if(i>=start&&k<(int)p.size())p[(size_t)k]+=v*v;}
    return p;
}
// RT60 from the octave energy-decay curve (T20: -5..-25 dB) of summed 10 ms powers.
static double rt60FromPowers(const std::vector<double>& p){
    std::vector<double> edc(p.size());double acc=0;for(size_t i=p.size();i-->0;){acc+=p[i];edc[i]=acc;}
    std::vector<double> db;for(double v:edc)db.push_back(10*std::log10(v/edc[0]+1e-40));int a=-1,b=-1;for(int i=0;i<(int)db.size();++i){if(a<0&&db[(size_t)i]<-5)a=i;if(db[(size_t)i]<-25){b=i;break;}}
    if(a<0||b<0||b-a<3)return -1;double sx=0,sy=0,sxx=0,sxy=0;const int n=b-a;for(int i=a;i<b;++i){const double t=i*.01;sx+=t;sy+=db[(size_t)i];sxx+=t*t;sxy+=t*db[(size_t)i];}
    const double slope=(n*sxy-sx*sy)/(n*sxx-sx*sx);return -60/slope;
}
// Musical tails: a flute-like phrase or a detuned saw pad, convolved with a synthetic stereo
// exponential-noise reverb (RT60 2 s, channels decorrelated).
static Multi musicTail(bool padChord,int n,double sr){
    Chan dry(n,0.f);std::mt19937 rng(9);std::normal_distribution<double> g(0,1);
    if(padChord){const double notes[]={110,130.81,164.81,220,261.63};double lp=0,ph[5][3]={};for(int i=0;i<(int)(3*sr)&&i<n;++i){const double t=i/sr;double s=0;
        for(int k=0;k<5;++k)for(int d=0;d<3;++d){ph[k][d]+=notes[k]*std::pow(2.,((d-1)*7+3*std::sin(2*PI*(4.8+.3*k)*t+k))/1200.)/sr;s+=2*(ph[k][d]-std::floor(ph[k][d]))-1;}
        lp+=(1-std::exp(-2*PI*1800/sr))*(s-lp);dry[i]=(float)(.02*lp*std::min(1.,t/.3)*std::min(1.,(3-t)/.05));}}
    else{const double notes[]={587.33,659.26,739.99,880,783.99,739.99,659.26},dur[]={.35,.35,.35,.6,.4,.35,.55};double ph=0,t0=.1;
        for(int k=0;k<7;++k){for(int i=(int)(t0*sr);i<(int)((t0+dur[k])*sr)&&i<n;++i){const double tt=i/sr-t0;ph+=notes[k]*(1+.006*std::sin(2*PI*5.5*tt))/sr;
            dry[i]=(float)(.1*std::min(1.,tt/.04)*std::min(1.,(dur[k]-tt)/.03)*(std::sin(2*PI*ph)+.25*std::sin(4*PI*ph)+.08*std::sin(6*PI*ph)+.03*g(rng)));}t0+=dur[k];}}
    Multi out(2);for(int c=0;c<2;++c){Chan ir((size_t)(2.5*sr));for(size_t i=0;i<ir.size();++i)ir[i]=(float)(g(rng)*std::pow(10.,-3.*i/sr/2.)*.02);auto y=convolve(dry,ir);y.resize((size_t)n);out[c]=y;}
    return out;
}
// Spectral stationarity at semitone resolution: RMS deviation (dB) of 500 ms frame spectra from
// the long-term spectrum, over bands within 40 dB of the strongest. Notes coming and going (grains
// replaying different parts of a phrase) raise it; a steady wash keeps it low.
static double semitoneDeviation(const Chan& x,int start,int len,double sr){
    const int F=(int)(.5*sr),H=F/2;std::vector<std::vector<double>> fr;std::vector<double> lo;for(double f=100;f<8000;f*=std::pow(2.,1/12.))lo.push_back(f);
    for(int s=start;s+F<=start+len;s+=H){std::vector<cd> a(nextPow2((size_t)F));for(int i=0;i<F;++i)a[i]=x[(size_t)s+i]*(.5-.5*std::cos(2*PI*i/F));fft(a,false);std::vector<double> e(lo.size()-1,1e-30);
        for(size_t k=1;k<a.size()/2;++k){const double f=k*sr/a.size();auto it=std::upper_bound(lo.begin(),lo.end(),f);if(it==lo.begin()||it==lo.end())continue;e[(size_t)(it-lo.begin()-1)]+=std::norm(a[k]);}fr.push_back(e);}
    std::vector<double> mean(lo.size()-1,0);for(auto& e:fr)for(size_t b=0;b<e.size();++b)mean[b]+=e[b]/fr.size();const double top=*std::max_element(mean.begin(),mean.end());
    double s=0;int c=0;for(auto& e:fr)for(size_t b=0;b<e.size();++b)if(mean[b]>top*1e-4){const double d=10*std::log10(e[b]/mean[b]);s+=d*d;++c;}return std::sqrt(s/c);
}

static double gEngage=3;static bool frozenAfter(double t){return t>=gEngage;}
static double gRelease=1e9;static bool frozenWindow(double t){return t>=gEngage&&t<gRelease;}
static double gRe=1e9;static bool frozenTwice(double t){return (t>=gEngage&&t<gRelease)||t>=gRe;}

static std::string only;static bool want(int k){if(only.empty())return true;return (","+only+",").find(","+std::to_string(k)+",")!=std::string::npos;}
int main(int argc,char** argv){std::setvbuf(stdout,nullptr,_IONBF,0);
    const bool quick=argc>1&&std::string(argv[1])=="quick";if(argc>2)only=argv[2];
    constexpr double sr=48000;const std::array<double,7> hall{{2.2,2.1,2.0,1.9,1.7,1.3,0.8}},small{{.6,.5,.45,.42,.38,.32,.25}},cathedral{{8,7.5,7,6.3,5.2,3.6,2}};
    // 1) Network structure.
    if(want(1))
    {TailFreeze f;f.prepare(sr,2,128);const auto& net=f.network();bool primes=true;for(int i=0;i<DiffusingNetwork::N;++i){int d=net.delay(i);for(int k=2;(long long)k*k<=d;++k)primes&=d%k!=0;if(i)primes&=d>net.delay(i-1);}
     std::printf("network delays %d..%d samples (%.1f..%.1f ms), memory %.3f MiB for 2 ch @48k\n",net.minDelay(),net.maxDelay(),1000.*net.minDelay()/sr,1000.*net.maxDelay()/sr,f.memoryBytes()/1048576.);
     check(primes,"16 distinct increasing prime delay lengths");
     for(double rate:{44100.,48000.,96000.,192000.}){TailFreeze g;g.prepare(rate,16,128);std::printf("memory @%.0f Hz: %.2f MiB for 16 channels\n",rate,g.memoryBytes()/1048576.);}
     // The colouration table (output EQ) re-measured with independent seeds at 48 kHz.
     {const auto meas=DiffusingNetwork::measureColouration(sr,quick?48:160,0x1234567u),tab=DiffusingNetwork::colourationDb(sr);double worst=0;std::printf("colouration 48 kHz (measured/table, dB):");
      for(int b=0;b<7;++b){std::printf(" %+.2f/%+.2f",meas[b],tab[b]);worst=std::max(worst,std::abs(meas[b]-tab[b])*(b<2?.5:1.));}std::printf("\n");
      check(worst<=.2,"output-EQ colouration table matches a fresh measurement (0.2 dB; 0.4 dB at 125/250 Hz)");}}
    // 2) Continuity at engage, 8-realisation ensemble, 250 ms windows; old vs new.
    if(want(2))
    {const int seeds=16,R=2;const int n=(int)(6*sr);gEngage=3;double worstNew=0,worstOld=0,singleNew=0;
     std::array<std::vector<double>,7> accNew,accOld;for(auto& v:accNew)v.assign(12,0);for(auto& v:accOld)v.assign(12,0);
     for(int s=0;s<seeds;++s){auto in=noiseTail(n,R,100+s,.4,sr);auto a=runNew(in,sr,hall,frozenAfter),o=runOld(in,sr,hall,frozenAfter);
        for(int b=0;b<7;++b){auto pn=windowPowers(a[0],(int)(2*sr),(int)(3*sr),centres[b],sr,(int)(.25*sr)),po=windowPowers(o[0],(int)(2*sr),(int)(3*sr),centres[b],sr,(int)(.25*sr));
            double ref=0;for(int w=0;w<4;++w)ref+=pn[w]/4;for(int w=4;w<12;++w)singleNew=std::max(singleNew,std::abs(10*std::log10(pn[w]/ref)));
            for(int w=0;w<12;++w){accNew[b][w]+=pn[w];accOld[b][w]+=po[w];}}}
     for(int b=0;b<7;++b){double rn=0,ro=0;for(int w=0;w<4;++w){rn+=accNew[b][w]/4;ro+=accOld[b][w]/4;}std::printf("engage %5.0f Hz new:",centres[b]);
        for(int w=4;w<12;++w){const double dn=10*std::log10(accNew[b][w]/rn),dold=10*std::log10(accOld[b][w]/ro);worstNew=std::max(worstNew,std::abs(dn));worstOld=std::max(worstOld,std::abs(dold));std::printf(" %+.2f",dn);}std::printf("\n");}
     std::printf("engage continuity (ensemble of %d, 8 windows 0..2 s): worst new %.3f dB, old granular %.3f dB; single-run worst new %.3f dB\n",seeds,worstNew,worstOld,singleNew);
     check(worstNew<=1,"engage: every octave band within +/-1 dB in every 250 ms window over 0..2 s (ensemble mean)");}
    // 3) Stationarity (modulation 0.5..20 Hz), spectral flux, inter-channel correlation; old vs new vs stationary reference.
    if(want(3))
    {const int R=2,n=(int)(24*sr);gEngage=4;auto in=noiseTail(n,R,7,.4,sr);auto ref=noiseTail(n,R,8,.4,sr);auto a=runNew(in,sr,hall,frozenAfter),o=runOld(in,sr,hall,frozenAfter);
     const int s0=(int)(6*sr),len=(int)(16*sr);double worstRatio=0,worstOldRatio=0,worstPeak=0,worstOldPeak=0;bool better=true;
     for(int b=0;b<7;++b){const Mod mr=modulation(ref[0],s0,len,centres[b],sr),mn=modulation(a[0],s0,len,centres[b],sr),mo=modulation(o[0],s0,len,centres[b],sr);
        std::printf("modulation %5.0f Hz: reference %.4f, new %.4f (x%.2f, peak %.1f dB), old %.4f (x%.2f, peak %.1f dB)\n",centres[b],mr.index,mn.index,mn.index/mr.index,mn.peakDb,mo.index,mo.index/mr.index,mo.peakDb);
        worstRatio=std::max(worstRatio,mn.index/mr.index);worstOldRatio=std::max(worstOldRatio,mo.index/mr.index);worstPeak=std::max(worstPeak,mn.peakDb);worstOldPeak=std::max(worstOldPeak,mo.peakDb);better&=mn.index<mo.index;}
     std::printf("stationarity: worst new/reference %.3f (old %.3f); worst modulation peak/median new %.2f dB (old %.2f dB)\n",worstRatio,worstOldRatio,worstPeak,worstOldPeak);
     check(worstRatio<=1.25,"held envelope modulation 0.5..20 Hz <= 1.25 x stationary Gaussian reference in every octave");
     std::printf("noise input: new below old in every octave: %s (both are close to the stationary reference; reported)\n",better?"yes":"no");
     // Musical tails: the material the owner listened to. Freeze 0.1 s after the phrase/chord ends.
     for(int k=0;k<2;++k){const bool padChord=k==1;auto mt=musicTail(padChord,(int)(24*sr),sr);gEngage=padChord?3.0:3.2;auto an=runNew(mt,sr,hall,frozenAfter),ao=runOld(mt,sr,hall,frozenAfter);
        const int m0=(int)(5*sr),ml=(int)(16*sr);const double dn=semitoneDeviation(an[0],m0,ml,sr),dold=semitoneDeviation(ao[0],m0,ml,sr),dref=semitoneDeviation(ref[0],m0,ml,sr);
        std::printf("%s tail: semitone 500 ms spectral deviation new %.3f dB, old %.3f dB (stationary noise %.3f dB)\n",padChord?"pad":"flute",dn,dold,dref);
        double mnSum=0,moSum=0;int used=0;bool peaksLower=true;for(int b=0;b<7;++b){const double pw=bandPower(mt[0],(int)(2*sr),(int)sr,centres[b],sr);double total=0;for(int q=0;q<7;++q)total=std::max(total,bandPower(mt[0],(int)(2*sr),(int)sr,centres[q],sr));if(pw<total*1e-3)continue;
            const Mod mn=modulation(an[0],m0,ml,centres[b],sr),mo=modulation(ao[0],m0,ml,centres[b],sr);std::printf("  %s %5.0f Hz modulation 0.5..20 Hz: new %.4f (peak %.1f dB), old %.4f (peak %.1f dB)\n",padChord?"pad":"flute",centres[b],mn.index,mn.peakDb,mo.index,mo.peakDb);mnSum+=mn.index;moSum+=mo.index;++used;peaksLower&=mn.peakDb<mo.peakDb;}
        std::printf("  %s mean modulation over %d active octaves: new %.4f, old %.4f\n",padChord?"pad":"flute",used,mnSum/used,moSum/used);
        check(peaksLower,padChord?"pad tail: periodic modulation peak (peak/median of the 0.5..20 Hz envelope spectrum) below old granular in every active octave":"flute tail: periodic modulation peak (peak/median of the 0.5..20 Hz envelope spectrum) below old granular in every active octave");
        check(dn<=.75*dold,padChord?"pad tail: semitone spectral deviation of the hold <= 0.75 x old granular":"flute tail: semitone spectral deviation of the hold <= 0.75 x old granular");}
     const double fr=flux(ref[0],s0,len,sr),fn=flux(a[0],s0,len,sr),fo=flux(o[0],s0,len,sr);std::printf("spectral flux (50 ms, dB RMS): reference %.3f, new %.3f, old %.3f\n",fr,fn,fo);
     check(fn<=1.15*fr,"held spectral flux <= 1.15 x stationary reference");
     const double cPre=correlation(in[0],in[1],(int)(1*sr),(int)(3*sr)),cNew=correlation(a[0],a[1],s0,len),cOld=correlation(o[0],o[1],s0,len);std::printf("inter-channel correlation: pre-freeze %.4f, held new %.4f, held old %.4f\n",cPre,cNew,cOld);
     check(std::abs(cNew-cPre)<=.1,"held inter-channel correlation within +/-0.1 of the pre-freeze tail");
     double worstAuto=0;for(int lag=(int)(sr/50);lag<=(int)(sr/4);lag+=31){double xy=0,xx=0,yy=0;for(int i=s0;i<s0+(int)sr;++i){xy+=a[0][i]*a[0][i+lag];xx+=a[0][i]*a[0][i];yy+=a[0][i+lag]*a[0][i+lag];}worstAuto=std::max(worstAuto,std::abs(xy/std::sqrt(xx*yy)));}
     std::printf("held max |autocorrelation| 20..250 ms lags %.4f\n",worstAuto);check(worstAuto<.3,"no strong short loop autocorrelation");}
    // 4) Five-minute stability (no growth, every octave +/-1 dB), and finite output.
    if(want(4)&&!quick){const int R=2;const double hold=300;const int n=(int)((3+hold)*sr);gEngage=3;auto in=noiseTail((int)(3*sr),R,21,.4,sr);for(auto& c:in)c.resize((size_t)n,0.f);
     const auto t0=std::chrono::steady_clock::now();auto a=runNew(in,sr,hall,frozenAfter);const double secs=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
     bool finite=true;for(auto& c:a)for(float v:c)finite&=std::isfinite(v);double worst=0;
     for(int b=0;b<7;++b){const double first=bandPower(a[0],(int)(5*sr),(int)(10*sr),centres[b],sr);double w=0;for(double t=40;t+10<=3+hold;t+=50){const double d=10*std::log10(bandPower(a[0],(int)(t*sr),(int)(10*sr),centres[b],sr)/first);w=std::max(w,std::abs(d));}
        const double last=10*std::log10(bandPower(a[0],(int)((hold-7)*sr),(int)(10*sr),centres[b],sr)/first);std::printf("5 min %5.0f Hz: end %+.4f dB, worst 10 s window %.4f dB\n",centres[b],last,w);worst=std::max(worst,std::max(w,std::abs(last)));}
     std::printf("5-minute hold render %.2f s (%.1fx real time, 2 ch)\n",secs,(3+hold)/secs);check(finite,"5-minute hold finite");check(worst<=1,"5-minute hold: every octave within +/-1 dB, no growth");}
    // 5) Input rejection: different tail input after engage gives identical held output after the crossfade.
    if(want(5))
    {const int R=2,n=(int)(5*sr);gEngage=2;auto in=noiseTail(n,R,31,.4,sr);auto other=in;std::mt19937 rng(5);std::uniform_real_distribution<float> u(-.4f,.4f);for(auto& c:other)for(int i=(int)(2*sr);i<n;++i)c[i]=u(rng);
     auto a=runNew(in,sr,hall,frozenAfter),b=runNew(other,sr,hall,frozenAfter);bool same=true;for(int r=0;r<R;++r)for(int i=(int)(2*sr)+(int)(.02*sr)+128;i<n;++i)same&=a[r][i]==b[r][i];check(same,"frozen hold rejects any new tail input after the 20 ms crossfade");}
    // 6) Click-free engage/release on tones (as E1: adjacent step < .01 FS for a .02+.02 FS tone pair).
    if(want(6))
    {const int n=(int)(6*sr);Multi in(1,Chan(n));for(int i=0;i<n;++i)in[0][i]=(float)(.02*(std::sin(2*PI*125*i/sr)+std::sin(2*PI*1000*i/sr)));gEngage=2;gRelease=4;
     auto a=runNew(in,sr,hall,frozenWindow);double worst=0;for(int i=1;i<n;++i)if(std::abs(i-2*sr)<2400||std::abs(i-4*sr)<2400)worst=std::max(worst,std::abs((double)a[0][i]-a[0][i-1]));
     const double natural=.02*(2*PI*125/sr+2*PI*1000/sr);std::printf("engage/release tone max adjacent step %.6f FS (signal's own max step %.6f)\n",worst,natural);check(worst<.01,"engage/release continuous tone derivative stays below .01 FS");gRelease=1e9;}
    // 7) Release RT60 per band within +/-10 % of the configured room RT60 (three rooms).
    if(want(7))for(const auto* rt:{&small,&hall,&cathedral}){const int R=1,seeds=8;const double rel=rt==&cathedral?18:rt==&hall?6:3;const int n=(int)((5+rel)*sr);gEngage=2;gRelease=4;
     // Ensemble of 8 seeds (one noise realisation's low-octave EDC fluctuates by tens of %). Reference: stationary
     // noise decaying from 4 s with exactly the room RT60 per octave, measured the same way (printed).
     std::array<std::vector<double>,7> pn,pr;
     for(int s=0;s<seeds;++s){auto in=noiseTail((int)(2*sr),R,41+s,0,sr);for(auto& c:in)c.resize((size_t)n,0.f);auto a=runNew(in,sr,*rt,frozenWindow);auto ref=noiseTail(n,R,142+s,0,sr);applyDecay(ref,(int)(4*sr),*rt,sr);
        for(int b=0;b<7;++b){auto x=decayPowers(a[0],(int)(4*sr),(int)(rel*sr),centres[b],sr),y=decayPowers(ref[0],(int)(4*sr),(int)(rel*sr),centres[b],sr);if(pn[b].empty()){pn[b].assign(x.size(),0);pr[b].assign(y.size(),0);}for(size_t i=0;i<x.size();++i){pn[b][i]+=x[i];pr[b][i]+=y[i];}}}
     bool ok=true;std::printf("release RT60 (room %.2f s mid):",(*rt)[3]);
     for(int b=0;b<7;++b){const double m=rt60FromPowers(pn[b]),mr=rt60FromPowers(pr[b]);const double e=m/(*rt)[b]-1;std::printf(" %.0fHz %.3f(%+.1f%%; ideal %+.1f%%)",centres[b],m,100*e,100*(mr/(*rt)[b]-1));ok&=m>0&&std::abs(e)<=.10;}
     std::printf("\n");check(ok,"release decays per octave with the room RT60 within +/-10 % (8-seed ensemble EDC)");gRelease=1e9;}
    // 8) Decaying capture: freeze 0.3 s into a free decay; held level vs the decaying level at engage.
    if(want(8))for(int cfg=0;cfg<3;++cfg){const auto* rt=cfg==0?&small:&hall;const int R=cfg==2?12:2,n=(int)(6*sr),seeds=6;double singleN=0,singleO=0;std::array<double,7> sp{},sn{},so{};
     // Ensemble mean over 6 seeds is asserted (the bias of capture + un-decay); a single 160 ms snapshot
     // scatters by about +-0.8 dB at 250 Hz by itself (printed as the single-run worst).
     for(int s=0;s<seeds;++s){auto in=noiseTail(n,R,51+s,.4,sr);applyDecay(in,(int)(2*sr),*rt,sr);auto pre=noiseTail(n,R,51+s,.4,sr);gEngage=2.3;auto a=runNew(in,sr,*rt,frozenAfter),o=runOld(in,sr,*rt,frozenAfter);
        for(int b=1;b<7;++b){const double p0=bandPower(pre[0],(int)(1*sr),(int)(1*sr),centres[b],sr)*std::pow(10.,-6*.3/(*rt)[b]);
            const double hn=bandPower(a[0],(int)(3*sr),(int)(2*sr),centres[b],sr),ho=bandPower(o[0],(int)(3*sr),(int)(2*sr),centres[b],sr);sp[b]+=p0;sn[b]+=hn;so[b]+=ho;
            singleN=std::max(singleN,std::abs(10*std::log10(hn/p0)));singleO=std::max(singleO,std::abs(10*std::log10(ho/p0)));}}
     double worstN=0,worstO=0;for(int b=1;b<7;++b){const double dn=10*std::log10(sn[b]/sp[b]),dold=10*std::log10(so[b]/sp[b]);std::printf("  decaying %5.0f Hz: level at engage %.1f dB, held new %+.2f old %+.2f (ensemble)\n",centres[b],10*std::log10(sp[b]/seeds),dn,dold);worstN=std::max(worstN,std::abs(dn));worstO=std::max(worstO,std::abs(dold));}
     std::printf("decaying capture (room mid RT60 %.2f s, %d channels%s, 250..8k Hz): ensemble worst new %.3f dB, old %.3f dB; single-run worst new %.3f dB, old %.3f dB\n",(*rt)[3],R,R>2?", un-decay spread over blocks":"",worstN,worstO,singleN,singleO);
     check(worstN<=1,"decaying capture holds the level of the tail at engage (+/-1 dB per octave, 6-seed ensemble)");}
    // 9) Re-engage during release: hold, release, re-engage after 0.5 s; holds the then-current level.
    if(want(9))
    {const int R=2,n=(int)(8*sr),seeds=4;gEngage=2;gRelease=3;gRe=3.5;std::array<std::vector<double>,7> rel,held;
     // Level at re-engage: straight-line fit (dB vs time) to the ensemble release decay 3.1..3.5 s, evaluated at
     // 3.5 s (IIR octave filters, 10 ms windows; a brick-wall FFT band of a short decaying segment smears).
     for(int s=0;s<seeds;++s){auto in=noiseTail((int)(2*sr),R,61+s,.4,sr);for(auto& c:in)c.resize((size_t)n,0.f);auto a=runNew(in,sr,hall,frozenTwice);
        for(int b=1;b<7;++b){auto x=decayPowers(a[0],(int)(3.1*sr),(int)(.4*sr),centres[b],sr),y=decayPowers(a[0],(int)(4.5*sr),(int)(3*sr),centres[b],sr);if(rel[b].empty()){rel[b].assign(x.size(),0);held[b].assign(y.size(),0);}
            for(size_t k=0;k<x.size();++k)rel[b][k]+=x[k];for(size_t k=0;k<y.size();++k)held[b][k]+=y[k];}}
     double worst=0;for(int b=1;b<7;++b){double sx=0,sy=0,sxx=0,sxy=0;const int m=(int)rel[b].size();for(int k=0;k<m;++k){const double t=(k+.5)*.01,d=10*std::log10(rel[b][k]);sx+=t;sy+=d;sxx+=t*t;sxy+=t*d;}
        const double slope=(m*sxy-sx*sy)/(m*sxx-sx*sx),icpt=(sy-slope*sx)/m,at=icpt+slope*.4;double h=0;for(double v:held[b])h+=v/held[b].size();const double d=10*std::log10(h)-at;
        std::printf("  re-engage %5.0f Hz: release slope %.1f dB/s, level at re-engage %.2f dB, held %.2f dB (%+.2f)\n",centres[b],slope,at,10*std::log10(h),d);worst=std::max(worst,std::abs(d));}
     std::printf("re-engage after 0.5 s release: worst octave level change %.3f dB (4-seed ensemble)\n",worst);check(worst<=1.5,"re-engage during release holds the current level (+/-1.5 dB per octave)");gRelease=1e9;gRe=1e9;}
    // 10) Tones: 60 s drift (E1 assertion kept) and capture level per octave tone.
    if(want(10))
    {double worstDrift=0,worstCap=0;for(double hz:centres){const int n=(int)(64*sr);Multi in(1,Chan(n));for(int i=0;i<(int)(4*sr);++i)in[0][i]=(float)(.1*std::sin(2*PI*hz*i/sr));gEngage=4;auto a=runNew(in,sr,hall,frozenAfter);
        double pre=0,first=0,last=0;for(int i=(int)(3*sr);i<(int)(4*sr);++i)pre+=a[0][i]*a[0][i];for(int i=(int)(6*sr);i<(int)(10*sr);++i)first+=a[0][i]*a[0][i];for(int i=(int)(59*sr);i<(int)(63*sr);++i)last+=a[0][i]*a[0][i];
        const double drift=10*std::log10(last/first),cap=10*std::log10(first/4/pre);std::printf("tone %5.0f Hz: capture %+.3f dB, 60 s drift %+.4f dB\n",hz,cap,drift);worstDrift=std::max(worstDrift,std::abs(drift));worstCap=std::max(worstCap,std::abs(cap));}
     check(worstDrift<=1,"coherent tonal Freeze level remains within +/-1 dB over 60 s");std::printf("tone capture worst %.3f dB (reported)\n",worstCap);}
    // 11) Empty hold, then release: recorder resumes and the next capture sustains the actual sound.
    if(want(11))
    {TailFreeze f;f.prepare(sr,1,128);f.setRoomDecay(hall);std::vector<float> t(128),h(128);for(int k=0;k<20;++k){std::fill(t.begin(),t.end(),0.f);f.process(t.data(),h.data(),128,1,128,true);}
     for(int k=0;k<750;++k){for(int i=0;i<128;++i)t[i]=.03f*(float)std::sin((k*128+i)*.123);f.process(t.data(),h.data(),128,1,128,false);}
     double p=0;for(int k=0;k<375;++k){std::fill(t.begin(),t.end(),0.f);const bool act=f.process(t.data(),h.data(),128,1,128,true);for(int i=0;i<128;++i){const double y=act?h[i]:t[i];p+=y*y;}}check(p>.1,"empty Freeze release resumes recorder; next capture sustains actual sound");}
    // 12) Every layout and sample rate: finite and non-silent per channel; Physical (unarmed) does nothing.
    if(want(12))for(double rate:{44100.,48000.,88200.,96000.,176400.,192000.})for(int count:{1,2,3,4,6,8,12,16}){
        const int n=(int)(1.5*rate);Multi in(count,Chan(n));for(int c=0;c<count;++c)for(int i=0;i<n;++i)in[c][i]=.01f*(c+1)*(float)std::sin(i*(.071+c*.003)*48000/rate);
        gEngage=.6;auto a=runNew(in,rate,hall,frozenAfter);bool alive=true;for(int c=0;c<count;++c){double p=0;for(int i=(int)(1*rate);i<n;++i){alive&=std::isfinite(a[c][i]);p+=a[c][i]*a[c][i];}alive&=p>1e-4;}
        if(!alive||count==16)std::printf("rate %.0f channels %d: %s\n",rate,count,alive?"finite, non-silent":"FAILED");if(!alive)check(false,"layout/rate finite non-silent");}
    check(true,"Freeze finite and non-silent for 1..16 channels at 44.1..192 kHz");
    {TailFreeze f;f.prepare(sr,2,128);f.setArmed(false);std::vector<float> t(256,.1f),h(256,-7.f);bool untouched=true;for(int k=0;k<100;++k){untouched&=!f.process(t.data(),h.data(),128,2,128,false);}for(float v:h)untouched&=v==-7.f;check(untouched,"Physical (unarmed, off): no output written, tail passes unchanged");}
    // 13) Engine: real Concert hall, zero callback allocations while toggling Freeze; tail-only rejection.
    if(want(13))
    {auto rooms=verbp::builtinRooms();auto room=std::find_if(rooms.begin(),rooms.end(),[](const auto& r){return r.name=="Concert hall (audience zone)";});std::istringstream text(room->text);auto sc=parseScene(text,"e2");sc.sources.resize(1);sc.receivers=makeReceiverArray(Layout::StereoORTF,sc.listenerPos,.1);
     EngineConfig config;config.erOrder=1;config.tailRays=8000;config.tailThread=false;Engine e1(sc,config),e2(sc,config);e1.setSoloMask(4);e2.setSoloMask(4);
     std::mt19937 rng(3);std::uniform_real_distribution<float> u(-.1f,.1f);std::vector<float> in(256),l1(256),r1(256),l2(256),r2(256),zero(256,0.f);const float* ip[]={in.data()};const float* zp[]={zero.data()};float* o1[]={l1.data(),r1.data()};float* o2[]={l2.data(),r2.data()};
     bool same=true;long long allocs=0;
     for(int k=0;k<2000;++k){for(float& v:in)v=u(rng);const bool frozen=k>=600;e1.setCreativeWet(0,100,frozen,0,300);e2.setCreativeWet(0,100,frozen,0,300);
        allocations=0;countAllocations=true;e1.process(ip,o1,256);e2.process(k<600?ip:zp,o2,256);countAllocations=false;allocs+=allocations;
        if(k>=600+2)for(int i=0;i<256;++i)same&=l1[i]==l2[i]&&r1[i]==r2[i];}
     check(same,"engine: new input does not enter the held tail (tail-only output identical with/without input)");
     for(int k=0;k<1500;++k){const bool frozen=(k/97)%2==0;e1.setCreativeWet(0,100,frozen,0,300);e1.setFreezeAvailable(k%300<150);for(float& v:in)v=u(rng);allocations=0;countAllocations=true;e1.process(ip,o1,1+k%256);countAllocations=false;allocs+=allocations;}
     std::printf("engine callback allocations while toggling Freeze/availability: %lld\n",allocs);check(allocs==0,"zero allocations in the audio callback");}
    // 14) Cost of the engage block (capture analysis) and of held blocks, decaying tail, 2/12/16 channels.
    if(want(14))for(double rate:{48000.,96000.,192000.})for(int count:{2,12,16}){const std::array<double,7>& rt=small;TailFreeze f;f.prepare(rate,count,128);f.setRoomDecay(rt);
        const int n=(int)(1.2*rate);Multi in=noiseTail(n,count,5,.3,rate);applyDecay(in,(int)(.7*rate),rt,rate);std::vector<float> t((size_t)count*128),h(t.size());double engage=0,typical=0;int blocks=0,frozenBlocks=0,pendingBlocks=0;
        for(int p=0;p+128<=n;p+=128){for(int c=0;c<count;++c)std::copy_n(&in[c][p],128,&t[(size_t)c*128]);const bool fr=p>=(int)(.85*rate);const auto a0=std::chrono::steady_clock::now();f.process(t.data(),h.data(),128,count,128,fr);
            const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-a0).count();if(fr){if(f.capturePending())++pendingBlocks;if(frozenBlocks++<64)engage=std::max(engage,us);else{typical+=us;++blocks;}}}
        std::printf("cost %3.0f kHz %2d ch: engage: worst block %.1f us of the first 64 (seed prepared over %d extra blocks, decay fraction %.2f), held block mean %.1f us per 128 samples (%.2f%% of the 128-sample period)\n",rate/1000,count,engage,pendingBlocks,f.lastDecayFraction(),typical/blocks,100*typical/blocks/(128e6/rate));}
    std::printf("TOTAL %d PASS %d FAIL\n",passed,failed);return failed?1:0;
}
