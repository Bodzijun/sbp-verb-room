#include "creative_wet.hpp"
#include "dsp.hpp"
#include "rt_engine.hpp"
#include "../plugin/RoomLibrary.h"
#include <cstdio>
#include <chrono>
#include <numeric>
#include <random>
using namespace verb;
static int failed=0,passed=0;
static void check(bool ok,const char* msg){std::printf("[%s] %s\n",ok?"PASS":"FAIL",msg);ok?++passed:++failed;}
static double octave(const std::vector<float>& x,int start,int count,double hz,double sr){
 const size_t n=nextPow2((size_t)count);std::vector<cd> a(n);
 for(int i=0;i<count;++i)a[i]=x[start+i];fft(a,false);double e=0;
 for(size_t k=1;k<n/2;++k)if(k*sr/n>=hz/std::sqrt(2.)&&k*sr/n<hz*std::sqrt(2.))e+=std::norm(a[k]);
 return e/n/count;
}

static std::vector<float> engineIR(Scene sc,int solo,double pd=0,double width=100,int warm=4800) {
 EngineConfig c;c.erOrder=1;c.tailThread=false;c.tailRays=8000;
 Engine e(sc,c);e.setEarlyDiffusion(0);e.setSoloMask(solo);e.setCreativeWet(pd,width,false,0,300);
 std::vector<float> input(4096),output(4096);const float* ip[]={input.data()};float* op[]={output.data()};
 for(int n=0;n<warm;n+=128){std::fill(input.begin(),input.end(),0);e.process(ip,op,std::min(128,warm-n));}
 std::vector<float> ir(24000);for(int n=0;n<24000;n+=128){std::fill(input.begin(),input.end(),0);if(n==0)input[0]=1;e.process(ip,op,std::min(128,24000-n));std::copy_n(output.data(),std::min(128,24000-n),ir.data()+n);}return ir;
}
static void engineChecks() {
 auto sc=verbp::detail::box(8,6,3,"Brick","Parquet_floor","Plaster",2,2,1,5,3,1.4);sc.receivers.resize(1);sc.receivers[0].pos=sc.listenerPos;sc.receivers[0].pattern=0;sc.sources[0].pattern=0;sc.sources[0].azimuthDeg=0;sc.listenerYaw=0;sc.diffraction=false;
 const auto early=engineIR(sc,2),delayed=engineIR(sc,2,50),direct=engineIR(sc,1),directDelayed=engineIR(sc,1,50);
 double directError=0,earlyError=0;for(size_t i=0;i<early.size();++i){directError=std::max(directError,std::abs((double)direct[i]-directDelayed[i]));earlyError=std::max(earlyError,std::abs((double)delayed[i]-(i>=2400?early[i-2400]:0)));}
 std::printf("engine predelay early-shift error %.12g FS, direct error %.12g FS\n",earlyError,directError);
 check(earlyError<1e-7,"integrated predelay shifts every early sample exactly 50 ms");check(directError==0,"integrated predelay leaves direct bit-exact");
 auto peak=[](const auto& v){return (int)(std::max_element(v.begin(),v.end(),[](float a,float b){return std::abs(a)<std::abs(b);})-v.begin());};
 auto big=sc;scaleRoomGeometry(big,1.5);for(auto& t:big.sources)t.pos=t.pos*1.5;for(auto& t:big.receivers)t.pos=t.pos*1.5;big.listenerPos=big.listenerPos*1.5;
 auto irBig=engineIR(big,2);const int a=peak(early),b=peak(irBig);std::printf("Size analytic reflection peak %d -> %d, expected %.1f\n",a,b,a*1.5);check(std::abs(b-a*1.5)<=1.5,"Size rectangular reflection delay scales by k within interpolation sample");
 // Derivative check against the natural sine derivative; crossfading fixed taps
 // contributes at most 2*A/fade, independent of sweep velocity (no Doppler read).
 WetPredelay pd;pd.prepare(48000,1);double worst=0;float previous=0;
 for(int n=0;n<48000;++n){pd.begin((n/113)%251);const float y=pd.process(0,.1f*(float)std::sin(2*kPi*440*n/48000.));pd.end();if(n)worst=std::max(worst,std::abs((double)y-previous));previous=y;}
 std::printf("predelay 0..250 sweep max adjacent step %.9f FS\n",worst);check(worst<=2*.1*std::sin(kPi*440/48000.)+2*.1/960+1e-6,"predelay sweep derivative bounded by natural sine plus 20ms crossfade");
 for(double rate:{44100.,48000.,88200.,96000.,176400.,192000.}) {
  auto scene=sc;scene.sr=rate;EngineConfig config;config.erOrder=1;config.tailThread=false;config.tailRays=1000;Engine e(scene,config);e.setCreativeWet(250,200,true,24,50);
  std::vector<float> in(4096,.01f),out(4096);const float* ip[]={in.data()};float* op[]={out.data()};bool finite=true;
  for(int size:{1,2,3,63,64,127,128,129,256,511,512,1023,2048,4096}){e.process(ip,op,size);for(int i=0;i<size;++i)finite&=std::isfinite(out[i]);}
  check(finite,"all creative controls finite at changing 1..4096 blocks and common 44.1..192k rates");
 }
}


