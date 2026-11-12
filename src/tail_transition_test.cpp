#include "rt_engine.hpp"
#include "geometry.hpp"
#include "dsp.hpp"
#include "../plugin/RoomLibrary.h"
#include <cstdio>
#include <numeric>

using namespace verb;
static int passes=0,failures=0;
static void check(bool yes,const char* text){std::printf("[%s] %s\n",yes?"PASS":"FAIL",text);yes?++passes:++failures;}
static std::vector<double> residual(const std::vector<double>& y) {
    double matrix[4][5]{};
    for(size_t i=0;i<y.size();++i) {
        const double t=2.*i/(y.size()-1)-1.;double powers[7]{1.};
        for(int k=1;k<7;++k)powers[k]=powers[k-1]*t;
        for(int r=0;r<4;++r){for(int c=0;c<4;++c)matrix[r][c]+=powers[r+c];matrix[r][4]+=powers[r]*y[i];}
    }
    for(int r=0;r<4;++r) {
        const double pivot=matrix[r][r];for(int c=r;c<5;++c)matrix[r][c]/=pivot;
        for(int k=0;k<4;++k)if(k!=r){const double v=matrix[k][r];for(int c=r;c<5;++c)matrix[k][c]-=v*matrix[r][c];}
    }
    auto result=y;
    for(size_t i=0;i<y.size();++i){const double t=2.*i/(y.size()-1)-1.;double trend=0;for(int k=3;k>=0;--k)trend=trend*t+matrix[k][4];result[i]-=trend;}
    return result;
}
static double correlation(const std::vector<double>& a,const std::vector<double>& b) {
    double aa=0,bb=0,ab=0;for(size_t i=0;i<a.size();++i){aa+=a[i]*a[i];bb+=b[i]*b[i];ab+=a[i]*b[i];}return ab/std::sqrt(aa*bb);
}
static std::vector<double> octave(const std::vector<float>& x,double sr,double lo,double hi) {
    const size_t n=nextPow2(x.size()*2);std::vector<cd> data(n);
    for(size_t i=0;i<x.size();++i)data[i]=x[i];fft(data,false);
    for(size_t k=0;k<n;++k) {
        const double f=(k<=n/2?k:n-k)*sr/n;
        const double edge=std::clamp(std::min((f-lo)/(.05*lo),(hi-f)/(.05*hi)),0.,1.);
        data[k]*=.5-.5*std::cos(kPi*edge);
    }
    fft(data,true);std::vector<double> result(x.size());for(size_t i=0;i<x.size();++i)result[i]=data[i].real();return result;
}
static double t20(const std::vector<double>& energy,double sr) {
    double total=std::accumulate(energy.begin(),energy.end(),0.),remaining=total;
    double sx=0,sy=0,sxx=0,sxy=0,n=0;
    for(size_t k=0;k<energy.size();++k) {
        const double db=10.*std::log10(std::max(1e-300,remaining/total));
        if(db< -25)break;if(db<= -5){const double t=k/sr;sx+=t;sy+=db;sxx+=t*t;sxy+=t*db;++n;}remaining-=energy[k];
    }
    return n>4?-60.*(n*sxx-sx*sx)/(n*sxy-sx*sy):-1.;
}
static Wav noise(const Scene& s,const TailHist& h) {
    const size_t n=(size_t)std::ceil(s.length*s.sr);BandBuf buf(h.h.size());
    for(auto& rc:buf)for(auto& b:rc)b.assign(n,0.);
    addTailNoise(s,h,s.seed,buf);Wav result;result.sr=(int)s.sr;result.ch=combineBands(buf,s.sr);fadeTailEnd(result.ch,n,s.sr);return result;
}
static void checkEnvelope(Scene s,const char* name) {
    s.length=effectiveTailSeconds(s,1);s=withClutter(withRoughness(s),128);
    auto raw=traceTail(s,s.sources[0],s.receivers,1,48000,s.seed,false),old=raw,now=raw;
    finalizeTail(old,false);finalizeTail(now);
    const auto a=analyzeTail(old),b=analyzeTail(now);
    bool area=true,slope=true,onset=true,outside=true;
    for(size_t r=0;r<now.h.size();++r)for(int band=0;band<NB;++band) {
        const auto& x=old.h[r][band];const auto& y=now.h[r][band];
        const double ea=std::accumulate(x.begin(),x.end(),0.),eb=std::accumulate(y.begin(),y.end(),0.);
        area&=ea==0?eb==0:std::abs(eb/ea-1.)<1e-10;
        size_t k=0;while(k<x.size()&&x[k]<=0){onset&=y[k]==0;++k;}if(k<x.size())onset&=x[k]==y[k];
        for(k=(size_t)std::ceil(.65/now.binDur);k<x.size();++k)outside&=x[k]==y[k];
    }
    for(int band=0;band<NB;++band)if(a.rt60[band]>0)slope&=std::abs(b.rt60[band]/a.rt60[band]-1.)<.03;
    check(area,"transition preserves energy per receiver/band to 1e-10 relative");
    check(slope,"transition preserves modeled octave T20 within 3%");
    check(onset&&outside,"onset and bins after 650 ms are exactly unchanged");
    const auto before=noise(s,old),after=noise(s,now);bool energy=true,rt=true;
    for(int band=0;band<NB;++band) {
        std::vector<double> e0(before.ch[0].size()),e1(e0.size());
        for(size_t r=0;r<before.ch.size();++r) {
            const auto x=octave(before.ch[r],s.sr,kBandHz[band]/std::sqrt(2.),kBandHz[band]*std::sqrt(2.));
            const auto y=octave(after.ch[r],s.sr,kBandHz[band]/std::sqrt(2.),kBandHz[band]*std::sqrt(2.));
            for(size_t k=0;k<x.size();++k){e0[k]+=x[k]*x[k];e1[k]+=y[k]*y[k];}
        }
        const double db=10*std::log10(std::accumulate(e1.begin(),e1.end(),0.)/std::accumulate(e0.begin(),e0.end(),0.));
        const double delta=t20(e1,s.sr)/t20(e0,s.sr)-1.;
        std::printf("%s octave %.0f energy %+.5f dB T20 %+.4f%%\n",name,kBandHz[band],db,100*delta);
        energy&=std::abs(db)<=.2;rt&=std::abs(delta)<=.03;
    }
    check(energy,"rendered late octave energies remain within +/-0.2 dB of 0.1.15 envelope");
    check(rt,"independently measured rendered late octave T20 remains within +/-3%");
}
static void quality(Scene s,const char* name) {
    EngineConfig c;c.tailThread=false;c.erOrder=1;c.tailRays=48000;
    Engine e(s,c);e.setEarlyDiffusion(.5);
    Wav out;out.sr=48000;out.ch.resize(s.receivers.size());for(auto& ch:out.ch)ch.resize(96000);
    std::vector<float> input(512);const float* ip[]={input.data()};std::vector<float*> op(out.ch.size());
    std::vector<std::vector<float>> scratch(out.ch.size(),std::vector<float>(512));for(size_t r=0;r<op.size();++r)op[r]=scratch[r].data();
    for(int k=0;k<32;++k)e.process(ip,op.data(),512);
    for(size_t pos=0;pos<96000;pos+=512){input[0]=pos==0?1.f:0.f;const int n=(int)std::min<size_t>(512,96000-pos);e.process(ip,op.data(),n);for(size_t r=0;r<op.size();++r)std::copy_n(op[r],n,out.ch[r].data()+pos);}
    std::vector<std::array<std::vector<double>,4>> residuals(out.ch.size());
    bool variance=true;double cb=0;int pairs=0;
    for(int band=0;band<4;++band) {
        double sum=0;const double lo=1000.*(1<<band),hi=2*lo;
        for(size_t r=0;r<out.ch.size();++r) {
            const auto x=octave(out.ch[r],48000,lo,hi);std::vector<double> levels;
            for(size_t pos=3840;pos<19200;pos+=480){double energy=0;for(size_t i=pos;i<pos+480;++i)energy+=x[i]*x[i];levels.push_back(10*std::log10(energy/480.+1e-300));}
            residuals[r][band]=residual(levels);for(double x:residuals[r][band])sum+=x*x;
        }
        const double actual=std::sqrt(sum/(32*out.ch.size())),expected=10./std::log(10.)/std::sqrt((hi-lo)*.01);
        std::printf("%s %.0f-%.0f Hz residual %.5f dB, Gaussian %.5f dB, factor %.5f\n",name,lo,hi,actual,expected,actual/expected);
        variance&=actual<=1.6*expected;
    }
    for(const auto& r:residuals)for(int i=0;i<4;++i)for(int j=i+1;j<4;++j){cb+=correlation(r[i],r[j]);++pairs;}
    std::printf("%s mean cross-band correlation %.6f\n",name,cb/pairs);
    check(variance,"80-400 ms: every band <=1.6 times stationary Gaussian expectation");
    check(cb/pairs<.25,"80-400 ms: mean cross-band residual correlation <0.25");
}
static void flutter() {
    TailHist h;h.nBins=1600;h.binDur=.001;h.h.resize(1);h.h2.resize(1);
    for(int b=0;b<NB;++b)for(size_t k=0;k<h.nBins;++k) {
        const bool echo=k>=80&&k<=400&&k%40==0;
        const double value=k<40?0.:std::exp(-6.*std::log(10.)*(k-40)*.001)* (echo?100.:.05);
        h.h[0][b].push_back(value);h.h2[0][b].push_back(value*value/(echo?64.:2.));
    }
    auto old=h;finalizeTail(old,false);finalizeTail(h);bool preserved=true;
    for(int b=0;b<NB;++b)for(size_t k=80;k<=400;k+=40)preserved&=h.h[0][b][k]==old.h[0][b][k];
    check(preserved,"strong reliable flutter reflections between parallel walls retain exact arrival-bin levels");
    Scene box;box.sr=48000;box.length=1.;box.height=3.;box.mats={presetByName("Open"),presetByName("Concrete")};box.floorMat=0;box.ceilMat=-1;
    box.walls={{0,0,0,20,1},{10,20,10,0,1}};
    Transducer source,receiver;source.pos={2,10,1.5};receiver.pos={3,10,1.5};box.sources={source};box.receivers={receiver};
    EngineConfig config;config.erOrder=4;config.tailThread=false;config.tailRays=48000;
    Engine engine(box,config);engine.setSoloMask(2);
    std::vector<float> input(8192),output(8192);const float* ip[]={input.data()};float* op[]={output.data()};
    engine.process(ip,op,2048);std::fill(output.begin(),output.end(),0.f);input[0]=1.;engine.process(ip,op,(int)input.size());
    bool arrivals=true;
    for(double distance:{35.,39.,41.}) {
        const size_t center=(size_t)std::lround(distance/speedOfSound(box.tempC)*box.sr);float peak=0;
        for(size_t k=center-8;k<center+9;++k)peak=std::max(peak,std::abs(output[k]));
        arrivals&=peak>.005f;
    }
    check(arrivals,"parallel concrete walls retain distinct late flutter arrivals at geometric 35/39/41 m path delays");
}
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {
        for(const auto& room:verbp::builtinRooms())if(room.name=="Bedroom"||room.name.find("Studio live")!=std::string::npos||room.name.find("Concert hall")!=std::string::npos) {
            std::istringstream text(room.text);auto s=parseScene(text,room.name);s.sources.resize(1);if(s.receivers.size()>2)s.receivers.resize(2);s.sr=48000;
            quality(s,room.name.c_str());checkEnvelope(s,room.name.c_str());
        }
        flutter();
    }catch(const std::exception& e){std::printf("Exception: %s\n",e.what());++failures;}
    std::printf("Tail transition: %d PASS / %d FAIL\n",passes,failures);return failures?1:0;
}
