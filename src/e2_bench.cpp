// E2 cost bench (tool only). Engine-level per-block timing with Freeze OFF, and a
// class-level micro bench of the hold component when off. Built against pre-E2 and E2 sources.
#include "rt_engine.hpp"
#include "layouts.hpp"
#include "creative_wet.hpp"
#include "../plugin/RoomLibrary.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>
using namespace verb;
static double pct(std::vector<double> v,double p){std::sort(v.begin(),v.end());return v[(size_t)std::min<double>(v.size()-1,p*(v.size()-1))];}
int main(int argc,char** argv){
    const int block=argc>1?std::atoi(argv[1]):256;const int sources=argc>2?std::atoi(argv[2]):4;const int blocks=argc>3?std::atoi(argv[3]):3000;
    const bool creative=argc>4?std::atoi(argv[4])!=0:true;
    auto lib=verbp::builtinRooms();auto it=std::find_if(lib.begin(),lib.end(),[](const auto& r){return r.name=="Concert hall (audience zone)";});
    std::istringstream text(it->text);auto sc=parseScene(text,"bench");
    while((int)sc.sources.size()<sources){auto s=sc.sources[0];s.pos.x+=.5*sc.sources.size();sc.sources.push_back(s);}sc.sources.resize(sources);
    sc.receivers=makeReceiverArray(Layout::Surround714,sc.listenerPos,.1);
    EngineConfig cfg;cfg.erOrder=1;cfg.tailRays=20000;cfg.tailThread=false;cfg.sharedTail=sources>1;Engine e(sc,cfg);
#ifdef E2_NEW
    e.setFreezeAvailable(creative);
#endif
    e.setCreativeWet(0,100,false,0,300);
    const int S=e.numSources(),R=e.numReceivers();std::vector<std::vector<float>> in(S,std::vector<float>(block)),out(R,std::vector<float>(block));
    std::vector<const float*> ip(S);std::vector<float*> op(R);for(int s=0;s<S;++s)ip[s]=in[s].data();for(int r=0;r<R;++r)op[r]=out[r].data();
    std::vector<double> t;t.reserve(blocks);uint32_t rng=1;
    for(int k=0;k<blocks+200;++k){for(auto& v:in)for(float& x:v){rng=rng*1664525u+1013904223u;x=((rng>>9)*(1.f/8388608.f)-.5f)*.05f;}
        const auto a=std::chrono::steady_clock::now();e.process(ip.data(),op.data(),block);const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-a).count();if(k>=200)t.push_back(ms);}
    double mean=0;for(double x:t)mean+=x;mean/=t.size();
    std::printf("ENGINE S=%d R=%d block=%d creative=%d mean %.4f ms median %.4f p99 %.4f worst %.4f budget %.4f\n",S,R,block,(int)creative,mean,pct(t,.5),pct(t,.99),pct(t,1),1000.*block/48000);
    // class-level: the hold component alone, Freeze off, per 256-sample block at R channels
    {
#ifdef E2_NEW
        TailFreeze h;h.prepare(48000,R,128);h.setArmed(creative);std::vector<float> tail((size_t)R*128,.01f),held((size_t)R*128);
        std::vector<double> c;for(int k=0;k<4000;++k){const auto a=std::chrono::steady_clock::now();for(int q=0;q<block;q+=128)h.process(tail.data(),held.data(),128,R,std::min(128,block-q),false);c.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-a).count());}
#else
        TailHold h;h.prepare(48000,R);std::vector<double> c;volatile float sink=0;
        for(int k=0;k<4000;++k){const auto a=std::chrono::steady_clock::now();for(int j=0;j<block;++j){h.begin(false);for(int r=0;r<R;++r)sink=sink+h.process(r,.01f*(float)((j*7+r)%13));h.end();}c.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-a).count());}
#endif
        double m=0;for(double x:c)m+=x;m/=c.size();std::printf("HOLD-OFF R=%d block=%d mean %.3f us median %.3f us p99 %.3f us\n",R,block,m,pct(c,.5),pct(c,.99));
    }
    return 0;
}
