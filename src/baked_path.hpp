#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
namespace verb {
// Initial nodes are only part of the job: independent midpoint validation follows.
inline int bakedProgressPercent(size_t initialDone,size_t initialTotal,size_t validated,size_t validations) {
    if(initialDone<initialTotal||validations==0)return initialTotal?std::min(45,(int)(45*initialDone/initialTotal)):0;
    return 45+std::min(54,(int)(54*validated/validations));
}
enum class BakedRefinement { Accept, Split, Approximate };
inline BakedRefinement bakedRefinement(double error,size_t nodes,double a,double b) {
    if(error<=1)return BakedRefinement::Accept;
    const double mid=(a+b)*.5;
    return nodes<64&&mid>a&&mid<b?BakedRefinement::Split:BakedRefinement::Approximate;
}
// Control-thread description, one physical lap. object=-1: listener; otherwise engine point index.
struct BakedPath {
    int object=-1,group=-1;
    bool closed=false;
    std::vector<std::array<double,4>> poses;
    std::vector<double> distance;
    bool operator==(const BakedPath& p) const {return object==p.object&&group==p.group&&closed==p.closed&&poses==p.poses&&distance==p.distance;}
    std::array<double,4> at(double u) const {
        if(poses.size()<2||distance.size()!=poses.size())return {};
        const double d=std::clamp(u,0.,1.)*distance.back();
        size_t j=std::clamp<size_t>((size_t)(std::upper_bound(distance.begin(),distance.end(),d)-distance.begin()),1,poses.size()-1);
        const double span=distance[j]-distance[j-1],f=span>0?(d-distance[j-1])/span:0;
        auto p=poses[j-1];for(int k=0;k<3;++k)p[k]+=(poses[j][k]-p[k])*f;
        p[3]+=std::remainder(poses[j][3]-p[3],360.)*f;return p;
    }
    // Nearest path segment; bounded, allocation-free. No sqrt on the search path.
    bool project(const double* p,double& u,size_t* hint=nullptr) const noexcept {
        double best=1e30;size_t jBest=0;double fBest=0;
        size_t lo=1,hi=poses.size();if(hint&&*hint>0&&*hint<poses.size()){lo=*hint>4?*hint-4:1;hi=std::min(poses.size(),*hint+5);}
        for(size_t pass=0;pass<2;++pass){for(size_t j=lo;j<hi;++j){double dot=0,span=0;for(int k=0;k<3;++k){double d=poses[j][k]-poses[j-1][k];dot+=(p[k]-poses[j-1][k])*d;span+=d*d;}
            double f=span>0?std::clamp(dot/span,0.,1.):0,e=0;for(int k=0;k<3;++k){double d=p[k]-poses[j-1][k]-f*(poses[j][k]-poses[j-1][k]);e+=d*d;}if(e<best){best=e;jBest=j;fBest=f;}}
        if(best<=.02*.02||(lo==1&&hi==poses.size()))break;lo=1;hi=poses.size();}
        if(hint)*hint=jBest;
        if(!jBest||best>.02*.02||distance.back()<=0)return false;
        u=(distance[jBest-1]+fBest*(distance[jBest]-distance[jBest-1]))/distance.back();
        double yaw=poses[jBest-1][3]+std::remainder(poses[jBest][3]-poses[jBest-1][3],360.)*fBest;
        return std::abs(std::remainder(p[3]-yaw,360.))<=1.;
    }
};
struct BakedTable {
    BakedPath path;
    std::vector<BakedPath> movingPaths;
    std::vector<std::array<double,3>> residualSegments;
    uint64_t request=0,geometry=0,quality=0,job=0;
    std::vector<double> staticPose,patterns,u,energy,amplitude;
    std::vector<double> noiseDb,sumNoiseDb;
    int sources=0,receivers=0,discontinuities=0,selectedRays=0;
    bool moves(int poseIndex)const noexcept {for(const auto&p:movingPaths)if(poseIndex==(p.object<0?0:p.object+1))return true;return false;}
    size_t stride() const {return (size_t)sources*receivers*7;}
    bool lookup(double phase,double* out) const noexcept {
        if(u.size()<2)return false;
        if(path.closed && phase>=1)phase=0;
        size_t j=std::clamp<size_t>((size_t)(std::upper_bound(u.begin(),u.end(),phase)-u.begin()),1,u.size()-1);
        double f=std::clamp((phase-u[j-1])/(u[j]-u[j-1]),0.,1.);size_t n=stride();
        for(size_t k=0;k<n;++k){double a=amplitude[(j-1)*n+k],b=amplitude[j*n+k],v=a+(b-a)*f;out[k]=v*v;}return true;
    }
    uint64_t hash() const {uint64_t h=1469598103934665603ull;auto mix=[&](const void* v,size_t n){auto p=(const unsigned char*)v;for(size_t j=0;j<n;++j){h^=p[j];h*=1099511628211ull;}};for(double v:u)mix(&v,sizeof v);for(double v:energy)mix(&v,sizeof v);return h;}
};
}

