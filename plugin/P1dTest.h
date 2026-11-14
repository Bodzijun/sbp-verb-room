#pragma once
#include <cstring>
// Frozen pre-P1d reflection rule, used only to prove unchanged cases and measure the fix.
static verbp::PathPoint p1dReflection(const verbp::MovementPath& p,int segment,double u) {
    using namespace verbp;const int n=(int)p.points.size();
    auto at=[&](int i)->PathPoint {if(p.closed)return p.points[(size_t)((i%n+n)%n)];if(i<0)return pathMix(p.points[1],p.points[0],2);if(i>=n)return pathMix(p.points[(size_t)n-2],p.points.back(),2);return p.points[(size_t)i];};
    const auto a=at(segment-1),b=at(segment),c=at(segment+1),d=at(segment+2);
    if(!p.smooth)return pathMix(b,c,u);if(pathDistance(b,c)<1e-10)return b;
    const double t0=0,t1=std::max(1e-5,std::sqrt(pathDistance(a,b))),t2=t1+std::max(1e-5,std::sqrt(pathDistance(b,c))),t3=t2+std::max(1e-5,std::sqrt(pathDistance(c,d))),t=t1+u*(t2-t1);
    auto blend=[](const PathPoint& x,const PathPoint& y,double lo,double hi,double v){return pathMix(x,y,(v-lo)/(hi-lo));};
    auto a1=blend(a,b,t0,t1,t),a2=blend(b,c,t1,t2,t),a3=blend(c,d,t2,t3,t);
    return blend(blend(a1,a2,t0,t2,t),blend(a2,a3,t1,t3,t),t1,t2,t);
}
static void testP1dGeometry() {
    using namespace verbp;MovementPath arc;arc.smooth=true;
    constexpr double pi=3.14159265358979323846;
    for(int j=0;j<5;++j){double a=j*20*pi/180;arc.points.push_back({10*std::cos(a),10*std::sin(a),1.5});}
    double radial=0,oldRadial=0,displacement=0,minCross=1e30,oldMin=1e30,oldMax=-1e30;
    for(int s:{0,3})for(int j=0;j<=1000;++j){double u=j/1000.;auto q=pathCurve(arc,s,u),v=p1dReflection(arc,s,u);radial=std::max(radial,std::abs(std::hypot(q[0],q[1])-10));oldRadial=std::max(oldRadial,std::abs(std::hypot(v[0],v[1])-10));displacement=std::max(displacement,pathDistance(q,v));if(j>0&&j<1000){auto a=pathCurve(arc,s,u-.001),b=pathCurve(arc,s,u+.001);minCross=std::min(minCross,(q[0]-a[0])*(b[1]-q[1])-(q[1]-a[1])*(b[0]-q[0]));a=p1dReflection(arc,s,u-.001);b=p1dReflection(arc,s,u+.001);double c=(v[0]-a[0])*(b[1]-v[1])-(v[1]-a[1])*(b[0]-v[0]);oldMin=std::min(oldMin,c);oldMax=std::max(oldMax,c);}}
    std::printf("P1d arc radial new %.12g old %.12g displacement %.12g m cross new %.12g old [%.12g,%.12g]\n",radial,oldRadial,displacement,minCross,oldMin,oldMax);
    check(minCross>0&&oldMin<0&&oldMax>0,"P1d both end spans have no inflection; old rule has one");
    check(radial<.005&&radial<oldRadial/10,"P1d R10m arc radial error below 5mm and at least 10x smaller");
    MovementPath uneven;uneven.smooth=true;for(double degrees:{0.,13.,37.,62.,85.}){double a=degrees*pi/180;uneven.points.push_back({10*std::cos(a),10*std::sin(a),1.5+.01*degrees});}
    auto derivative=[&](int s,bool end){const double h=1e-5,dt=std::max(1e-5,std::sqrt(pathDistance(uneven.points[(size_t)s],uneven.points[(size_t)s+1])));auto a=pathCurve(uneven,s,end?1:0),b=pathCurve(uneven,s,end?1-h:h),c=pathCurve(uneven,s,end?1-2*h:2*h);PathPoint v{};for(int k=0;k<3;++k)v[k]=(end?3*a[k]-4*b[k]+c[k]:-3*a[k]+4*b[k]-c[k])/(2*h*dt);return v;};
    double c1=0;for(int joint:{1,3})c1=std::max(c1,pathDistance(derivative(joint-1,true),derivative(joint,false)));
    std::printf("P1d C1 knot-parameter derivative max mismatch %.12g\n",c1);
    check(c1<1e-6,"P1d C1 at second/penultimate point in centripetal knot parameter");
    bool unchanged=true;
    for(bool closed:{false,true})for(bool smooth:{false,true}){if(!closed&&smooth)continue;auto p=uneven;p.closed=closed;p.smooth=smooth;CompiledPath table;table.build(p);std::vector<PathPoint> points{p.points.front()};std::vector<double>lengths{0};int segments=(int)p.points.size()-(closed?0:1),steps=smooth?128:1;for(int s=0;s<segments;++s)for(int j=1;j<=steps;++j){auto q=closed&&s==segments-1&&j==steps?p.points.front():p1dReflection(p,s,(double)j/steps);lengths.push_back(lengths.back()+pathDistance(points.back(),q));points.push_back(q);}unchanged&=points.size()==table.points.size()&&lengths.size()==table.lengths.size()&&std::memcmp(points.data(),table.points.data(),points.size()*sizeof(PathPoint))==0&&std::memcmp(lengths.data(),table.lengths.data(),lengths.size()*sizeof(double))==0;}
    check(unchanged,"P1d closed paths and polylines retain identical point/length bits");
    bool interiors=true;for(int s:{1,2})for(int j=0;j<=128;++j){auto a=pathCurve(uneven,s,j/128.),b=p1dReflection(uneven,s,j/128.);interiors&=std::memcmp(a.data(),b.data(),sizeof(PathPoint))==0;}
    check(interiors,"P1d interior spans retain identical bits");
    auto two=uneven;two.points.resize(2);bool straight=true;for(int j=0;j<=128;++j){auto a=pathCurve(two,0,j/128.),b=pathMix(two.points[0],two.points[1],j/128.);straight&=std::memcmp(a.data(),b.data(),sizeof(PathPoint))==0;}check(straight,"P1d two-point open smooth path is exactly linear");
}