static void realFreezeCheck() {
 auto rooms=verbp::builtinRooms();auto room=std::find_if(rooms.begin(),rooms.end(),[](const auto& r){return r.name=="Concert hall (audience zone)";});std::istringstream text(room->text);auto sc=parseScene(text,"hold");sc.receivers=makeReceiverArray(Layout::StereoORTF,sc.listenerPos,.1);
 EngineConfig config;config.erOrder=1;config.tailRays=12000;config.tailThread=false;Engine engine(sc,config);engine.setSoloMask(4);
 std::mt19937 random(19);std::uniform_real_distribution<float> noise(-.1f,.1f);std::vector<float> in(128),left(128),right(128),capture(63*48000);const float* ip[]={in.data()};float* op[]={left.data(),right.data()};
 for(int pos=0;pos<(int)capture.size();pos+=128){const int n=std::min(128,(int)capture.size()-pos);const bool frozen=pos>=3*48000;engine.setCreativeWet(0,100,frozen,0,300);for(int i=0;i<n;++i)in[i]=frozen?0:noise(random);engine.process(ip,op,n);std::copy_n(left.data(),n,capture.data()+pos);}
 bool stable=true,level=true;
 for(int b=0;b<7;++b){const double hz=125*std::pow(2.,b);const double a=octave(capture,5*48000,4*48000,hz,48000),z=octave(capture,58*48000,4*48000,hz,48000),prior=octave(capture,(int)(2.75*48000),12000,hz,48000);const double db=10*std::log10(z/a),jump=10*std::log10(a/prior);std::printf("real Hall freeze octave %.0f: drift %+.4f dB, capture level %+.4f dB\n",hz,db,jump);stable&=std::abs(db)<=1;level&=std::abs(jump)<=1;}
 check(stable,"real Concert hall held tail every octave +/-1 dB over 60 s");check(level,"real Concert hall hold captured octave levels within +/-1 dB of engagement window");
}

int main(){std::setvbuf(stdout,nullptr,_IONBF,0);
 for(double sr:{44100.,48000.,96000.,192000.}){
  WetPredelay d;d.prepare(sr,2);for(int n=0;n<(int)(.03*sr);++n){d.begin(123);d.process(0,0);d.process(1,0);d.end();}
  int onset=-1;for(int n=0;n<(int)(.3*sr);++n){d.begin(123);const float y=d.process(0,n==0?1.f:0);d.process(1,0);d.end();if(y&&onset<0)onset=n;}
  std::printf("predelay sr %.0f onset %d expected %d\n",sr,onset,(int)std::lround(.123*sr));check(onset==(int)std::lround(.123*sr),"predelay exact rounded sample at each rate");
  WetDuck duck;duck.prepare(sr);duck.settings(300);float gain=0;for(int n=0;n<(int)sr;++n)gain=duck.process(.1f,12);
  check(std::abs(20*std::log10(gain)+12)<.01,"duck steady reduction matches 12 dB");
  for(int n=0;n<(int)(.3*sr);++n)gain=duck.process(0,12);
  std::printf("duck after 300 ms %.5f dB\n",duck.reductionDb());check(std::abs(duck.reductionDb()-12/std::exp(1.))<.02,"duck release time constant matches analytic 1/e");
  for(int n=0;n<(int)(10*sr);++n)gain=duck.process(0,0);check(gain==1,"duck zero is exact unity after release");
 }
 {float l=.7f,r=-.2f,ol=l,orr=r;widthPair(l,r,1);check(l==ol&&r==orr,"width 100 is bit transparent");widthPair(l,r,0);check(l==r&&l==(ol+orr)*.5f,"width 0 is mono mid");l=1;r=-1;widthPair(l,r,2);check(std::abs(l-std::sqrt(1.6f))<1e-6&&r==-l,"width 200 side coefficient 2 with -3.98 dB pair normalisation; power <= +3 dB");}
 {auto room=verbp::detail::box(8,6,3,"Brick","Parquet_floor","Plaster",2,3,1.5,5,3,1.5);const auto stored=serializeScene(room);auto smaller=room,larger=room;
  scaleRoomGeometry(smaller,.5);scaleRoomGeometry(larger,1.6);
  check(serializeScene(room)==stored&&std::abs(larger.height-4.8)<1e-12&&larger.walls[1].bx==12.8,"Size scales copy, preserves authoring geometry");
  auto a=estimateRoomRT60(smaller),b=estimateRoomRT60(room),c=estimateRoomRT60(larger);bool monotonic=true;for(int k=0;k<7;++k)monotonic&=a[k]<b[k]&&b[k]<c[k];check(monotonic,"Size Eyring octave RT60 grows monotonically");
 }
 engineChecks();
 realFreezeCheck();
 // E2: the class-level granular TailHold assertions moved to e2_freeze_test.cpp (TailFreeze),
 // each with an equal or stricter counterpart; the removed block is kept in out/E2/.
 std::printf("TOTAL %d PASS %d FAIL\n",passed,failed);return failed?1:0;
}
