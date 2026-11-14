#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace verbp {
using PathPoint = std::array<double, 3>;
struct MovementPath {
    std::vector<PathPoint> points;
    bool closed = false, smooth = false, orient = false, enabled = true;
    int laps = 1;
    bool operator==(const MovementPath& b) const { return points == b.points && closed == b.closed && smooth == b.smooth && orient == b.orient && enabled == b.enabled && laps == b.laps; }
};
inline bool validPath(const MovementPath& p) {
    if (p.laps < 1 || p.laps > 64) return false;
    if (p.points.empty()) return true;
    if (p.points.size() < 2 || p.points.size() > 64) return false;
    for (const auto& q : p.points) for (int k=0;k<3;++k)
        if (!std::isfinite(q[k]) || q[k] < (k==2?0:-200) || q[k] > (k==2?30:200)) return false;
    return true;
}
inline PathPoint pathMix(const PathPoint& a,const PathPoint& b,double u) {
    return {a[0]+(b[0]-a[0])*u,a[1]+(b[1]-a[1])*u,a[2]+(b[2]-a[2])*u};
}
inline double pathDistance(const PathPoint& a,const PathPoint& b) { return std::hypot(std::hypot(a[0]-b[0],a[1]-b[1]),a[2]-b[2]); }
// Centripetal Barry-Goldman interpolation. Open end spans use the three-point
// knot parabola (no phantom), preserving its tangent at the neighbouring interior span.
inline PathPoint pathCurve(const MovementPath& p,int segment,double u) {
    const int n=(int)p.points.size();
    auto at=[&](int i)->PathPoint { if(p.closed)return p.points[(size_t)((i%n+n)%n)];
        if(i<0)return pathMix(p.points[1],p.points[0],2);
        if(i>=n)return pathMix(p.points[(size_t)n-2],p.points.back(),2);
        return p.points[(size_t)i]; };
    const auto a=at(segment-1),b=at(segment),c=at(segment+1),d=at(segment+2);
    if(!p.smooth)return pathMix(b,c,u);
    if(!p.closed&&n==2)return pathMix(b,c,u);
    if(pathDistance(b,c)<1e-10)return b;
    auto blend=[](const PathPoint& x,const PathPoint& y,double lo,double hi,double v){return pathMix(x,y,(v-lo)/(hi-lo));};
    if(!p.closed&&segment==0){
        const double t1=0,t2=std::max(1e-5,std::sqrt(pathDistance(b,c))),t3=t2+std::max(1e-5,std::sqrt(pathDistance(c,d))),t=t1+u*(t2-t1);
        return blend(blend(b,c,t1,t2,t),blend(c,d,t2,t3,t),t1,t3,t);
    }
    const double t0=0,t1=std::max(1e-5,std::sqrt(pathDistance(a,b))),t2=t1+std::max(1e-5,std::sqrt(pathDistance(b,c))),t3=t2+std::max(1e-5,std::sqrt(pathDistance(c,d))),t=t1+u*(t2-t1);
    if(!p.closed&&segment==n-2)return blend(blend(a,b,t0,t1,t),blend(b,c,t1,t2,t),t0,t2,t);
    auto a1=blend(a,b,t0,t1,t),a2=blend(b,c,t1,t2,t),a3=blend(c,d,t2,t3,t);
    return blend(blend(a1,a2,t0,t2,t),blend(a2,a3,t1,t3,t),t1,t2,t);
}
struct CompiledPath {
    std::vector<PathPoint> points;
    std::vector<double> lengths;
    bool enabled=false,orient=false,closed=false;
    int laps=1;
    void build(const MovementPath& p) {
        points.clear(); lengths.clear(); enabled=p.enabled && p.points.size()>=2;orient=p.orient;closed=p.closed;laps=std::clamp(p.laps,1,64);
        if(p.points.size()<2)return;
        const int segments=(int)p.points.size()-(p.closed?0:1), steps=p.smooth?128:1;
        points.push_back(p.points.front()); lengths.push_back(0);
        for(int s=0;s<segments;++s)for(int j=1;j<=steps;++j){
            auto q=(p.closed && s==segments-1 && j==steps)?p.points.front():pathCurve(p,s,(double)j/steps);
            lengths.push_back(lengths.back()+pathDistance(points.back(),q));points.push_back(q);
        }
    }
    double length() const { return lengths.empty()?0:lengths.back(); }
    PathPoint evaluate(double u,double* yaw=nullptr) const noexcept {
        if(points.empty())return {};
        const double clamped=std::clamp(u,0.0,1.0);
        double frac=clamped;
        if(closed){const double phase=clamped*laps;frac=phase-std::floor(phase);}
        if(closed && frac==0.0){
            size_t direction=1;
            while(direction+1<points.size() && lengths[direction]-lengths[direction-1]<=1e-12)++direction;
            if(yaw && lengths[direction]-lengths[direction-1]>1e-12)*yaw=std::atan2(points[direction][1]-points[direction-1][1],points[direction][0]-points[direction-1][0])*180.0/3.14159265358979323846;
            return points.front();
        }
        const double distance=frac*length();
        auto it=std::upper_bound(lengths.begin(),lengths.end(),distance);
        const size_t i=std::clamp<size_t>((size_t)(it-lengths.begin()),1,points.size()-1);
        const double span=lengths[i]-lengths[i-1];
        if(yaw && lengths[i]-lengths[i-1]>1e-12)*yaw=std::atan2(points[i][1]-points[i-1][1],points[i][0]-points[i-1][0])*180.0/3.14159265358979323846;
        return pathMix(points[i-1],points[i],span>1e-12?(distance-lengths[i-1])/span:0);
    }
};
// SPSC triple mailbox. Producer owns back, consumer owns front; only middle is exchanged.
// Producer is serialised by the processor's control lock. No memory is freed by consume().
class PathMailbox {
    std::array<CompiledPath,3> slots;
    std::atomic<int> middle{1};
    int back=2,front=0;
public:
    void publish(const MovementPath& p) { slots[(size_t)back].build(p);back=middle.exchange(back|4)&3; }
    const CompiledPath& consume() noexcept { if(middle.load()&4)front=middle.exchange(front)&3;return slots[(size_t)front]; }
};
}
