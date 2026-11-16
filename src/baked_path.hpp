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
struct BakedTimingKey {
    double t = 0.0, f = 0.0;
    uint8_t easing = 0;
    bool operator==(const BakedTimingKey& o) const noexcept { return t == o.t && f == o.f && easing == o.easing; }
};
// Control-thread description, one physical lap. object=-1: listener; otherwise engine point index.
struct BakedPath {
    int object=-1,group=-1;
    bool closed=false;
    std::vector<std::array<double,4>> poses;
    std::vector<double> distance;
    bool recorded=false;
    uint64_t recording=0;
    double startU=0.0, endU=1.0;
    int laps=1;
    uint64_t timingSig=0;
    std::vector<BakedTimingKey> timingKeys;
    // E28: the table coordinate IS the group Position u (distance[i]=u_i*D, startU=0,endU=1,laps=1): the description of an owner that
    // looks at a moving member of the same Group. Only selects the joint-curve station placement; the maths of at() is unchanged.
    bool uIndexed=false;
    bool operator==(const BakedPath& p) const {return object==p.object&&group==p.group&&closed==p.closed&&recorded==p.recorded&&recording==p.recording&&uIndexed==p.uIndexed&&startU==p.startU&&endU==p.endU&&laps==p.laps&&timingSig==p.timingSig&&poses==p.poses&&distance==p.distance&&timingKeys==p.timingKeys;}
    bool hasTiming() const noexcept { return startU > 0.0 || endU < 1.0 || !timingKeys.empty() || laps > 1; }
    double mapTime(double u) const noexcept {
        double c=std::clamp(u,0.,1.);
        if(startU>0.||endU<1.||!timingKeys.empty()){
            if(c<=startU)c=0.;
            else if(c>=endU)c=1.;
            else{
                const double wSpan=endU-startU;
                const double w=wSpan>0.?std::clamp((c-startU)/wSpan,0.,1.):1.;
                if(w<=0.)c=timingKeys.empty()?0.:timingKeys.front().f;
                else if(w>=1.)c=timingKeys.empty()?1.:timingKeys.back().f;
                else if(timingKeys.size()>=2){
                    auto it=std::upper_bound(timingKeys.begin(),timingKeys.end(),w,[](double val,const BakedTimingKey& k){return val<k.t;});
                    const size_t hi=std::clamp<size_t>((size_t)(it-timingKeys.begin()),1,timingKeys.size()-1);
                    const size_t lo=hi-1;
                    const double dt=timingKeys[hi].t-timingKeys[lo].t;
                    double s=dt>0.?std::clamp((w-timingKeys[lo].t)/dt,0.,1.):0.;
                    if(timingKeys[lo].easing==1)s=s*s;
                    else if(timingKeys[lo].easing==2){const double inv=1.-s;s=1.-inv*inv;}
                    else if(timingKeys[lo].easing==3)s=s*s*(3.-2.*s);
                    c=timingKeys[lo].f+s*(timingKeys[hi].f-timingKeys[lo].f);
                }else c=w;
            }
        }
        if(closed&&laps>1){const double phase=c*laps;c=phase-std::floor(phase);}
        return c;
    }
    std::array<double,4> at(double u) const {
        if(poses.size()<2||distance.size()!=poses.size())return {};
        const double d=(hasTiming()?mapTime(u):std::clamp(u,0.,1.))*distance.back();
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
    std::vector<int> groupSources;
    std::vector<uint64_t> objRevisions;
    bool movesListener = false;
    uint64_t request=0,geometry=0,quality=0,job=0;
    std::vector<double> staticPose,patterns,u,energy,amplitude;
    std::vector<double> noiseDb,sumNoiseDb;
    int sources=0,receivers=0,discontinuities=0,selectedRays=0;
    std::vector<double> jointU, jointPoses, jointDist;
    double activeUStart=0., activeUEnd=1.;
    bool moves(int poseIndex)const noexcept {for(const auto&p:movingPaths)if(poseIndex==(p.object<0?0:p.object+1))return true;return false;}
    bool hasDeclaredJoint() const noexcept { return jointU.size()>=2; }
    void buildJointCurve() {
        jointU.clear(); jointPoses.clear(); jointDist.clear(); activeUStart=0.; activeUEnd=1.;
        if(movingPaths.empty())return;
        std::vector<double> cand={0.,1.};
        for(int i=1;i<256;++i)cand.push_back((double)i/256.);
        for(const auto& p:movingPaths){
            const double s0=std::clamp(p.startU,0.,1.), e0=std::clamp(p.endU,0.,1.);
            cand.push_back(s0); cand.push_back(e0);
            if(e0>s0){
                for(const auto& k:p.timingKeys)cand.push_back(std::clamp(s0+k.t*(e0-s0),0.,1.));
                const int sub=std::min(512,64*std::max(1,p.laps));
                for(int i=1;i<sub;++i)cand.push_back(s0+((double)i/(double)sub)*(e0-s0));
                if(p.closed&&p.laps>1)for(int l=1;l<p.laps;++l){
                    const double ul=s0+((double)l/(double)p.laps)*(e0-s0);
                    if(ul>1e-9)cand.push_back(ul-1e-9);
                    cand.push_back(ul);
                }
            }
        }
        std::sort(cand.begin(),cand.end());
        jointU.reserve(cand.size());
        for(double v:cand)if(jointU.empty()||v-jointU.back()>1e-12)jointU.push_back(v);
        if(jointU.size()<2)jointU={0.,1.};
        const size_t M=movingPaths.size(), N=jointU.size();
        jointPoses.resize(N*M*4);
        jointDist.assign(N,0.);
        for(size_t i=0;i<N;++i){
            for(size_t m=0;m<M;++m){
                auto v=movingPaths[m].at(jointU[i]);
                for(int k=0;k<4;++k)jointPoses[(i*M+m)*4+(size_t)k]=v[(size_t)k];
            }
            if(i>0){
                double sq=0.;
                for(size_t m=0;m<M;++m)for(int k=0;k<4;++k){
                    double d=jointPoses[(i*M+m)*4+(size_t)k]-jointPoses[((i-1)*M+m)*4+(size_t)k];
                    if(k==3)d=std::remainder(d,360.)*.01;
                    sq+=d*d;
                }
                jointDist[i]=jointDist[i-1]+std::sqrt(sq);
            }
        }
        size_t firstMove=1;
        while(firstMove<N && jointDist[firstMove]<=1e-12)++firstMove;
        if(firstMove>=N){activeUStart=0.;activeUEnd=1.;return;}
        activeUStart=jointU[firstMove-1];
        size_t lastMove=N-1;
        while(lastMove>1 && jointDist[lastMove]-jointDist[lastMove-1]<=1e-12)--lastMove;
        activeUEnd=jointU[lastMove];
    }
    std::vector<double> initialStationsForJoint(double roomMin) const {
        if(jointU.size()<2||jointDist.empty()||jointDist.back()<=1e-12||activeUEnd<=activeUStart)return {0.,1.};
        const double totalD=jointDist.back();
        const int intervals=std::clamp((int)std::ceil(totalD/std::clamp(.1*roomMin,.1,.5)),1,31);
        std::vector<double> out;
        out.push_back(activeUStart);
        for(int j=1;j<intervals;++j){
            const double target=totalD*(double)j/(double)intervals;
            auto it=std::upper_bound(jointDist.begin(),jointDist.end(),target);
            const size_t idx=std::clamp<size_t>((size_t)(it-jointDist.begin()),1,jointU.size()-1);
            const double span=jointDist[idx]-jointDist[idx-1];
            const double f=span>0.?std::clamp((target-jointDist[idx-1])/span,0.,1.):0.;
            const double uVal=jointU[idx-1]+f*(jointU[idx]-jointU[idx-1]);
            if(uVal>out.back()+1e-12 && uVal<activeUEnd-1e-12)out.push_back(uVal);
        }
        if(activeUEnd>out.back()+1e-12)out.push_back(activeUEnd);
        if(out.size()<2)return {0.,1.};
        return out;
    }
    bool projectDeclared(const double* pose,double& uOut,size_t* hint=nullptr) const noexcept {
        const size_t N=jointU.size(), M=movingPaths.size();
        if(N<2||M==0||jointPoses.size()<N*M*4)return false;
        double best=1e30, bestF=0.; size_t bestJ=0; bool bestTol=false;
        auto evalSeg=[&](size_t j,double& fOut,bool& tolOk)->double{
            double dot=0.,span=0.;
            const double* a=&jointPoses[((j-1)*M)*4];
            const double* b=&jointPoses[(j*M)*4];
            for(size_t m=0;m<M;++m){
                const int off=4*(movingPaths[m].object+1);
                for(int k=0;k<4;++k){
                    double delta=b[m*4+k]-a[m*4+k];
                    double v=pose[off+k]-a[m*4+k];
                    if(k==3){delta=std::remainder(delta,360.)*.01;v=std::remainder(v,360.)*.01;}
                    dot+=v*delta;span+=delta*delta;
                }
            }
            const double f=span>0.?std::clamp(dot/span,0.,1.):0.;
            double e=0.; bool ok=true;
            for(size_t m=0;m<M;++m){
                const int off=4*(movingPaths[m].object+1);
                double posSq=0.;
                for(int k=0;k<3;++k){
                    const double interp=a[m*4+k]+f*(b[m*4+k]-a[m*4+k]);
                    const double diff=pose[off+k]-interp;
                    posSq+=diff*diff;
                }
                const double yawInterp=a[m*4+3]+f*std::remainder(b[m*4+3]-a[m*4+3],360.);
                const double yawErr=std::abs(std::remainder(pose[off+3]-yawInterp,360.));
                e+=posSq+(yawErr*.01)*(yawErr*.01);
                if(posSq>.02*.02||yawErr>1.)ok=false;
            }
            fOut=f;tolOk=ok;return e;
        };
        size_t lo=1,hi=N;
        if(hint&&*hint>0&&*hint<N){lo=*hint>8?*hint-8:1;hi=std::min(N,*hint+9);}
        for(int pass=0;pass<2;++pass){
            for(size_t j=lo;j<hi;++j){
                double f=0.; bool ok=false;
                const double e=evalSeg(j,f,ok);
                if(e<best){best=e;bestF=f;bestJ=j;bestTol=ok;}
            }
            if((bestJ&&bestTol)||(lo==1&&hi==N))break;
            lo=1;hi=N;
        }
        if(hint)*hint=bestJ;
        if(!bestJ||!bestTol)return false;
        uOut=jointU[bestJ-1]+bestF*(jointU[bestJ]-jointU[bestJ-1]);
        return true;
    }
    // Joint metric: metres, with one degree of yaw worth one centimetre.
    // Full bounded search includes jumps/loops; hint breaks ties for continuity.
    bool projectJoint(const double* pose,double& phase,size_t* hint=nullptr) const noexcept {
        if(movingPaths.empty())return false;
        const auto& d=movingPaths.front().distance;
        if(d.size()<2||d.back()<=0)return false;
        double best=1e30,bestF=0;size_t bestJ=0;
        size_t lo=1,hi=d.size();
        if(hint&&*hint>0&&*hint<d.size()){lo=*hint>8?*hint-8:1;hi=std::min(d.size(),*hint+9);}
        for(int pass=0;pass<2;++pass){
        for(size_t j=lo;j<hi;++j){
            double dot=0,span=0;
            for(const auto&p:movingPaths)for(int k=0;k<4;++k){
                double delta=k==3?std::remainder(p.poses[j][k]-p.poses[j-1][k],360.)*.01:p.poses[j][k]-p.poses[j-1][k];
                double v=pose[4*(p.object+1)+k]-p.poses[j-1][k];if(k==3)v=std::remainder(v,360.)*.01;
                dot+=v*delta;span+=delta*delta;
            }
            double f=span>0?std::clamp(dot/span,0.,1.):0,e=0;
            for(const auto&p:movingPaths)for(int k=0;k<4;++k){
                double delta=k==3?std::remainder(p.poses[j][k]-p.poses[j-1][k],360.):p.poses[j][k]-p.poses[j-1][k];
                double v=pose[4*(p.object+1)+k]-p.poses[j-1][k]-f*delta;if(k==3)v=std::remainder(v,360.)*.01;e+=v*v;
            }
            if(e<best){best=e;bestF=f;bestJ=j;}
        }
        if(best<=.02*.02||(lo==1&&hi==d.size()))break;
        lo=1;hi=d.size();
        }
        if(hint)*hint=bestJ;
        if(!bestJ||best>.02*.02)return false;
        phase=(d[bestJ-1]+bestF*(d[bestJ]-d[bestJ-1]))/d.back();return true;
    }
    size_t stride() const {return (size_t)sources*receivers*7;}
    bool lookup(double phase,double* out) const noexcept {
        if(u.size()<2)return false;
        if(jointU.empty() && path.closed && phase>=1)phase=0;
        size_t j=std::clamp<size_t>((size_t)(std::upper_bound(u.begin(),u.end(),phase)-u.begin()),1,u.size()-1);
        double f=std::clamp((phase-u[j-1])/(u[j]-u[j-1]),0.,1.);size_t n=stride();
        for(size_t k=0;k<n;++k){double a=amplitude[(j-1)*n+k],b=amplitude[j*n+k],v=a+(b-a)*f;out[k]=v*v;}return true;
    }
    uint64_t hash() const {uint64_t h=1469598103934665603ull;auto mix=[&](const void* v,size_t n){auto p=(const unsigned char*)v;for(size_t j=0;j<n;++j){h^=p[j];h*=1099511628211ull;}};for(double v:u)mix(&v,sizeof v);for(double v:energy)mix(&v,sizeof v);return h;}
};
}

