// Shared 2.5D geometry helpers: surfaces are vertical wall segments (ids 0..W-1), floor (W), ceiling (W+1).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>

#include "scene.hpp"

namespace verb {

struct SoftEdgeProfile { uint64_t legs=0,lit=0,shadow=0,walls=0,vertices=0,junctionRays=0,knife=0,powers=0,validationScans=0,boxes=0; };
inline thread_local SoftEdgeProfile softProfile;
inline thread_local bool profilingSoftLeg=false;
#define E5B_COUNT(field,n) do { if(profilingSoftLeg)softProfile.field+=(n); } while(0)



struct Geo {
    const Scene& s;
    int W;  // wall count
    bool hasCeil;
    bool ampNonIncreasing = true;  // see the constructor: lets the search drop paths that are already too quiet
    double H;
    std::vector<const Zone*> plates;  // horizontal plates (zones with a height): surface ids W+2 ...
    // E5b immutable screening geometry, prepared with the scene, never in process().
    struct Rim { int surf; Vec3 A,B,u; double length; };
    std::vector<Rim> screenRims;
    std::vector<int> freeScreenWalls;
    std::vector<std::vector<int>> litRimsBySurface;
    std::vector<std::vector<unsigned char>> plateJoined;
    std::vector<unsigned char> wallBottomJoined,wallTopJoined;
    bool freeReflectorHeights=false;
    // Closed wall ends (E3): for wall end k = 2*wall + end (end 0 = a, 1 = b) the walls that meet there, as rays leaving
    // the point (angle in radians, wall index). A wall that ends at the point gives one ray (towards its other end); a
    // wall that merely passes through the point (a T junction) gives two. The wall's own ray is included. A free end
    // (nothing else there) has an empty list. Built once here (never on the audio path).
    struct JRay { double ang; int wall; };
    static constexpr double kJoinTol = 1e-3;  // metres: ends closer than this count as one point
    std::vector<int> jStart;
    std::vector<JRay> jRays;
    const JRay* jBegin(int wall, int end) const { return jRays.data() + jStart[(size_t)(2 * wall + end)]; }
    const JRay* jEnd(int wall, int end) const { return jRays.data() + jStart[(size_t)(2 * wall + end + 1)]; }
    // E3c: exact rectangular separating shells, constructed on the scene/control thread.
    // z slabs handle stacked door/window panels. Only complete, exactly joined sides qualify.
    struct Shell { double x0,x1,y0,y1,z0,z1; bool opaque; };
    std::vector<Shell> shells;
    void buildShells() {
        if (!ampNonIncreasing) return;
        std::vector<double> xs,ys,zs;
        for (const auto& w:s.walls) {
            if(w.ax==w.bx) xs.push_back(w.ax);
            if(w.ay==w.by) ys.push_back(w.ay);
            zs.push_back(w.z0);zs.push_back(w.z1);
        }
        auto unique=[](auto& v){std::sort(v.begin(),v.end());v.erase(std::unique(v.begin(),v.end()),v.end());};
        unique(xs);unique(ys);unique(zs);
        // Optional acceleration only: avoid expensive construction for highly detailed scenes.
        if(xs.size()>24 || ys.size()>24 || zs.size()>32) return;
        const size_t work=xs.size()*(xs.size()-1)/2 * (ys.size()*(ys.size()-1)/2) * zs.size() * (size_t)W;
        if(work>200000)return; // optional acceleration must also be cheap in editor scene snapshots
        for(size_t a=0;a<xs.size();++a)for(size_t b=a+1;b<xs.size();++b)
        for(size_t d=0;d<ys.size();++d)for(size_t e=d+1;e<ys.size();++e)
        for(size_t z=1;z<zs.size();++z){
            if(xs[b]-xs[a]<.01 || ys[e]-ys[d]<.01)continue;
            bool side[4]={},opaque[4]={};
            for(const auto& w:s.walls){
                if(w.z0>zs[z-1] || w.z1<zs[z])continue;
                int k=-1;
                if(w.ax==w.bx && std::min(w.ay,w.by)==ys[d] && std::max(w.ay,w.by)==ys[e]){
                    if(w.ax==xs[a])k=0;else if(w.ax==xs[b])k=1;
                }else if(w.ay==w.by && std::min(w.ax,w.bx)==xs[a] && std::max(w.ax,w.bx)==xs[b]){
                    if(w.ay==ys[d])k=2;else if(w.ay==ys[e])k=3;
                }
                if(k<0)continue;
                const auto& m=s.mats[(size_t)w.mat];
                if(m.tau[3]<.48)side[k]=true; // slack from the required .7 amplitude
                bool zero=true;for(int h=0;h<NB;++h)zero=zero && m.tau[h]==0;
                opaque[k]=opaque[k]||zero;
            }
            if(side[0]&&side[1]&&side[2]&&side[3]){
                bool op=opaque[0]&&opaque[1]&&opaque[2]&&opaque[3];
                if(!shells.empty() && shells.back().x0==xs[a] && shells.back().x1==xs[b] && shells.back().y0==ys[d] && shells.back().y1==ys[e] && shells.back().z1==zs[z-1] && shells.back().opaque==op) shells.back().z1=zs[z];
                else {
                    if(shells.size()>=128)return; // optional acceleration has a fixed query budget
                    shells.push_back({xs[a],xs[b],ys[d],ys[e],zs[z-1],zs[z],op});
                }
            }
        }
    }
    bool separatedShell(Vec3 a,Vec3 b,bool needClear) const {
        for(const auto& q:shells){
            if(!needClear && !q.opaque)continue;
            // All candidate heights are convex combinations of the endpoint heights.
            if(std::min(a.z,b.z)<=q.z0+1e-8 || std::max(a.z,b.z)>=q.z1-1e-8)continue;
            auto region=[&](Vec3 p){
                // Stay away from the numerical corner/along-wall tolerances of bendsOk.
                constexpr double m=.002;
                if(p.x>q.x0+m && p.x<q.x1-m && p.y>q.y0+m && p.y<q.y1-m)return 1;
                if(p.x<q.x0-m || p.x>q.x1+m || p.y<q.y0-m || p.y>q.y1+m)return -1;
                return 0;
            };
            int u=region(a),v=region(b);if(u*v<0)return true;
        }
        return false;
    }
    explicit Geo(const Scene& sc) : s(sc), W((int)sc.walls.size()), hasCeil(sc.ceilMat >= 0), H(sc.height) {
        for (const auto& m : sc.mats)  // E3b: every transmission factor sqrt(tau) is in [0, 1] => path amplitudes never grow
            for (int b = 0; b < NB; ++b) ampNonIncreasing = ampNonIncreasing && m.tau[b] >= 0.0 && m.tau[b] <= 1.0;
        buildJunctions();
        buildShells();
        for (const auto& z : sc.ceilZones) {
            hasCeil |= z.mat >= 0 && z.z < 0;  // a ceiling over part of an open sky
            if (z.z >= 0 && z.mat >= 0 && z.poly.size() >= 3) plates.push_back(&z);
        }
        for (const auto& z : sc.floorZones)
            if (z.z >= 0 && z.mat >= 0 && z.poly.size() >= 3) plates.push_back(&z);
        auto screen=[](const Material& m){for(int b=0;b<NB;++b)if(m.tau[b]>.2)return false;return true;};
        for(int i=0;i<W;++i)if(screen(mat(i)) && (jStart[2*i]==jStart[2*i+1] || jStart[2*i+1]==jStart[2*i+2]))freeScreenWalls.push_back(i);
        for(int id=W+2;id<nSurf();++id)if(screen(mat(id))) {
            const auto& p=plate(id);
            for(size_t j=0;j<p.poly.size();++j){auto a=p.poly[j],b=p.poly[(j+1)%p.poly.size()];Vec3 A{a.first,a.second,p.z},B{b.first,b.second,p.z};screenRims.push_back({id,A,B,unit(B-A),norm(B-A)});}
        }
        // Boundary adjacency is geometric and immutable; never search topology in process().
        auto onSegment=[](Vec3 p,Vec3 a,Vec3 b){Vec3 d=b-a;double t=dot(p-a,d)/std::max(1e-20,dot(d,d));return t>=-1e-6&&t<=1+1e-6&&norm(p-(a+d*t))<1e-5;};
        auto inside=[&](const Zone& p,Vec3 a){if(pointInZone(p,a.x,a.y))return true;for(size_t j=0;j<p.poly.size();++j){auto u=p.poly[j],v=p.poly[(j+1)%p.poly.size()];if(onSegment(a,{u.first,u.second,p.z},{v.first,v.second,p.z}))return true;}return false;};
        wallBottomJoined.assign(W,0);wallTopJoined.assign(W,0);
        for(int i=0;i<W;++i){const auto& w=s.walls[i];double lo=std::max(0.,w.z0),hi=std::min(H,w.z1);
            wallBottomJoined[i]=lo==0 && s.floorMat>=0;
            wallTopJoined[i]=hi==H && hasCeil;
            for(const auto* p:plates){
                if(std::abs(p->z-lo)<1e-6&&inside(*p,{w.ax,w.ay,lo})&&inside(*p,{w.bx,w.by,lo}))wallBottomJoined[i]=1;
                if(std::abs(p->z-hi)<1e-6&&inside(*p,{w.ax,w.ay,hi})&&inside(*p,{w.bx,w.by,hi}))wallTopJoined[i]=1;
            }
        }
        for(int i=0;i<W;++i)freeReflectorHeights=freeReflectorHeights||!wallBottomJoined[i]||!wallTopJoined[i];
        plateJoined.resize(plates.size());
        for(size_t pi=0;pi<plates.size();++pi){const auto& p=*plates[pi];auto& joined=plateJoined[pi];joined.assign(p.poly.size(),0);
            for(size_t j=0;j<p.poly.size();++j){auto u=p.poly[j],v=p.poly[(j+1)%p.poly.size()];Vec3 a{u.first,u.second,p.z},b{v.first,v.second,p.z};
                for(const auto& w:s.walls)if(p.z>=w.z0-1e-6&&p.z<=std::min(w.z1,H)+1e-6&&onSegment(a,{w.ax,w.ay,p.z},{w.bx,w.by,p.z})&&onSegment(b,{w.ax,w.ay,p.z},{w.bx,w.by,p.z}))joined[j]=1;
                for(size_t qi=0;qi<plates.size();++qi)if(qi!=pi&&std::abs(plates[qi]->z-p.z)<1e-6){const auto& q=*plates[qi];for(size_t k=0;k<q.poly.size();++k){auto x=q.poly[k],y=q.poly[(k+1)%q.poly.size()];if(onSegment(a,{x.first,x.second,q.z},{y.first,y.second,q.z})&&onSegment(b,{x.first,x.second,q.z},{y.first,y.second,q.z}))joined[j]=1;}}
            }
        }
        litRimsBySurface.resize((size_t)nSurf());
        for(int id=0;id<nSurf();++id)for(int j=0;j<(int)screenRims.size();++j)if(screenRims[j].surf!=id)litRimsBySurface[id].push_back(j);
        buildWelds();
    }
    // ---- E6: welded edge topology (built with the scene, never in process()) ----
    // Every edge of a face (wall end, wall bottom / top, plate polygon side, floor / ceiling zone side) is split into
    // sub-segments along which the same faces meet. For each sub-segment the faces are stored as rays in the plane
    // perpendicular to the edge (angle in the basis e1, e2; u = e1 x e2 is the edge axis): a face that ends on the edge
    // gives one ray, a face that passes through it (the floor under a wall, a wall through a plate) gives two. The rays
    // cut the perpendicular plane into sectors. kind: 0 free (one ray: the edge of a lone face), 1 coplanar weld (two
    // opposite rays of different faces or materials), 2 wedge (two rays at an angle: a corner), 3 T (three rays), 4 more.
    // domain: the edge lies in the floor plane (z = 0) or under the ceiling plane (z = H): a straight leg between two
    // points of the room can never pass it on the other side (the half plane beyond is not part of the model).
    struct Weld {
        Vec3 A{}, B{}, u{}, e1{}, e2{};
        double len = 0;
        bool vertical = false, domain = false;
        int n = 0, kind = 0;
        static constexpr int kMax = 8;
        double ang[kMax] = {};
        int face[kMax] = {}, mat[kMax] = {};  // surface id; material index (-1: none)
        bool contA = false, contB = false;    // another sub-segment of the same line continues past A / B
    };
    std::vector<Weld> welds;
    struct WeldCounts { int edges = 0, freeEdges = 0, shared = 0, partlyShared = 0, coplanar = 0, wedge = 0, tee = 0, multi = 0,
                        domain = 0, corners = 0, unsupported = 0, originalEdges = 0; } weldCounts;
    // Per face, the sub-segments that lie on its boundary (indices into welds), for the support hand-over.
    std::vector<std::vector<int>> weldsOfSurface;
    std::vector<int> legWelds;   // sub-segments that can change a straight leg's transmission when it passes them (E6)
    void addWeld(Weld w) {
        // sort the rays by angle in [0, 2 pi)
        for (int i = 0; i < w.n; ++i) { while (w.ang[i] < 0) w.ang[i] += 2 * kPi; while (w.ang[i] >= 2 * kPi) w.ang[i] -= 2 * kPi; }
        for (int i = 1; i < w.n; ++i) for (int j = i; j > 0 && w.ang[j] < w.ang[j - 1]; --j) {
            std::swap(w.ang[j], w.ang[j - 1]); std::swap(w.face[j], w.face[j - 1]); std::swap(w.mat[j], w.mat[j - 1]); }
        if (w.n == 1) w.kind = 0;
        else if (w.n == 2) w.kind = std::fabs(std::fabs(w.ang[1] - w.ang[0]) - kPi) < 1e-6 ? 1 : 2;
        else w.kind = w.n == 3 ? 3 : 4;
        welds.push_back(w);
    }
    void buildWelds() {
        welds.clear();
        weldCounts = WeldCounts{};
        constexpr double tz = 1e-6, eps = 1e-5;
        auto lo = [&](const Wall& w) { return std::max(0.0, w.z0); };
        auto hi = [&](const Wall& w) { return std::min(w.z1, H); };
        // ---- vertical edges: at every wall end ----
        std::vector<Vec3> doneV;
        for (int i = 0; i < W; ++i) for (int e = 0; e < 2; ++e) {
            const Wall& wi = s.walls[(size_t)i];
            const double px = e ? wi.bx : wi.ax, py = e ? wi.by : wi.ay;
            ++weldCounts.originalEdges;
            if (hi(wi) - lo(wi) <= tz) continue;
            struct Part { double ang, z0, z1; int wall; };
            Part parts[2 * Weld::kMax + 8]; int np = 0;
            constexpr int kParts = 2 * Weld::kMax + 8;
            for (int j = 0; j < W && np < kParts - 1; ++j) {
                const Wall& wj = s.walls[(size_t)j];
                const double len = std::hypot(wj.bx - wj.ax, wj.by - wj.ay);
                if (len <= kJoinTol || hi(wj) - lo(wj) <= tz) continue;
                const bool atA = std::hypot(wj.ax - px, wj.ay - py) <= kJoinTol, atB = std::hypot(wj.bx - px, wj.by - py) <= kJoinTol;
                if (atA) parts[np++] = {std::atan2(wj.by - wj.ay, wj.bx - wj.ax), lo(wj), hi(wj), j};
                if (atB) parts[np++] = {std::atan2(wj.ay - wj.by, wj.ax - wj.bx), lo(wj), hi(wj), j};
                if (atA || atB) continue;
                const double ux = (wj.bx - wj.ax) / len, uy = (wj.by - wj.ay) / len;
                const double along = (px - wj.ax) * ux + (py - wj.ay) * uy, off = (px - wj.ax) * uy - (py - wj.ay) * ux;
                if (std::fabs(off) <= kJoinTol && along > kJoinTol && along < len - kJoinTol) {
                    parts[np++] = {std::atan2(uy, ux), lo(wj), hi(wj), j};
                    parts[np++] = {std::atan2(-uy, -ux), lo(wj), hi(wj), j};
                }
            }
            double zs[2 * kParts]; int nz = 0;
            for (int k = 0; k < np; ++k) { zs[nz++] = parts[k].z0; zs[nz++] = parts[k].z1; }
            std::sort(zs, zs + nz);
            // the original edge (wall i, end e) is partly shared when its sub-segments differ
            uint64_t firstSig = 0; bool sigSet = false, split = false;
            const bool first = [&] { for (const auto& q : doneV) if (std::hypot(q.x - px, q.y - py) <= kJoinTol) return false; return true; }();
            for (int k = 0; k + 1 < nz; ++k) {
                const double z0 = zs[k], z1 = zs[k + 1];
                if (z1 - z0 <= tz) continue;
                const double zm = .5 * (z0 + z1);
                Weld w; w.vertical = true; w.A = {px, py, z0}; w.B = {px, py, z1}; w.u = {0, 0, 1}; w.e1 = {1, 0, 0}; w.e2 = {0, 1, 0};
                w.len = z1 - z0;
                uint64_t sig = 1469598103934665603ull;
                for (int q = 0; q < np && w.n < Weld::kMax; ++q) if (parts[q].z0 < zm && parts[q].z1 > zm) {
                    w.ang[w.n] = parts[q].ang; w.face[w.n] = parts[q].wall; w.mat[w.n] = s.walls[(size_t)parts[q].wall].mat; ++w.n;
                    sig = (sig ^ (uint64_t)(parts[q].wall + 1)) * 1099511628211ull;
                }
                if (zm > lo(wi) && zm < hi(wi)) { if (sigSet && sig != firstSig) split = true; if (!sigSet) { firstSig = sig; sigSet = true; } }
                if (first && w.n >= 1) addWeld(w);
            }
            if (split) ++weldCounts.partlyShared;
            if (first) doneV.push_back({px, py, 0});
        }
        // ---- horizontal edges: wall bottoms / tops, plate sides, floor / ceiling zone sides ----
        struct Seg { Vec3 a, b; };
        std::vector<Seg> segs;
        for (const auto& w : s.walls) {
            if (hi(w) - lo(w) <= tz || std::hypot(w.bx - w.ax, w.by - w.ay) <= kJoinTol) continue;
            segs.push_back({{w.ax, w.ay, lo(w)}, {w.bx, w.by, lo(w)}});
            segs.push_back({{w.ax, w.ay, hi(w)}, {w.bx, w.by, hi(w)}});
        }
        for (const auto* p : plates) for (size_t j = 0; j < p->poly.size(); ++j) {
            auto a = p->poly[j], b = p->poly[(j + 1) % p->poly.size()];
            segs.push_back({{a.first, a.second, p->z}, {b.first, b.second, p->z}});
        }
        for (const auto& z : s.floorZones) if (z.z < 0 && z.poly.size() >= 3) for (size_t j = 0; j < z.poly.size(); ++j) {
            auto a = z.poly[j], b = z.poly[(j + 1) % z.poly.size()]; segs.push_back({{a.first, a.second, 0}, {b.first, b.second, 0}}); }
        if (hasCeil || !s.ceilZones.empty())
            for (const auto& z : s.ceilZones) if (z.z < 0 && z.poly.size() >= 3) for (size_t j = 0; j < z.poly.size(); ++j) {
                auto a = z.poly[j], b = z.poly[(j + 1) % z.poly.size()]; segs.push_back({{a.first, a.second, H}, {b.first, b.second, H}}); }
        weldCounts.originalEdges += (int)segs.size();
        std::vector<unsigned char> used(segs.size(), 0);
        for (size_t si = 0; si < segs.size(); ++si) {
            if (used[si]) continue;
            Vec3 d = segs[si].b - segs[si].a; d.z = 0;
            const double dl = std::hypot(d.x, d.y);
            if (dl <= 1e-9) { used[si] = 1; continue; }
            d = d * (1.0 / dl);
            if (d.x < -1e-12 || (std::fabs(d.x) <= 1e-12 && d.y < 0)) d = d * -1.0;
            const double z = segs[si].a.z, off = d.x * segs[si].a.y - d.y * segs[si].a.x;
            auto onLine = [&](Vec3 p) { return std::fabs(p.z - z) < tz && std::fabs(d.x * p.y - d.y * p.x - off) < 1e-6; };
            std::vector<size_t> members;
            for (size_t sj = si; sj < segs.size(); ++sj) if (!used[sj] && onLine(segs[sj].a) && onLine(segs[sj].b)) { used[sj] = 1; members.push_back(sj); }
            const Vec3 base{segs[si].a.x - d.x * (d.x * segs[si].a.x + d.y * segs[si].a.y), segs[si].a.y - d.y * (d.x * segs[si].a.x + d.y * segs[si].a.y), z};
            auto param = [&](Vec3 p) { return d.x * p.x + d.y * p.y; };
            std::vector<double> br;
            for (size_t m : members) { br.push_back(param(segs[m].a)); br.push_back(param(segs[m].b)); }
            // where the line enters / leaves other faces in the same plane (plates, zones), and collinear walls
            auto cutPoly = [&](const Zone& zn) {
                for (size_t j = 0; j < zn.poly.size(); ++j) {
                    auto a = zn.poly[j], b = zn.poly[(j + 1) % zn.poly.size()];
                    auto cr = [](double ax, double ay, double bx, double by) { return ax * by - ay * bx; };
                    const double ex = b.first - a.first, ey = b.second - a.second, den = cr(d.x, d.y, ex, ey);
                    if (std::fabs(den) < 1e-12) continue;
                    const double t = cr(a.first - base.x, a.second - base.y, ex, ey) / den;
                    const double v = cr(a.first - base.x, a.second - base.y, d.x, d.y) / den;
                    if (v >= -1e-9 && v <= 1 + 1e-9) br.push_back(t);
                }
            };
            for (const auto* p : plates) if (std::fabs(p->z - z) < tz) cutPoly(*p);
            if (z < tz) for (const auto& zn : s.floorZones) if (zn.z < 0 && zn.poly.size() >= 3) cutPoly(zn);
            if (std::fabs(z - H) < tz) for (const auto& zn : s.ceilZones) if (zn.z < 0 && zn.poly.size() >= 3) cutPoly(zn);
            std::vector<int> colWalls;
            for (int i = 0; i < W; ++i) {
                const Wall& w = s.walls[(size_t)i];
                if (!(lo(w) <= z + tz && hi(w) >= z - tz) || hi(w) - lo(w) <= tz) continue;
                if (!onLine({w.ax, w.ay, z}) || !onLine({w.bx, w.by, z})) continue;
                colWalls.push_back(i); br.push_back(param({w.ax, w.ay, 0})); br.push_back(param({w.bx, w.by, 0}));
            }
            std::sort(br.begin(), br.end());
            const Vec3 nrm{-d.y, d.x, 0};
            const bool domain = z < tz || (hasCeil && std::fabs(z - H) < tz);
            std::vector<uint64_t> sigOf(br.size(), 0); std::vector<unsigned char> isEdge(br.size(), 0);
            for (size_t k = 0; k + 1 < br.size(); ++k) {
                const double t0 = br[k], t1 = br[k + 1];
                if (t1 - t0 <= 1e-6) continue;
                const double tm = .5 * (t0 + t1);
                const Vec3 M = base + d * tm;
                Weld w; w.vertical = false; w.domain = domain; w.A = base + d * t0; w.B = base + d * t1; w.u = d; w.e1 = nrm; w.e2 = {0, 0, 1};
                w.len = t1 - t0;
                auto ray = [&](double ang, int face, int mat) { if (w.n < Weld::kMax) { w.ang[w.n] = ang; w.face[w.n] = face; w.mat[w.n] = mat; ++w.n; } };
                for (int i : colWalls) {
                    const Wall& wl = s.walls[(size_t)i];
                    const double ta = param({wl.ax, wl.ay, 0}), tb = param({wl.bx, wl.by, 0});
                    if (tm <= std::min(ta, tb) || tm >= std::max(ta, tb)) continue;
                    if (lo(wl) < z - tz) ray(-kPi / 2, i, wl.mat);
                    if (hi(wl) > z + tz) ray(kPi / 2, i, wl.mat);
                }
                for (int k2 = 0; k2 < (int)plates.size(); ++k2) {
                    const Zone& p = *plates[(size_t)k2];
                    if (std::fabs(p.z - z) >= tz) continue;
                    const Vec3 a = M + nrm * eps, b = M - nrm * eps;
                    if (pointInZone(p, a.x, a.y)) ray(0, W + 2 + k2, p.mat);
                    if (pointInZone(p, b.x, b.y)) ray(kPi, W + 2 + k2, p.mat);
                }
                if (z < tz) { const Vec3 a = M + nrm * eps, b = M - nrm * eps; ray(0, W, floorMatAt(s, a.x, a.y)); ray(kPi, W, floorMatAt(s, b.x, b.y)); }
                else if (std::fabs(z - H) < tz) {
                    const Vec3 a = M + nrm * eps, b = M - nrm * eps;
                    const int ma = ceilMatAt(s, a.x, a.y), mb = ceilMatAt(s, b.x, b.y);
                    if (ma >= 0) ray(0, W + 1, ma);
                    if (mb >= 0) ray(kPi, W + 1, mb);
                }
                uint64_t sig = 1469598103934665603ull;
                for (int q = 0; q < w.n; ++q) sig = (sig ^ (uint64_t)((w.face[q] + 7) * 131 + w.mat[q] + 3 + (int)(w.ang[q] * 4))) * 1099511628211ull;
                sigOf[k] = sig;
                // a single face running through the line (plate interior, uniform floor, a wall passing the height) is no edge
                const bool through = w.n == 2 && w.face[0] == w.face[1] && w.mat[0] == w.mat[1];
                if (w.n == 0 || through) continue;
                isEdge[k] = 1;
                addWeld(w);
            }
            for (size_t m : members) {
                const double ta = std::min(param(segs[m].a), param(segs[m].b)), tb = std::max(param(segs[m].a), param(segs[m].b));
                uint64_t s0 = 0; bool set = false, split = false;
                for (size_t k = 0; k + 1 < br.size(); ++k) {
                    const double tm = .5 * (br[k] + br[k + 1]);
                    if (br[k + 1] - br[k] <= 1e-6 || tm <= ta || tm >= tb) continue;
                    const uint64_t sg = isEdge[k] ? sigOf[k] : 0;
                    if (set && sg != s0) split = true;
                    if (!set) { s0 = sg; set = true; }
                }
                if (split) ++weldCounts.partlyShared;
            }
        }
        // ---- counts, per-surface lists, leg-relevant set ----
        weldsOfSurface.assign((size_t)nSurf(), {});
        legWelds.clear();
        std::vector<Vec3> ends;
        for (int k = 0; k < (int)welds.size(); ++k) {
            const Weld& w = welds[(size_t)k];
            ++weldCounts.edges;
            if (w.n == 1) ++weldCounts.freeEdges; else ++weldCounts.shared;
            if (w.kind == 1) ++weldCounts.coplanar; else if (w.kind == 2) ++weldCounts.wedge; else if (w.kind == 3) ++weldCounts.tee; else if (w.kind == 4) ++weldCounts.multi;
            if (w.domain && w.n >= 2) ++weldCounts.domain;
            if (w.n >= Weld::kMax) ++weldCounts.unsupported;
            for (int q = 0; q < w.n; ++q) {
                auto& l = weldsOfSurface[(size_t)w.face[q]];
                if (l.empty() || l.back() != k) l.push_back(k);
            }
            if (w.n >= 2 && !w.domain) {
                // a leg passing this edge on the other side crosses other faces, or the edge is convex (a reflex sector)
                double widest = 0;
                for (int q = 0; q < w.n; ++q) widest = std::max(widest, (q + 1 < w.n ? w.ang[q + 1] : w.ang[0] + 2 * kPi) - w.ang[q]);
                bool differ = false;
                for (int q = 1; q < w.n; ++q) if (w.mat[q] != w.mat[0]) differ = true;
                if (widest > kPi + 1e-9 || differ || w.n >= 3) legWelds.push_back(k);
            }
            if (w.n >= 2) { ends.push_back(w.A); ends.push_back(w.B); }
        }
        // three-face corners: a point where a vertical and a horizontal shared sub-segment meet
        std::vector<Vec3> corners;
        for (int k = 0; k < (int)welds.size(); ++k) {
            const Weld& w = welds[(size_t)k];
            if (!w.vertical || w.n < 2) continue;
            for (const Vec3 p : {w.A, w.B}) {
                bool meet = false;
                for (const Weld& h : welds) if (!h.vertical && h.n >= 2 && (norm(h.A - p) < 1e-6 || norm(h.B - p) < 1e-6)) { meet = true; break; }
                if (!meet) continue;
                bool seen = false;
                for (const Vec3 q : corners) if (norm(q - p) < 1e-6) seen = true;
                if (!seen) corners.push_back(p);
            }
        }
        weldCounts.corners = (int)corners.size();
        // continuation flags: the leg hand-over uses a sub-segment only while the stationary point is on it, or beyond an
        // end where nothing continues the line (then the clamped end point, whose detour grows, fades the hand-over out)
        for (auto& w : welds) for (const auto& v : welds) {
            if (&v == &w || std::fabs(std::fabs(dot(v.u, w.u)) - 1) > 1e-9) continue;
            if (norm(v.A - w.A) < 1e-6 || norm(v.B - w.A) < 1e-6) w.contA = true;
            if (norm(v.A - w.B) < 1e-6 || norm(v.B - w.B) < 1e-6) w.contB = true;
        }
    }
    void buildJunctions() {
        jStart.assign((size_t)(2 * W + 1), 0);
        jRays.clear();
        std::vector<JRay> tmp;
        for (int i = 0; i < W; ++i)
            for (int e = 0; e < 2; ++e) {
                jStart[(size_t)(2 * i + e)] = (int)jRays.size();
                const Wall& wi = s.walls[(size_t)i];
                const double px = e ? wi.bx : wi.ax, py = e ? wi.by : wi.ay;
                tmp.clear();
                const double ox = e ? wi.ax : wi.bx, oy = e ? wi.ay : wi.by;
                if (std::hypot(ox - px, oy - py) > kJoinTol) tmp.push_back({std::atan2(oy - py, ox - px), i});
                for (int j = 0; j < W; ++j) {
                    if (j == i) continue;
                    const Wall& wj = s.walls[(size_t)j];
                    const double len = std::hypot(wj.bx - wj.ax, wj.by - wj.ay);
                    if (len <= kJoinTol) continue;
                    const bool atA = std::hypot(wj.ax - px, wj.ay - py) <= kJoinTol, atB = std::hypot(wj.bx - px, wj.by - py) <= kJoinTol;
                    if (atA) tmp.push_back({std::atan2(wj.by - wj.ay, wj.bx - wj.ax), j});
                    if (atB) tmp.push_back({std::atan2(wj.ay - wj.by, wj.ax - wj.bx), j});
                    if (atA || atB) continue;
                    const double ux = (wj.bx - wj.ax) / len, uy = (wj.by - wj.ay) / len;
                    const double along = (px - wj.ax) * ux + (py - wj.ay) * uy, off = (px - wj.ax) * uy - (py - wj.ay) * ux;
                    if (std::fabs(off) <= kJoinTol && along > kJoinTol && along < len - kJoinTol) {  // T junction
                        tmp.push_back({std::atan2(uy, ux), j});
                        tmp.push_back({std::atan2(-uy, -ux), j});
                    }
                }
                if (tmp.size() >= 2) jRays.insert(jRays.end(), tmp.begin(), tmp.end());  // a lone own ray = a free end: not stored
            }
        jStart[(size_t)(2 * W)] = (int)jRays.size();
    }
    int floorId() const { return W; }
    int ceilId() const { return W + 1; }
    int nSurf() const { return W + 2 + (int)plates.size(); }
    bool isPlate(int surf) const { return surf >= W + 2; }
    const Zone& plate(int surf) const { return *plates[(size_t)(surf - W - 2)]; }
    const Material& mat(int surf) const {
        if (surf >= W + 2) return s.mats[(size_t)plate(surf).mat];
        return s.mats[surf < W ? s.walls[surf].mat : (surf == W ? s.floorMat : s.ceilMat)];
    }
    // Material of surface `surf` at the hit point (x, y): floor and ceiling zones are looked up there.
    // nullptr = nothing there (an opening in the ceiling): the sound passes.
    const Material* matAt(int surf, double x, double y) const {
        if (surf < W) return &s.mats[(size_t)s.walls[(size_t)surf].mat];
        if (surf >= W + 2) return &s.mats[(size_t)plate(surf).mat];
        const int m = surf == W ? floorMatAt(s, x, y) : ceilMatAt(s, x, y);
        return m >= 0 ? &s.mats[(size_t)m] : nullptr;
    }
};

inline double cross2(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }

inline Vec3 mirror(const Geo& g, Vec3 p, int surf) {
    if (surf < g.W) {
        const Wall& w = g.s.walls[surf];
        double dx = w.bx - w.ax, dy = w.by - w.ay;
        double t = ((p.x - w.ax) * dx + (p.y - w.ay) * dy) / (dx * dx + dy * dy);
        double fx = w.ax + t * dx, fy = w.ay + t * dy;
        return {2 * fx - p.x, 2 * fy - p.y, p.z};
    }
    if (surf == g.W) return {p.x, p.y, -p.z};
    if (surf >= g.W + 2) return {p.x, p.y, 2 * g.plate(surf).z - p.z};
    return {p.x, p.y, 2 * g.H - p.z};
}

// Where segment cur->target crosses surface `surf` (within its extent).
inline bool crossSurface(const Geo& g, Vec3 cur, Vec3 target, int surf, Vec3& P) {
    double t;
    if (surf < g.W) {
        const Wall& w = g.s.walls[surf];
        double rx = target.x - cur.x, ry = target.y - cur.y, ex = w.bx - w.ax, ey = w.by - w.ay;
        double den = cross2(rx, ry, ex, ey);
        if (std::fabs(den) < 1e-12) return false;
        t = cross2(w.ax - cur.x, w.ay - cur.y, ex, ey) / den;
        double u = cross2(w.ax - cur.x, w.ay - cur.y, rx, ry) / den;
        if (t <= 1e-9 || t >= 1 - 1e-9 || u < 0 || u > 1) return false;
        double z = cur.z + t * (target.z - cur.z);
        if (z < std::max(0.0, w.z0) || z > std::min(w.z1, g.H)) return false;  // a reflection point must lie on the wall's face
    } else if (surf >= g.W + 2) {
        const Zone& pl = g.plate(surf);
        double dz = target.z - cur.z;
        if (std::fabs(dz) < 1e-12) return false;
        t = (pl.z - cur.z) / dz;
        if (t <= 1e-9 || t >= 1 - 1e-9) return false;
        if (!pointInZone(pl, cur.x + t * (target.x - cur.x), cur.y + t * (target.y - cur.y))) return false;
    } else {
        double dz = target.z - cur.z;
        if (std::fabs(dz) < 1e-12) return false;
        t = ((surf == g.W ? 0.0 : g.H) - cur.z) / dz;
        if (t <= 1e-9 || t >= 1 - 1e-9) return false;
    }
    P = cur + (target - cur) * t;
    return true;
}

// Walks the segment a-b past every wall it crosses (except ign1/ign2). Each crossing multiplies `amp[band]`
// (if given) by the wall's amplitude transmission sqrt(tau_b). Returns false when a crossed wall is opaque
// (tau == 0 in all bands): the path does not exist. Open segments (tau = 1) pass the sound unchanged.
inline bool transmitLeg(const Geo& g, Vec3 a, Vec3 b, int ign1, int ign2, float* amp) {
    double rx = b.x - a.x, ry = b.y - a.y;
    for (int i = 0; i < g.W; ++i) {
        if (i == ign1 || i == ign2) continue;
        const Wall& w = g.s.walls[i];
        double ex = w.bx - w.ax, ey = w.by - w.ay;
        double den = cross2(rx, ry, ex, ey);
        if (std::fabs(den) < 1e-12) continue;
        double t = cross2(w.ax - a.x, w.ay - a.y, ex, ey) / den;
        double u = cross2(w.ax - a.x, w.ay - a.y, rx, ry) / den;
        if (t > 1e-6 && t < 1 - 1e-6 && u >= 0 && u <= 1) {
            const double zc = a.z + t * (b.z - a.z);  // height of the path where it crosses the wall's line
            if (zc < w.z0 || zc > w.z1) continue;      // it passes over (or under) this wall
            const Material& m = g.s.mats[(size_t)w.mat];
            double mx = 0;
            for (int k = 0; k < NB; ++k) mx = std::max(mx, m.tau[k]);
            if (mx <= 0) return false;
            if (amp)
                for (int k = 0; k < NB; ++k) amp[k] *= (float)std::sqrt(m.tau[k]);
        }
    }
    const double dz = b.z - a.z;
    if (!g.plates.empty() && std::fabs(dz) > 1e-12)
        for (int k = 0; k < (int)g.plates.size(); ++k) {
            const int id = g.W + 2 + k;
            if (id == ign1 || id == ign2) continue;
            const Zone& pl = *g.plates[(size_t)k];
            const double t = (pl.z - a.z) / dz;
            if (t <= 1e-6 || t >= 1 - 1e-6) continue;
            if (!pointInZone(pl, a.x + t * (b.x - a.x), a.y + t * (b.y - a.y))) continue;
            const Material& m = g.s.mats[(size_t)pl.mat];
            double mx = 0;
            for (int q = 0; q < NB; ++q) mx = std::max(mx, m.tau[q]);
            if (mx <= 0) return false;
            if (amp)
                for (int q = 0; q < NB; ++q) amp[q] *= (float)std::sqrt(m.tau[q]);
        }
    return true;
}

inline bool blocked(const Geo& g, Vec3 a, Vec3 b, int ign1, int ign2) { return !transmitLeg(g, a, b, ign1, ign2, nullptr); }

// A reflection point P that lies exactly on another wall's plane (the junction of a wall with the floor or ceiling, or a
// corner of two walls) is touched, not crossed, by each of the two legs that meet there, so transmitLeg (which does not
// count a crossing at t = 0 or t = 1) lets a path slip through the wall at such a point (E3: a source at 1.5 m in a 3 m
// room, symmetric with the receiver, reflects at exactly z = 0 on the wall's foot). The path really crosses the wall if the
// point before P and the point after P are on opposite sides of the wall's plane, P is within the wall's extent (ends
// included) and at a height the wall covers: the wall's transmission then applies, opaque kills the path (returns false).
// A leg lying in the plane counts as on neither side. O(W) per reflection point, no allocation.
inline bool touchCross(const Geo& g, Vec3 prev, Vec3 P, Vec3 next, float* amp) {
    constexpr double tol = 1e-6;
    for (int i = 0; i < g.W; ++i) {
        const Wall& w = g.s.walls[(size_t)i];
        const double ex = w.bx - w.ax, ey = w.by - w.ay, len = std::hypot(ex, ey);
        if (len < 1e-9) continue;
        const double off = cross2(ex, ey, P.x - w.ax, P.y - w.ay) / len;
        if (std::fabs(off) > tol) continue;                                    // P is not on this wall's plane
        const double along = ((P.x - w.ax) * ex + (P.y - w.ay) * ey) / len;
        if (along < -tol || along > len + tol) continue;                       // beyond the wall's ends
        if (P.z < w.z0 - tol || P.z > std::min(w.z1, g.H) + tol) continue;   // under / over the wall
        const double sp = cross2(ex, ey, prev.x - P.x, prev.y - P.y) / len, sn = cross2(ex, ey, next.x - P.x, next.y - P.y) / len;
        if (!(sp * sn < 0) || std::fabs(sp) < 1e-9 || std::fabs(sn) < 1e-9) continue;  // same side (or in the plane): touched only
        const Material& m = g.s.mats[(size_t)w.mat];
        double mx = 0;
        for (int b = 0; b < NB; ++b) mx = std::max(mx, m.tau[b]);
        if (mx <= 0) return false;
        if (amp)
            for (int b = 0; b < NB; ++b) amp[b] *= (float)std::sqrt(m.tau[b]);
    }
    if(amp && g.s.diffraction) for(int b=0;b<NB;++b) amp[b]=std::min(amp[b],1.f);
    return true;
}

// ---- closed wall ends (E3) ----
// A bend prev -> p -> next round the end `end` of wall `wall` (p is that end, at height p.z). Where other walls meet the
// same point (a closed corner, a T junction) and cover the path height, they cut the plane round p into angular
// sectors. Sound can bend round the corner only if both legs lie in the same sector (outside round the building, or
// inside the L-room's reflex corner). If the legs are in different sectors the bend would cross the walls that separate
// them, exactly as a straight leg passing a hair beside the corner does: those walls are the rays inside the smaller
// angular arc between the two legs (< 180 deg). An opaque wall there (tau = 0 in every band) kills the path (returns
// false); a transmissive one multiplies `amp[band]` (if given) by sqrt(tau_b), the same factor transmitLeg applies.
// With exactly 180 deg between the legs either side is possible: the easier one (per band) is taken. A free end (no
// other wall meets it, or none of them covers the height) and a vertical leg (no horizontal direction) always pass.
//
// A leg that runs exactly along a wall (a leg from corner to corner along a building's face) sits on a ray: it is on
// one face of that wall, and must stay on the same face at both of its ends. bendsOk below tries both faces of every
// such leg and keeps a consistent choice (this is what stops the sound creeping along a wall from the outside face
// round into the room).
struct Bend {
    int wall, end;        // the wall end the sound bends round
    Vec3 p, prev, next;   // the bend point and the points before and after it on the path
};

constexpr double kNoAngle = std::numeric_limits<double>::quiet_NaN();

// x mod 2 pi for |x| < 4 pi, exactly what std::fmod(x, 2 pi) returns there (fmod is exact, and x -+ 2 pi is exact by
// Sterbenz), without the library call (E3b).
inline double wrap2pi(double x) {
    constexpr double twoPi = 2.0 * kPi;
    if (x >= twoPi) return x - twoPi;
    if (x <= -twoPi) return x + twoPi;
    return x;
}

// sIn / sOut = 0: legs on a ray are compatible with both adjacent sectors (permissive; *tie reports such a leg).
// a0raw / a1raw: atan2 of the incoming / outgoing leg direction if the caller already has it (E3b; same value the
// function would compute), kNoAngle otherwise.
// sIn / sOut = +-1: the leg is shifted by a hair CCW / CW (as seen from p), which decides the tie.
inline bool cornerCore(const Geo& g, const Bend& bd, int sIn, int sOut, float* amp, bool* tie, double a0raw = kNoAngle, double a1raw = kNoAngle) {
    const Geo::JRay* r0 = g.jBegin(bd.wall, bd.end);
    const Geo::JRay* r1 = g.jEnd(bd.wall, bd.end);
    if (r1 - r0 < 2) return true;
    const double ix = bd.prev.x - bd.p.x, iy = bd.prev.y - bd.p.y, ox = bd.next.x - bd.p.x, oy = bd.next.y - bd.p.y;
    if (ix * ix + iy * iy < 1e-18 || ox * ox + oy * oy < 1e-18) return true;
    constexpr int kMaxRays = 64;
    double ang[kMaxRays];
    const Wall* rw[kMaxRays];
    int n = 0;
    for (const Geo::JRay* r = r0; r != r1 && n < kMaxRays; ++r) {
        const Wall& w = g.s.walls[(size_t)r->wall];
        if (bd.p.z < w.z0 - 1e-9 || bd.p.z > std::min(w.z1, g.H) + 1e-9) continue;  // passes over / under this wall
        ang[n] = r->ang;
        rw[n++] = &w;
    }
    if (n < 2) return true;
    constexpr double twoPi = 2.0 * kPi, hair = 2e-6;
    const double a0 = (std::isnan(a0raw) ? std::atan2(iy, ix) : a0raw) + hair * sIn;
    const double a1 = (std::isnan(a1raw) ? std::atan2(oy, ox) : a1raw) + hair * sOut;
    const double margin = (sIn == 0 && sOut == 0) ? 1e-7 : 0.0;
    double d = wrap2pi(a1 - a0);
    if (d < 0) d += twoPi;
    if (d < 1e-12 || d > twoPi - 1e-12) return true;  // both legs leave in the same direction
    double rel[kMaxRays];
    int nCcw = 0, nCw = 0;
    for (int k = 0; k < n; ++k) {
        double q = wrap2pi(ang[k] - a0);
        if (q < 0) q += twoPi;
        rel[k] = q;
        if (tie && (q < margin || q > twoPi - margin || std::fabs(q - d) < margin)) *tie = true;
        if (q > margin && q < d - margin) ++nCcw;
        else if (q > d + margin && q < twoPi - margin) ++nCw;
    }
    if (nCcw == 0 || nCw == 0) return true;  // same sector: it bends round the corner, not through it
    auto arc = [&](bool ccw, float* a) {
        for (int k = 0; k < n; ++k) {
            const bool in = ccw ? (rel[k] > margin && rel[k] < d - margin) : (rel[k] > d + margin && rel[k] < twoPi - margin);
            if (!in) continue;
            const Material& m = g.s.mats[(size_t)rw[k]->mat];
            double mx = 0;
            for (int b = 0; b < NB; ++b) mx = std::max(mx, m.tau[b]);
            if (mx <= 0) return false;
            if (a) for (int b = 0; b < NB; ++b) a[b] *= (float)std::sqrt(m.tau[b]);
        }
        return true;
    };
    if (d < kPi - 1e-6 || d > kPi + 1e-6) return arc(d < kPi, amp);
    float c1[NB], c2[NB];
    for (int b = 0; b < NB; ++b) c1[b] = c2[b] = 1.f;
    const bool ok1 = arc(true, c1), ok2 = arc(false, c2);
    if (!ok1 && !ok2) return false;
    if (amp)
        for (int b = 0; b < NB; ++b) amp[b] *= !ok1 ? c2[b] : !ok2 ? c1[b] : std::max(c1[b], c2[b]);
    return true;
}

// The bends of one path, in order (n <= 3; legs: S -> bend 0 -> ... -> R). False if the sound would have to go through
// an opaque wall at one of them; otherwise multiplies `amp` by the transmission of any transmissive wall it crosses.
// amp == nullptr: only the cheap permissive test (used to rank candidates).
// cost (E3d): if given, increased by the work done (rays at the junction + 2 per corner test), as DiffractionSearch counts it.
inline bool bendsOk(const Geo& g, const Bend* bd, int n, float* amp, const double* a0raw = nullptr, const double* a1raw = nullptr, int64_t* cost = nullptr) {
    auto A0 = [&](int k) { return a0raw ? a0raw[k] : kNoAngle; };
    auto A1 = [&](int k) { return a1raw ? a1raw[k] : kNoAngle; };
    auto charge = [&](int k) { if (cost) *cost += (int64_t)(g.jEnd(bd[k].wall, bd[k].end) - g.jBegin(bd[k].wall, bd[k].end)) + 2; };
    bool tie = false;
    float perm[NB];
    for (int b = 0; b < NB; ++b) perm[b] = 1.f;
    for (int k = 0; k < n; ++k) {
        charge(k);
        if (!cornerCore(g, bd[k], 0, 0, amp ? perm : nullptr, &tie, A0(k), A1(k))) return false;
    }
    if (!tie || !amp) {
        if (amp) for (int b = 0; b < NB; ++b) amp[b] *= perm[b];
        return true;
    }
    // some leg lies along a wall: it keeps to one face (left or right of its direction of travel); try each choice
    float best[NB] = {};
    bool found = false;
    double bestSum = -1;
    for (int mask = 0; mask < (1 << (n + 1)); ++mask) {
        auto sg = [&](int leg) { return (mask >> leg & 1) ? 1 : -1; };  // +1 = left of the travel direction
        float a[NB];
        for (int b = 0; b < NB; ++b) a[b] = 1.f;
        bool ok = true;
        for (int k = 0; k < n && ok; ++k) {  // the bend ends leg k (shift -sg) and starts leg k+1 (shift +sg)
            charge(k);
            ok = cornerCore(g, bd[k], -sg(k), sg(k + 1), a, nullptr, A0(k), A1(k));
        }
        if (!ok) continue;
        double sum = 0;
        for (int b = 0; b < NB; ++b) sum += a[b];
        if (sum > bestSum) { bestSum = sum; std::copy(a, a + NB, best); found = true; }
    }
    if (!found) return false;
    for (int b = 0; b < NB; ++b) amp[b] *= best[b];
    return true;
}

// ---- edge diffraction (vertical edges = wall ends) ----
// Knife-edge attenuation, dB, of a path whose detour is `delta` metres (ITU-R P.526 approximation of the Fresnel
// integral). `shadow` = true: the edge is in the way (the sound has to bend round it); false: the edge is just beside
// the clear path and still trims the first Fresnel zone. Continuous across the shadow boundary (6 dB there).
inline double knifeEdgeDb(double delta, double freq, double c, bool shadow) {
    E5B_COUNT(knife,1);
    double v = std::sqrt(std::max(0.0, 2.0 * delta * freq / c));
    if (!shadow) v = -v;
    if (v <= -0.78) return 0.0;
    const double x = v - 0.1;
    return 6.9 + 20.0 * std::log10(std::sqrt(x * x + 1.0) + x);
}

// Like crossSurface, but the end of a wall is soft: a reflection point a little beyond the wall's end is still
// allowed and a point right at the end is attenuated, by the knife-edge loss of the point's distance from the end
// (6 dB exactly at the end, none well inside, falling away outside). A specular reflection therefore fades in and out
// as the source or the listener moves past the end of the wall instead of switching on and off. `amp[band]` is
// multiplied by the loss. Floors, ceilings and scenes without diffraction keep the hard edge.
inline bool crossSurfaceLegacy(const Geo& g, Vec3 cur, Vec3 target, int surf, Vec3& P, double c, float* amp) {
    if (surf >= g.W || !g.s.diffraction) return crossSurface(g, cur, target, surf, P);
    const Wall& w = g.s.walls[surf];
    const double rx = target.x - cur.x, ry = target.y - cur.y, ex = w.bx - w.ax, ey = w.by - w.ay;
    const double den = cross2(rx, ry, ex, ey);
    if (std::fabs(den) < 1e-12) return false;
    const double t = cross2(w.ax - cur.x, w.ay - cur.y, ex, ey) / den;
    const double u = cross2(w.ax - cur.x, w.ay - cur.y, rx, ry) / den;
    if (t <= 1e-9 || t >= 1 - 1e-9) return false;
    const double wl = std::hypot(ex, ey);
    const double margin = 2.5;  // metres beyond the end where the reflection has died away at every frequency
    if (u * wl < -margin || (u - 1.0) * wl > margin) return false;
    const double z = cur.z + t * (target.z - cur.z);
    if (z < std::max(0.0, w.z0) || z > std::min(w.z1, g.H)) return false;
    P = cur + (target - cur) * t;
    const double dIn = std::min(u, 1.0 - u) * wl;  // >0 inside the wall, <0 beyond its end
    if (dIn >= 2.5) return true;
    const double d1 = std::max(0.05, norm(P - cur)), d2 = std::max(0.05, norm(target - P));
    const double delta = 0.5 * dIn * dIn * (1.0 / d1 + 1.0 / d2);
    for (int b = 0; b < NB; ++b) amp[b] *= (float)std::pow(10.0, -knifeEdgeDb(delta, kBandHz[b], c, dIn < 0) / 20.0);
    return true;
}

inline bool crossSurfaceSoft(const Geo& g, Vec3 cur, Vec3 target, int surf, Vec3& P, double c, float* amp) {
    if(!g.s.diffraction)return crossSurface(g,cur,target,surf,P);
    if(g.isPlate(surf)){
        const auto& pl=g.plate(surf);const double dz=target.z-cur.z;
        if(std::abs(dz)<1e-12)return false;
        double t=(pl.z-cur.z)/dz;if(t<=1e-9||t>=1-1e-9)return false;
        P=cur+(target-cur)*t;
        const bool inside=pointInZone(pl,P.x,P.y);double freeDistance=1e30,nearest=1e30;bool nearestJoined=false;
        for(size_t j=0;j<pl.poly.size();++j){auto a=pl.poly[j],b=pl.poly[(j+1)%pl.poly.size()];Vec3 A{a.first,a.second,pl.z},B{b.first,b.second,pl.z},D=B-A;
            double u=std::clamp(dot(P-A,D)/std::max(1e-20,dot(D,D)),0.,1.);double d=norm(P-(A+D*u));
            bool joined=g.plateJoined[(size_t)(surf-g.W-2)][j]!=0;
            if(d<nearest){nearest=d;nearestJoined=joined;}if(!joined)freeDistance=std::min(freeDistance,d);
        }
        if(!inside&&nearestJoined)return false;
        if(freeDistance==1e30)return inside;
        const double d1=std::max(.05,norm(P-cur)),d2=std::max(.05,norm(target-P));
        const double delta=.5*freeDistance*freeDistance*(1/d1+1/d2);
        for(int b=0;b<NB;++b){double db=knifeEdgeDb(delta,kBandHz[b],c,!inside);if(db!=0){E5B_COUNT(powers,1);amp[b]*=(float)std::pow(10.,-db/20.);}}
        return true;
    }
    if(surf>=g.W)return crossSurface(g,cur,target,surf,P);
    const Wall& w = g.s.walls[surf];
    const double rx = target.x - cur.x, ry = target.y - cur.y, ex = w.bx - w.ax, ey = w.by - w.ay;
    const double den = cross2(rx, ry, ex, ey);
    if (std::fabs(den) < 1e-12) return false;
    const double t = cross2(w.ax - cur.x, w.ay - cur.y, ex, ey) / den;
    const double u = cross2(w.ax - cur.x, w.ay - cur.y, rx, ry) / den;
    if (t <= 1e-9 || t >= 1 - 1e-9) return false;
    const double wl = std::hypot(ex, ey);
    const double margin = 2.5;  // metres beyond the end where the reflection has died away at every frequency
    if (u * wl < -margin || (u - 1.0) * wl > margin) return false;
    const double z = cur.z + t * (target.z - cur.z);
    const double lower=z-std::max(0.,w.z0),upper=std::min(w.z1,g.H)-z;
    if((g.wallBottomJoined[surf]&&lower<0)||(g.wallTopJoined[surf]&&upper<0))return false;
    P = cur + (target - cur) * t;
    const double dIn = std::min(u, 1.0 - u) * wl;  // >0 inside the wall, <0 beyond its end
    double heightDistance=1e30;
    if(!g.wallBottomJoined[surf])heightDistance=std::min(heightDistance,lower);
    if(!g.wallTopJoined[surf])heightDistance=std::min(heightDistance,upper);
    if(dIn>=2.5 && heightDistance==1e30)return true;
    const double d1 = std::max(0.05, norm(P - cur)), d2 = std::max(0.05, norm(target - P));
    const double delta = 0.5 * dIn * dIn * (1.0 / d1 + 1.0 / d2);
    for (int b = 0; b < NB; ++b) {
        double db=dIn>=2.5?0:knifeEdgeDb(delta,kBandHz[b],c,dIn<0);
        if(heightDistance<1e29)db+=knifeEdgeDb(.5*heightDistance*heightDistance*(1/d1+1/d2),kBandHz[b],c,heightDistance<0);
        if(db!=0){E5B_COUNT(powers,1);amp[b]*=(float)std::pow(10.,-db/20.);}
    }
    return true;
}

// An opaque wall can be diffracted around only if it really blocks at the path height.
inline bool edgeRelevant(const Wall& w, const Material& m, double z) {
    if (z < w.z0 || z > w.z1) return false;
    for (int k = 0; k < NB; ++k) if (m.tau[k] > 0.2) return false;  // lets 7 dB or less through: a screen, not a curtain
    return true;
}

// E5: stationary point on a finite horizontal knife edge. No allocation, O(1).
inline Vec3 plateEdgePoint(Vec3 S, Vec3 R, Vec3 A, Vec3 B) {
    const Vec3 u=unit(B-A); const double length=norm(B-A);
    const double a=dot(S-A,u), b=dot(R-A,u);
    const double h1=norm(S-A-u*a), h2=norm(R-A-u*b);
    const double t=(a*h2+b*h1)/std::max(1e-12,h1+h2);
    return A+u*std::clamp(t,0.0,length);
}
inline bool screens(const Material& m) {
    for(int b=0;b<NB;++b) if(m.tau[b]>.2) return false;
    return true;
}
// Direct path S->R is clear: walls whose end lies close beside it still take a share of the first Fresnel zone.
// Multiplies att[band] (amplitude) by the strongest such edge's attenuation.
inline void litEdgeAttenuation(const Geo& g, Vec3 S, Vec3 R, double c, float* att, int ignore = -1, bool freeOnly = false) {
    const double rx = R.x - S.x, ry = R.y - S.y, len2 = rx * rx + ry * ry, len = std::sqrt(len2);
    const double dist3 = std::sqrt(len2 + (R.z - S.z) * (R.z - S.z));
    double bestDb[NB] = {};
    for (int wi = 0; wi < (freeOnly?(int)g.freeScreenWalls.size():g.W); ++wi) {
        const int i=freeOnly?g.freeScreenWalls[(size_t)wi]:wi;
        if(i==ignore || len<1e-6) continue;
        E5B_COUNT(walls,1);
        const Wall& w = g.s.walls[i];
        const Material& m = g.s.mats[(size_t)w.mat];
        for (int e = 0; e < 2; ++e) {
            if(freeOnly && g.jBegin(i,e)!=g.jEnd(i,e))continue;
            const double ex = e ? w.bx : w.ax, ey = e ? w.by : w.ay, fx = e ? w.ax : w.bx, fy = e ? w.ay : w.by;
            const double t = ((ex - S.x) * rx + (ey - S.y) * ry) / len2;
            if(!freeOnly && (t<=0 || t>=1) && g.jBegin(i,e)!=g.jEnd(i,e))continue;
            const double perp = std::fabs(cross2(rx, ry, ex - S.x, ey - S.y)) / len;
            if (perp > 3.0) continue;
            const double z = S.z + std::clamp(t,0.0,1.0) * (R.z - S.z);
            if (!edgeRelevant(w, m, z)) continue;
            // a wall running along the path hardly screens it; one across it screens fully
            const double sinTheta = std::fabs(cross2(rx, ry, fx - ex, fy - ey)) / (len * std::max(1e-9, std::hypot(fx - ex, fy - ey)));
            const Vec3 E{ex, ey, z};
            if(!freeOnly && (norm(E-S)<1e-6 || norm(E-R)<1e-6))continue;
            const double delta = freeOnly ? .5*perp*perp*(1./std::max(.05,norm(E-S))+1./std::max(.05,norm(R-E))) : norm(E - S) + norm(R - E) - dist3;
            // Above this conservative bound the original knifeEdgeDb returns EXACTLY zero.
            if(delta>0.60840000001*c/(2*kBandHz[0]))continue;
            const double taper=std::exp(-std::pow(std::max(0.0,std::max(-t,t-1.0))*len/.5,2));
            for (int b = 0; b < NB; ++b) {
                if(delta>0.60840000001*c/(2*kBandHz[b]))continue;
                bestDb[b] = std::max(bestDb[b], knifeEdgeDb(delta, kBandHz[b], c, false) * std::min(1.0, 2.0 * sinTheta) * taper);
            }
        }
    }
    // Plate rims have a Fresnel zone in 3D, including when S and R have equal height.
    const Vec3 D=R-S; const double d2=dot(D,D);
    const double L=norm(D), cutoff=0.60840000001*c/(2*kBandHz[0]);
    const double radius=std::sqrt(cutoff*(2*L+cutoff)*.25);
    const auto* list=ignore>=0?&g.litRimsBySurface[(size_t)ignore]:nullptr;
    for(size_t ri=0;ri<(list?list->size():g.screenRims.size());++ri) {
        E5B_COUNT(boxes,1);
        const auto& edge=g.screenRims[list?(*list)[ri]:ri];
        if(edge.surf==ignore)continue;
        // An edge outside this expanded segment AABB cannot intersect its Fresnel ellipsoid.
        auto far=[&](double a,double b,double x,double y){return std::min(a,b)>std::max(x,y)+radius || std::max(a,b)<std::min(x,y)-radius;};
        if(far(edge.A.x,edge.B.x,S.x,R.x)||far(edge.A.y,edge.B.y,S.y,R.y)||far(edge.A.z,edge.B.z,S.z,R.z))continue;
        E5B_COUNT(vertices,1);
        const Vec3 u=edge.u,A=edge.A;const double length=edge.length;
        const double a=dot(S-A,u),b=dot(R-A,u);
        const double h1=norm(S-A-u*a),h2=norm(R-A-u*b);
        const double at=(a*h2+b*h1)/std::max(1e-12,h1+h2);
        const Vec3 E=A+u*std::clamp(at,0.0,length);
        const double t=dot(E-S,D)/std::max(1e-12,d2);
        if(t<=0 || t>=1)continue;
        const double delta=norm(E-S)+norm(R-E)-L;
        if(delta>cutoff)continue;
        for(int band=0;band<NB;++band) {
            if(delta>0.60840000001*c/(2*kBandHz[band]))continue;
            bestDb[band]=std::max(bestDb[band],knifeEdgeDb(delta,kBandHz[band],c,false));
        }
    }
    for (int b = 0; b < NB; ++b) if(bestDb[b]!=0) {E5B_COUNT(powers,1);att[b] *= (float)std::pow(10.0, -bestDb[b] / 20.0);}
}

struct DiffPath {
    Vec3 first, last;   // the wall end the sound bends round first (next to the source) and last (next to the receiver)
    double inner = 0;   // length of the legs between the first and the last end (0 for a single end)
    double length = 0;  // S -> ... -> R
    float amp[NB] = {};
};

// E5: bounded one-edge continuation. Scan once, validate at most four candidates.
// Thus O(walls + plate vertices + junction rays), independent of the multi-edge search.
// A failed candidate is never allowed to bypass other blockers or a sealed junction.
inline bool singleEdgePath(const Geo& g, Vec3 S, Vec3 R, double c, DiffPath& out,
                           int ignore=-1, bool platesOnly=false) {
    if(g.separatedShell(S,R,false)) return false;
    if(platesOnly) {
        bool crossed=false;const double dz=R.z-S.z;
        if(std::abs(dz)>1e-12)for(int id=g.W+2;id<g.nSurf();++id) {
            const auto& p=g.plate(id);double t=(p.z-S.z)/dz;
            if(t>1e-6 && t<1-1e-6 && screens(g.mat(id)) && pointInZone(p,S.x+t*(R.x-S.x),S.y+t*(R.y-S.y)))crossed=true;
        }
        if(!crossed)return false;
    }
    struct Candidate { Vec3 p; int surf=-1,end=0; double delta=1e30; } best[4];
    int count=0; const double L=norm(R-S);
    auto offer=[&](Vec3 E,int surf,int end) {
        double delta=norm(E-S)+norm(R-E)-L;
        int j=count; while(j>0 && best[j-1].delta>delta)--j;
        if(j>=4)return;
        for(int k=std::min(3,count);k>j;--k)best[k]=best[k-1];
        best[j]={E,surf,end,delta};count=std::min(4,count+1);
    };
    const double dx=R.x-S.x,dy=R.y-S.y,l2=std::max(1e-12,dx*dx+dy*dy);
    if(!platesOnly)for(int wi=0;wi<(ignore>=0?(int)g.freeScreenWalls.size():g.W);++wi) {
        const int i=ignore>=0?g.freeScreenWalls[(size_t)wi]:wi;
        E5B_COUNT(walls,1);
        if(i==ignore)continue; const auto& w=g.s.walls[i];
        for(int e=0;e<2;++e) {
            if(ignore>=0 && g.jBegin(i,e)!=g.jEnd(i,e))continue;
            double x=e?w.bx:w.ax,y=e?w.by:w.ay;
            double t=std::clamp(((x-S.x)*dx+(y-S.y)*dy)/l2,0.0,1.0);
            double z=S.z+t*(R.z-S.z);
            if(edgeRelevant(w,g.mat(i),z))offer({x,y,z},i,e);
        }
    }
    const auto* rimList=ignore>=0?&g.litRimsBySurface[(size_t)ignore]:nullptr;
    for(size_t ri=0;ri<(rimList?rimList->size():g.screenRims.size());++ri) {
        const auto& edge=g.screenRims[rimList?(*rimList)[ri]:ri];
        E5B_COUNT(vertices,1);
        const Vec3 u=edge.u,A=edge.A;const double length=edge.length;
        const double a=dot(S-A,u),b=dot(R-A,u);
        const double h1=norm(S-A-u*a),h2=norm(R-A-u*b);
        const double at=(a*h2+b*h1)/std::max(1e-12,h1+h2);
        offer(A+u*std::clamp(at,0.0,length),edge.surf,0);
    }
    for(int j=0;j<count;++j) {
        const auto& e=best[j];float a[NB];std::fill_n(a,NB,1.f);
        E5B_COUNT(validationScans,2*(g.W+g.plates.size()+1));
        if(!transmitLeg(g,S,e.p,e.surf,ignore,a) || !transmitLeg(g,e.p,R,e.surf,ignore,a))continue;
        if(e.surf>=g.W && !touchCross(g,S,e.p,R,a))continue;
        if(e.surf<g.W) {E5B_COUNT(junctionRays,g.jEnd(e.surf,e.end)-g.jBegin(e.surf,e.end));const Bend bend{e.surf,e.end,e.p,S,R};if(!bendsOk(g,&bend,1,a))continue;}
        out.first=out.last=e.p;out.inner=0;out.length=L+e.delta;
        E5B_COUNT(powers,NB);
        for(int b=0;b<NB;++b)out.amp[b]=a[b]*(float)std::pow(10.,-knifeEdgeDb(e.delta,kBandHz[b],c,true)/20.);
        return true;
    }
    return false;
}
// ---- E6: welded edges on straight legs ----
// A leg passing a shared edge (a corner of two walls, a T junction, a coplanar seam between two materials) crosses the
// faces in the angular arc it sweeps round the edge; passing on the other side it would cross the faces of the other arc.
// The two transmissions T_own and T_other switch at the shadow boundary (the leg touching the edge). The hand-over keeps
// the higher one as the geometric field and lets the lower side take a knife-edge share of the difference:
//   lit (T_own >= T_other):    F = T_other + (T_own - T_other) * A_lit(delta)
//   shadow (T_own < T_other):  F = T_own   + (T_other - T_own) * A_shadow(delta)
// A_lit = A_shadow = 10^(-6.03/20) ~ 0.5 at delta = 0 (both formulas give (T_own + T_other) / 2: continuous); A_lit = 1
// for delta >= 0.6084 c / (2 f) and A_shadow is faded to 0 between D0 and D1 = 2 D0 (D0 = that bound at 125 Hz), so
// beyond D1 F = T_own exactly (E5b unchanged). min(T_own, T_other) <= F <= max(T_own, T_other): no energy is created.
// A leg whose ends both lie in one sector of at most 180 deg (inside a closed concave corner, beside a flat seam) does not
// pass the edge: no dip and no gain there.
inline double e6LitAmp(double delta, int b, double c) {
    E5B_COUNT(powers,2);
    const double db = knifeEdgeDb(delta, kBandHz[b], c, false);
    const double mid = std::pow(10.0, -(6.9 + 20.0*std::log10(std::sqrt(1.01)-.1))/20.0);
    return db == 0 ? 1.0 : .5 + .5*(std::pow(10.0,-db/20.0)-mid)/(1-mid);
}
inline double e6D0(double c) { return 0.60840000001 * c / (2 * kBandHz[0]); }
inline double e6ShadowAmp(double delta, int b, double c) {
    E5B_COUNT(powers,2);
    const double D0 = e6D0(c), D1 = 2 * D0;
    if (delta >= D1) return 0;
    double w = 1;
    if (delta > D0) { const double x = (delta - D0) / (D1 - D0); w = std::cos(.5 * kPi * x); w *= w; }
    const double mid = std::pow(10.0, -(6.9 + 20.0*std::log10(std::sqrt(1.01)-.1))/20.0);
    return w * .5/mid * std::pow(10.0, -knifeEdgeDb(delta, kBandHz[b], c, true) / 20.0);
}
inline double e6Blend(double own, double other, double delta, int b, double c) {
    if (own >= other) return other + (own - other) * e6LitAmp(delta, b, c);
    return own + (other - own) * e6ShadowAmp(delta, b, c);
}
// transmitLeg that ignores the faces in skip[0..ns)
inline bool transmitLegSkipping(const Geo& g, Vec3 a, Vec3 b, const int* skip, int ns, float* amp) {
    auto skipped = [&](int id) { for (int i = 0; i < ns; ++i) if (skip[i] == id) return true; return false; };
    const double rx = b.x - a.x, ry = b.y - a.y;
    for (int i = 0; i < g.W; ++i) {
        if (skipped(i)) continue;
        const Wall& w = g.s.walls[i];
        const double ex = w.bx - w.ax, ey = w.by - w.ay, den = cross2(rx, ry, ex, ey);
        if (std::fabs(den) < 1e-12) continue;
        const double t = cross2(w.ax - a.x, w.ay - a.y, ex, ey) / den, u = cross2(w.ax - a.x, w.ay - a.y, rx, ry) / den;
        if (t > 1e-6 && t < 1 - 1e-6 && u >= 0 && u <= 1) {
            const double zc = a.z + t * (b.z - a.z);
            if (zc < w.z0 || zc > w.z1) continue;
            const Material& m = g.s.mats[(size_t)w.mat];
            double mx = 0; for (int k = 0; k < NB; ++k) mx = std::max(mx, m.tau[k]);
            if (mx <= 0) return false;
            for (int k = 0; k < NB; ++k) amp[k] *= (float)std::sqrt(m.tau[k]);
        }
    }
    const double dz = b.z - a.z;
    if (std::fabs(dz) > 1e-12) for (int k = 0; k < (int)g.plates.size(); ++k) {
        const int id = g.W + 2 + k;
        if (skipped(id)) continue;
        const Zone& pl = *g.plates[(size_t)k];
        const double t = (pl.z - a.z) / dz;
        if (t <= 1e-6 || t >= 1 - 1e-6 || !pointInZone(pl, a.x + t * (b.x - a.x), a.y + t * (b.y - a.y))) continue;
        const Material& m = g.s.mats[(size_t)pl.mat];
        double mx = 0; for (int q = 0; q < NB; ++q) mx = std::max(mx, m.tau[q]);
        if (mx <= 0) return false;
        for (int q = 0; q < NB; ++q) amp[q] *= (float)std::sqrt(m.tau[q]);
    }
    return true;
}
// Accumulated hand-over of one leg: per band the strongest single dip (ratio <= 1) and gain (ratio >= 1) relative to
// the leg's own transmission, and the best hand-over value for a leg that is blocked on its own.
struct WeldLegSum { int n = 0; float dip[NB], gain[NB], best[NB]; WeldLegSum() { std::fill_n(dip, NB, 1.f); std::fill_n(gain, NB, 1.f); std::fill_n(best, NB, 0.f); } };
// The (at most 3, nearest first) shared edges whose hand-over changes leg a->b. Faces of surface `ign` (the reflecting
// face: its own edges are handled by the support taper) are not used. No allocation; O(legWelds) culled by boxes.
// own: the leg's real transmission (transmitLeg, 0 where it is blocked); the other side is the transmission of the
// rest of the leg times the faces in the other arc.
// mode 0: every leg-relevant edge on this leg. For a reflection on a horizontal face (floor, ceiling, plate) the path
// S -> P -> R is one straight line seen from above, so a vertical edge is passed by the whole path, not by a leg: the
// receiver leg (mode 1, far = source) carries the vertical edges on the path unfolded in the reflecting plane
// (zPlane), with ownPath = both legs' transmission and the touch at P; the source leg (mode 2) skips vertical edges.
inline int weldLegHits(const Geo& g, Vec3 a, Vec3 b, int ign, double c, const float* own, WeldLegSum& sum,
                       int mode = 0, Vec3 far = {}, double zPlane = 0, const float* ownPath = nullptr, const float* sourceLeg = nullptr) {
    if (g.legWelds.empty()) return 0;
    // Exterior-source continuation only. Interior shell sources retain the E5b model:
    // their material-changing transmission arcs require a separate coupled solver.
    const Vec3 origin = mode==2 ? b : far;
    for(const auto& sh:g.shells) if(origin.x>sh.x0-.05 && origin.x<sh.x1+.05 && origin.y>sh.y0-.05 && origin.y<sh.y1+.05) return 0;
    const double L = norm(b - a), D1 = 2 * e6D0(c);
    if (L < 1e-9) return 0;
    const Vec3 farImage{far.x, far.y, 2 * zPlane - far.z};
    const double Lp = mode == 1 ? norm(farImage - a) : 0;
    const double radius = std::sqrt(D1 * (2 * L + D1) * .25), radiusP = std::sqrt(D1 * (2 * Lp + D1) * .25);
    int count = 0;
    for (int k : g.legWelds) {
        E5B_COUNT(boxes,1);
        const auto& w = g.welds[(size_t)k];
        if (w.vertical ? (w.kind!=2 || mode!=1) : (mode==0)) continue;
        const bool whole = w.vertical && mode == 1;
        bool mine = false;
        for (int q = 0; q < w.n; ++q) mine |= w.face[q] == ign;
        if (mine) continue;
        auto apart = [](double p, double q, double x, double y, double r) { return std::min(p, q) > std::max(x, y) + r || std::max(p, q) < std::min(x, y) - r; };
        Vec3 X; double delta, beyond = 0; double ax, ay, bx, by;
        if (!whole) {
            if (apart(w.A.x, w.B.x, a.x, b.x, radius) || apart(w.A.y, w.B.y, a.y, b.y, radius) || apart(w.A.z, w.B.z, a.z, b.z, radius)) continue;
            // stationary point on the edge line; beyond an end that the line continues past, the next sub-segment owns it
            const double pa = dot(a - w.A, w.u), pb = dot(b - w.A, w.u);
            const double h1 = norm(a - w.A - w.u * pa), h2 = norm(b - w.A - w.u * pb);
            const double t = (pa * h2 + pb * h1) / std::max(1e-12, h1 + h2);
            if ((t < 0 && w.contA) || (t > w.len && w.contB)) continue;
            beyond = t < 0 ? -t : t > w.len ? t - w.len : 0;
            X = w.A + w.u * std::clamp(t, 0.0, w.len);
            delta = norm(a - X) + norm(b - X) - L;
            if (delta >= D1) continue;
            const Vec3 a1 = a + (b - a) * 1e-6, b1 = b + (a - b) * 1e-6;
            ax = dot(a1 - X, w.e1); ay = dot(a1 - X, w.e2); bx = dot(b1 - X, w.e1); by = dot(b1 - X, w.e2);
        } else {
            if (apart(w.A.x, w.B.x, a.x, far.x, radiusP) || apart(w.A.y, w.B.y, a.y, far.y, radiusP)) continue;
            // unfolded path a -> image of far; the edge and its image in the reflecting plane
            const double h1 = std::hypot(a.x - w.A.x, a.y - w.A.y), h2 = std::hypot(farImage.x - w.A.x, farImage.y - w.A.y);
            const double z = (a.z * h2 + farImage.z * h1) / std::max(1e-12, h1 + h2);
            const double r0 = w.A.z, r1 = w.B.z, m0 = 2 * zPlane - w.B.z, m1 = 2 * zPlane - w.A.z;
            const double zr = std::clamp(z, r0, r1), zm = std::clamp(z, m0, m1);
            const bool useReal = std::fabs(zr - z) <= std::fabs(zm - z);
            const double zx = useReal ? zr : zm;
            if (zx != z) {  // beyond an end: skip when the line continues there
                const bool atA = useReal ? zx == r0 : zx == m1;
                if ((atA && w.contA) || (!atA && w.contB)) continue;
                beyond = std::fabs(zx - z);
            }
            X = {w.A.x, w.A.y, zx};
            delta = norm(a - X) + norm(farImage - X) - Lp;
            if (delta >= D1) continue;
            const double fx = far.x - a.x, fy = far.y - a.y;
            ax = a.x + fx * 1e-6 - X.x; ay = a.y + fy * 1e-6 - X.y; bx = far.x - fx * 1e-6 - X.x; by = far.y - fy * 1e-6 - X.y;
        }
        // Beyond an end that nothing continues, the faces stop: the hand-over fades out over kWeldEndFade (the side
        // classification below treats the faces as unbounded along the edge, which only holds near the edge's extent).
        constexpr double kWeldEndFade = 0.1;
        if (beyond >= kWeldEndFade) continue;
        double fade = std::cos(.5 * kPi * beyond / kWeldEndFade); fade *= fade;
        if (std::hypot(ax, ay) < 1e-12 || std::hypot(bx, by) < 1e-12) continue;
        const double cr = cross2(ax, ay, bx, by), dp = ax * bx + ay * by;
        if (std::fabs(cr) < 1e-15 && dp > 0) continue;  // radial leg: it does not sweep round the edge
        const double start = cr >= 0 ? std::atan2(ay, ax) : std::atan2(by, bx), sweep = std::atan2(std::fabs(cr), dp);
        E5B_COUNT(junctionRays,w.n);
        bool inOther[Geo::Weld::kMax] = {}, inOwn[Geo::Weld::kMax] = {};
        int nOwn = 0;
        // a leg through the edge line itself: transmitLeg counts every face meeting there; use the side classification
        const bool singular = beyond == 0 && std::fabs(cr) <= 1e-7 * std::hypot(bx - ax, by - ay);
        double gapCcw = 2 * kPi, gapCw = 2 * kPi;
        for (int q = 0; q < w.n; ++q) {
            double rel = std::fmod(w.ang[q] - start, 2 * kPi); if (rel < 0) rel += 2 * kPi;
            inOwn[q] = rel > 1e-9 && rel < sweep - 1e-9;
            nOwn += inOwn[q];
            inOther[q] = rel > sweep + 1e-9 && rel < 2 * kPi - 1e-9;
            gapCcw = std::min(gapCcw, rel); gapCw = std::min(gapCw, 2 * kPi - rel);
        }
        
        if (nOwn == 0 && gapCcw + gapCw <= kPi + 1e-9) continue;  // both ends in one sector of <= 180 deg
        int skip[Geo::Weld::kMax + 1]; int ns = 0;
        for (int q = 0; q < w.n; ++q) skip[ns++] = w.face[q];
        skip[ns++] = ign;
        float rest[NB]; std::fill_n(rest, NB, 1.f);
        E5B_COUNT(validationScans, (whole ? 2 : 1) * (g.W + (int64_t)g.plates.size()));
        if (!transmitLegSkipping(g, a, b, skip, ns, rest)) continue;  // blocked elsewhere: nothing to hand over
        if (whole && !transmitLegSkipping(g, b, far, skip, ns, rest)) continue;
        const float* mine0 = whole ? ownPath : own;
        bool differs = false;
        float F[NB], o[NB];
        for (int band = 0; band < NB; ++band) {
            double other = rest[band], ownClass = rest[band];
            for (int q = 0; q < w.n; ++q) {
                const double amp = std::sqrt(w.mat[q] >= 0 ? g.s.mats[(size_t)w.mat[q]].tau[band] : 1.0);
                if (inOther[q]) other *= amp;
                if (inOwn[q]) ownClass *= amp;
            }
            const double mine = singular ? ownClass : mine0[band];
            o[band] = mine0[band];
            F[band] = (float)(mine + (e6Blend(mine, other, delta, band, c) - mine) * fade);
            differs |= F[band] != o[band];
        }
        if (!differs) continue;
#ifdef E6_DEBUG
        printf("    weld%d %s kind%d whole%d delta %.5f beyond %.4f singular%d own %.5f F %.5f (b3)\n", k, w.vertical ? "V" : "H", w.kind, (int)whole, delta, beyond, (int)singular, o[3], F[3]);
#endif
        ++count;
        for (int band = 0; band < NB; ++band) {
            if (o[band] > 1e-12) {
                const float r = F[band] / o[band];
                sum.dip[band] = std::min(sum.dip[band], r); sum.gain[band] = std::max(sum.gain[band], r);
            } else {
                // blocked on its own: the hand-over value itself, per leg (a whole-path value without the source leg)
                const float leg = whole ? (sourceLeg && sourceLeg[band] > 1e-12 ? F[band] / sourceLeg[band] : 0.f) : F[band];
                sum.best[band] = std::max(sum.best[band], leg);
            }
        }
    }
    sum.n = count;
    return count;
}
inline int64_t softEdgeWorkBound(const Geo& g) {
    int64_t edges=2*g.W;
    for(const auto* p:g.plates)edges+=(int64_t)p->poly.size();
    return edges + 4*(2*((int64_t)g.W+g.plates.size()+1)+(int64_t)g.jRays.size()+32)
        + 2*g.W + edges + 1 + (int64_t)g.legWelds.size()*(2*(g.W+g.plates.size())+64+32*NB);
}
// First-order reflected legs: lit and shadow use the same detour and -6 dB convention.
// The extra detour is represented by loss here; the specular tap retains its delay/direction.
// role (E6, reflections on a horizontal face only): 1 = receiver leg S = receiver, R = reflection point, far = source;
// 2 = the source leg. Callers that evaluate one reflected path pass both roles; 0 treats the leg on its own.
inline bool softReflectionLeg(const Geo& g, Vec3 S, Vec3 R, int ign1, int ign2, double c, float* amp, int role = 0, Vec3 far = {}) {
    if(!g.s.diffraction || ign2>=0)return transmitLeg(g,S,R,ign1,ign2,amp);
    struct ProfileScope { bool previous; ProfileScope():previous(profilingSoftLeg){profilingSoftLeg=true;++softProfile.legs;} ~ProfileScope(){profilingSoftLeg=previous;} } scope;
    float a[NB];std::fill_n(a,NB,1.f);
    if(!g.legWelds.empty()){
        const bool horizontal=ign1>=g.W;
        const int mode=horizontal?role:0;
        const double zPlane=ign1==g.W?0.0:ign1==g.W+1?g.H:ign1>=g.W+2?g.plate(ign1).z:0.0;
        float own[NB];std::fill_n(own,NB,1.f);
        const bool clear=transmitLeg(g,S,R,ign1,ign2,own);
        if(!clear)std::fill_n(own,NB,0.f);
        float rest[NB],ownPath[NB];std::fill_n(rest,NB,1.f);std::fill_n(ownPath,NB,0.f);
        if(mode==1){
            const bool ok=transmitLeg(g,R,far,ign1,-1,rest)&&touchCross(g,S,R,far,rest);
            if(!ok)std::fill_n(rest,NB,0.f);
            for(int b=0;b<NB;++b)ownPath[b]=own[b]*rest[b];
        }
        WeldLegSum sum; const int nh=weldLegHits(g,S,R,ign1,c,own,sum,mode,far,zPlane,ownPath,rest);
        if(nh>0){
            // E6: hand-over at the shared edges the leg passes (see weldLegHits); free edges as in E5b. Several edges near
            // one leg (a three-face corner): as for free edges in E5b, the strongest single dip and the strongest single
            // gain apply (a product would count the corner once per edge meeting there).
            bool any=false;
            for(int b=0;b<NB;++b){
                double v=own[b]*sum.dip[b]*sum.gain[b];
                v=std::max(v,(double)sum.best[b]);
                a[b]=(float)v;any|=a[b]>0;
            }
            if(clear){E5B_COUNT(lit,1);litEdgeAttenuation(g,S,R,c,a,ign1,true);}
            else{
                E5B_COUNT(shadow,1);DiffPath path;
                if(singleEdgePath(g,S,R,c,path,ign1)){const double spread=norm(R-S)/std::max(.05,path.length);
                    for(int b=0;b<NB;++b)a[b]=std::max(a[b],path.amp[b]*(float)spread);any=true;}
                if(!any)return false;
            }
            for(int b=0;b<NB;++b)amp[b]*=a[b];
            return true;
        }
        std::fill_n(a,NB,1.f);
    }
    if(transmitLeg(g,S,R,ign1,ign2,a)){ E5B_COUNT(lit,1);litEdgeAttenuation(g,S,R,c,a,ign1,true); }
    else {
        E5B_COUNT(shadow,1); DiffPath path;if(!singleEdgePath(g,S,R,c,path,ign1))return false;
        const double spread=norm(R-S)/std::max(.05,path.length);
        for(int b=0;b<NB;++b)a[b]=path.amp[b]*(float)spread;
    }
    for(int b=0;b<NB;++b)amp[b]*=a[b];
    return true;
}

// Audio callers reserve once for the scene's wall budget; offline callers may use
// the local fallback. Reusing these buffers does not change path order or gains.
struct DiffractionScratch {
    uint64_t capacityMisses=0; // owner may inspect; never silently discard a path
    struct Node { Vec3 p; int wall; int end; };  // end: 0 = the wall's a, 1 = its b
    std::vector<Node> nodes;
    std::vector<int> a, b;
    std::vector<float> ampA, ampB;
    // E3b: memos of the multi-end search (N <= 160 nodes): clear legs, corner tests at the first / last node, leg directions
    std::vector<unsigned char> legState, b0State, b2State;
    std::vector<float> legAmp3, b0Amp3, b2Amp3;  // band-3 transmission of a leg / of the corner crossings of a bend
    std::vector<double> ang;
    std::vector<int> kList;  // per middle end: the ends that can follow it
    void prepare(size_t walls) {
        const auto n=walls*2;
        nodes.reserve(n);a.reserve(n);b.reserve(n);
        ampA.resize(n*NB);ampB.resize(n*NB);
        const size_t m=std::min<size_t>(160,n);  // the multi-end search runs only for N <= 160 nodes, and N <= 2 * walls
        legState.assign(m*m,0);b0State=b2State=legState;legAmp3.assign(m*m,0.f);b0Amp3=b2Amp3=legAmp3;ang.assign(m*m,0.0);kList.assign(m*m,0);
    }
    // E3d: enough for the one-end stage only (DiffractionSearch::run with quickOnly)
    void prepareNodes(size_t walls) { nodes.reserve(walls * 2); }
};

// The direct path is blocked: find the way round the wall ends that the sound takes best. One end first (both legs
// clear); if none works (a closed building: the sound has to go round two corners), two, then three ends in a row.
// Every bend attenuates by its own knife-edge loss, so the level falls smoothly while the source goes behind a
// building and never switches on or off.
//
// E3d: the search is a resumable object. run() does the work in the original order, with the original arithmetic, and
// stops at a loop boundary when its work budget is spent; the next run() continues exactly there. The result is
// therefore bit-identical to a single uninterrupted run, however the work is split. Work is counted in units roughly
// proportional to time: a straight leg costs (walls + plates + 1), a corner test (rays at the junction + 2), one loop
// step 1. One loop step never costs more than a few legs and corner tests, so a run never overshoots its budget by more
// than that. `seq` names the wall ends of the path found (in order), so the engine can follow the same way round while a
// new search is in progress (evalDiffSeq).
struct DiffSeq {
    int n = 0;            // number of wall ends (0 = no path)
    int wall[3] = {}, end[3] = {};
};

struct DiffractionSearch {
    using Node = DiffractionScratch::Node;
    // inputs
    const Geo* g = nullptr;
    Vec3 S{}, R{};
    double c = 343.2;
    bool needClear = false;
    DiffractionScratch* work = nullptr;
    // result
    bool done = false, found = false;
    bool needsMulti = false;  // quick run (stage 1 only) ended without a decision: the multi-end search is needed
    DiffPath out;
    DiffSeq seq;
    int64_t units = 0;        // total work so far
    // ---- state ----
    enum Stage { kInit, kScan1, kEval1, kLists, kMemo, kTwo, kThree, kFinal, kDone };
    Stage stage = kDone;
    int N = 0;
    double dist3 = 0, rx = 0, ry = 0, len2 = 1;
    struct Cand { double delta; int idx; };
    Cand best[4] = {};
    int nb = 0, j1 = 0, k1 = 0;
    bool prune = false, listsStarted = false;
    int ia = 0, ib = 0, m3 = 0, phase = 0, kb = 0, kc3 = 0, ci = 0;
    double l1 = 0;
    float a1[NB] = {};
    double bestLen = 1e30;
    int bj = -1, bm = -1, bk = -1;
    float bestAmp[NB] = {};
    double angS[160] = {}, angR[160] = {};
    bool sealed[160] = {};
    int kCnt[160] = {};
    int64_t legCost = 1;
    static constexpr double kLoud = (double)0.7f, kSlack = 1.001;

    void start(const Geo& geo, Vec3 s, Vec3 r, double cc, bool nc, DiffractionScratch& w) {
        g = &geo; S = s; R = r; c = cc; needClear = nc; work = &w;
        done = found = needsMulti = false;
        out = DiffPath{};
        seq = DiffSeq{};
        units = 0;
        stage = kInit;
        listsStarted = false;
        legCost = (int64_t)geo.W + (int64_t)geo.plates.size() + 1;
    }
    void finish(bool f) { found = f; done = true; stage = kDone; }
    int64_t cornerCost(int wall, int end) const { return (int64_t)(g->jEnd(wall, end) - g->jBegin(wall, end)) + 2; }
    bool leg(Vec3 a, Vec3 b, int i1, int i2, float* amp) { units += legCost; return transmitLeg(*g, a, b, i1, i2, amp); }
    bool corner(const Bend& bd, int sIn, int sOut, float* amp, bool* tie, double a0 = kNoAngle, double a1r = kNoAngle) {
        units += cornerCost(bd.wall, bd.end);
        return cornerCore(*g, bd, sIn, sOut, amp, tie, a0, a1r);
    }
    bool bends(const Bend* bd, int n, float* amp, const double* r0 = nullptr, const double* r1 = nullptr) {
        int64_t cost = 0;
        const bool ok = bendsOk(*g, bd, n, amp, r0, r1, &cost);
        units += cost;
        return ok;
    }
    void setAmp(const Vec3* pts, int n, const float* legAmp) {  // pts[0]=S ... pts[n-1]=R: knife-edge loss at every bend
        units += 2 * n;
        for (int b = 0; b < NB; ++b) {
            double db = 0;
            for (int k = 1; k + 1 < n; ++k) {
                const double delta = norm(pts[k] - pts[k - 1]) + norm(pts[k + 1] - pts[k]) - norm(pts[k + 1] - pts[k - 1]);
                db += knifeEdgeDb(delta, kBandHz[b], c, true);
            }
            out.amp[b] = legAmp[b] * (float)std::pow(10.0, -db / 20.0);
        }
    }
    // ---- the memos of the multi-end search (E3b) ----
    std::vector<Node>& nodes() { return work->nodes; }
    size_t id(int u, int v) const { return (size_t)u * (size_t)N + (size_t)v; }
    double angNN(int u, int v) {  // direction from node u to node v, as the corner test at u computes it
        if (!sealed[u]) return kNoAngle;
        double& a = work->ang[id(u, v)];
        if (std::isnan(a)) a = std::atan2(work->nodes[v].p.y - work->nodes[u].p.y, work->nodes[v].p.x - work->nodes[u].p.x);
        return a;
    }
    double angToS(int u) {
        if (!sealed[u]) return kNoAngle;
        if (std::isnan(angS[u])) angS[u] = std::atan2(S.y - work->nodes[u].p.y, S.x - work->nodes[u].p.x);
        return angS[u];
    }
    double angToR(int u) {
        if (!sealed[u]) return kNoAngle;
        if (std::isnan(angR[u])) angR[u] = std::atan2(R.y - work->nodes[u].p.y, R.x - work->nodes[u].p.x);
        return angR[u];
    }
    // Largest band-3 factor this bend can have: the permissive reading (legs on a wall count for neither side) or any of
    // the four readings of bendsOk's face search (each leg shifted a hair to one side). bendsOk's result for a whole path
    // is a product of one such reading per bend, so it is never larger than the product of these caps.
    float bendCap3(const Bend& bd, double c0, double c1, float permissive3) {
        float best3 = permissive3;
        for (int si = -1; si <= 1; si += 2)
            for (int so = -1; so <= 1; so += 2) {
                float t[NB];
                for (int b = 0; b < NB; ++b) t[b] = 1.f;
                if (corner(bd, si, so, t, nullptr, c0, c1)) best3 = std::max(best3, t[3]);
            }
        return best3;
    }
    // Upper bound of the band-3 factor bendsOk gives a whole path: the product of the permissive readings when no leg lies on
    // a wall, otherwise the best consistent choice of the faces the along-wall legs keep to (every combination, as bendsOk
    // tries them). The final amplitude is never larger than this (0 where bendsOk would refuse the path).
    double pathCap3(const Bend* bd, int n, const double* r0, const double* r1) {
        double perm = 1.0;
        bool tieAny = false;
        float f[3][2][2];
        for (int k = 0; k < n; ++k) {
            float t[NB];
            for (int b = 0; b < NB; ++b) t[b] = 1.f;
            bool tie = false;
            if (!corner(bd[k], 0, 0, t, &tie, r0[k], r1[k])) perm = 0.0; else perm *= t[3];
            tieAny = tieAny || tie;
            for (int si = 0; si < 2; ++si)
                for (int so = 0; so < 2; ++so) {
                    float u[NB];
                    for (int b = 0; b < NB; ++b) u[b] = 1.f;
                    f[k][si][so] = corner(bd[k], si ? 1 : -1, so ? 1 : -1, u, nullptr, r0[k], r1[k]) ? u[3] : 0.f;
                }
        }
        double best = 0.0;
        for (int mask = 0; mask < (1 << (n + 1)); ++mask) {
            double pr = 1.0;
            for (int k = 0; k < n && pr > 0.0; ++k) pr *= f[k][(mask >> k & 1) ? 0 : 1][(mask >> (k + 1) & 1) ? 1 : 0];
            best = std::max(best, pr);
        }
        return tieAny ? best : perm;  // bendsOk uses the permissive reading only when no leg lies on a wall
    }
    bool legClear(int u, int v) {
        const size_t i = id(u, v);
        if (!work->legState[i]) {
            float t[NB];
            for (int b = 0; b < NB; ++b) t[b] = 1.f;
            const auto& nd = work->nodes;
            const bool ok = leg(nd[u].p, nd[v].p, nd[u].wall, nd[v].wall, t);
            work->legState[i] = ok ? 1 : 2;
            work->legAmp3[i] = ok ? t[3] : 0.f;
        }
        return work->legState[i] == 1;
    }
    bool bend0(int u, int v) {  // corner at u on the way S -> u -> v: false = the corner test refuses it outright
        const size_t i = id(u, v);
        if (!work->b0State[i]) {
            float pf[NB];
            for (int b = 0; b < NB; ++b) pf[b] = 1.f;
            bool tie = false;
            const auto& nd = work->nodes;
            const Bend bd{nd[u].wall, nd[u].end, nd[u].p, S, nd[v].p};
            const double c0 = angToS(u), c1 = angNN(u, v);
            const bool ok = corner(bd, 0, 0, pf, &tie, c0, c1);
            work->b0State[i] = ok ? 1 : 2;
            work->b0Amp3[i] = ok ? bendCap3(bd, c0, c1, pf[3]) : 0.f;
        }
        return work->b0State[i] == 1;
    }
    bool bend2(int u, int v) {  // corner at v on the way u -> v -> R
        const size_t i = id(u, v);
        if (!work->b2State[i]) {
            float pf[NB];
            for (int b = 0; b < NB; ++b) pf[b] = 1.f;
            bool tie = false;
            const auto& nd = work->nodes;
            const Bend bd{nd[v].wall, nd[v].end, nd[v].p, nd[u].p, R};
            const double c0 = angNN(v, u), c1 = angToR(v);
            const bool ok = corner(bd, 0, 0, pf, &tie, c0, c1);
            work->b2State[i] = ok ? 1 : 2;
            work->b2Amp3[i] = ok ? bendCap3(bd, c0, c1, pf[3]) : 0.f;
        }
        return work->b2State[i] == 1;
    }
    static bool close(Vec3 a, Vec3 b) { return norm(a - b) < 1e-6; }

    // Runs until the search is finished (returns true) or `budget` work units are spent (returns false; call again to
    // continue). budget < 0: no limit. quickOnly: stop after the one-end stage; if that does not decide the answer the
    // search ends with needsMulti = true (done stays false) and must be started again in full to finish.
    bool run(int64_t budget, bool quickOnly = false) {
        const int64_t stopAt = budget < 0 ? std::numeric_limits<int64_t>::max() : units + budget;
        auto out_of_budget = [&] { return units >= stopAt; };
        auto& w = *work;
        auto& nd = w.nodes;
        while (stage != kDone) {
            switch (stage) {
            case kInit: {
                units += (int64_t)g->shells.size() + 1;
                if (g->separatedShell(S, R, needClear)) { finish(false); break; }
                nd.clear();
                if (nd.capacity() < (size_t)g->W * 2) { ++w.capacityMisses; finish(false); break; }
                dist3 = norm(R - S);
                rx = R.x - S.x; ry = R.y - S.y; len2 = std::max(1e-12, rx * rx + ry * ry);
                units += (int64_t)g->W + 1;
                for (int i = 0; i < g->W; ++i) {
                    const Wall& wl = g->s.walls[i];
                    for (int e = 0; e < 2; ++e) {
                        const double ex = e ? wl.bx : wl.ax, ey = e ? wl.by : wl.ay;
                        double t = ((ex - S.x) * rx + (ey - S.y) * ry) / len2;
                        t = std::min(1.0, std::max(0.0, t));
                        const double z = S.z + t * (R.z - S.z);
                        if (z < 0 || z > g->H) continue;
                        nd.push_back({Vec3{ex, ey, z}, i, e});
                    }
                }
                N = (int)nd.size();
                nb = 0; j1 = 0;
                stage = kScan1;
                break;
            }
            case kScan1:  // 1. one wall end (the smallest detours first)
                for (; j1 < N; ++j1) {
                    if (out_of_budget()) return false;
                    units += 1;
                    const int j = j1;
                    const double delta = norm(nd[j].p - S) + norm(R - nd[j].p) - dist3;
                    int pos = nb;
                    while (pos > 0 && best[pos - 1].delta > delta) --pos;
                    if (pos >= 4) continue;
                    { const Bend bd{nd[j].wall, nd[j].end, nd[j].p, S, R}; if (!bends(&bd, 1, nullptr)) continue; }  // a sealed corner is no way round (and must not take a place among the best four)
                    for (int k = std::min(nb, 3); k > pos; --k) best[k] = best[k - 1];
                    best[pos] = {delta, j};
                    nb = std::min(nb + 1, 4);
                }
                k1 = 0;
                stage = kEval1;
                break;
            case kEval1:
                for (; k1 < nb; ++k1) {
                    if (out_of_budget()) return false;
                    units += 1;
                    const Node& n1 = nd[best[k1].idx];
                    float amp[NB];
                    for (int b = 0; b < NB; ++b) amp[b] = 1.f;
                    if (!leg(S, n1.p, n1.wall, -1, amp) || !leg(n1.p, R, n1.wall, -1, amp)) continue;
                    { const Bend bd{n1.wall, n1.end, n1.p, S, R}; if (!bends(&bd, 1, amp)) continue; }
                    if (needClear && amp[3] < 0.7f) continue;  // both legs must be in the open: the sound goes round, not through
                    units += 2 * (int64_t)g->W + 2;
                    litEdgeAttenuation(*g, S, n1.p, c, amp);  // other wall ends close beside the two legs still trim them,
                    litEdgeAttenuation(*g, n1.p, R, c, amp);  // so the level is continuous where a second corner takes over
                    const Vec3 pts[3] = {S, n1.p, R};
                    out.first = out.last = n1.p;
                    out.inner = 0;
                    out.length = norm(n1.p - S) + norm(R - n1.p);
                    setAmp(pts, 3, amp);
                    seq.n = 1; seq.wall[0] = n1.wall; seq.end[0] = n1.end;
                    finish(true);
                    return true;
                }
                if (N > 160) { finish(false); break; }  // a scene this busy: keep the cost bounded
                if (quickOnly) { needsMulti = true; return false; }
                stage = kLists;
                break;
            case kLists:  // 2. round several ends: which ends does the source see, which does the receiver see
                // needClear: a path whose mid-band amplitude is already below 0.7 can only get quieter (every factor is <= 1),
                // so it would be refused at the end anyway; dropping it early changes nothing but the cost (E3b).
                if (!listsStarted) {
                    prune = needClear && g->ampNonIncreasing;
                    w.a.clear(); w.b.clear();
                    if (w.a.capacity() < (size_t)N || w.b.capacity() < (size_t)N || w.ampA.size() < (size_t)N * NB || w.ampB.size() < (size_t)N * NB) { ++w.capacityMisses; finish(false); break; }
                    j1 = 0;
                    listsStarted = true;
                }
                for (; j1 < N; ++j1) {
                    if (out_of_budget()) return false;
                    const int j = j1;
                    float a[NB], bb[NB];
                    for (int b = 0; b < NB; ++b) a[b] = bb[b] = 1.f;
                    if (leg(S, nd[j].p, nd[j].wall, -1, a) && !(prune && a[3] < 0.7f)) { w.a.push_back(j); std::copy(a, a + NB, w.ampA.begin() + (long)j * NB); }
                    if (leg(nd[j].p, R, nd[j].wall, -1, bb) && !(prune && bb[3] < 0.7f)) { w.b.push_back(j); std::copy(bb, bb + NB, w.ampB.begin() + (long)j * NB); }
                }
                stage = kMemo;
                break;
            case kMemo: {
                // Each memo depends on the geometry only, so it is worked out once per search (lazily) instead of once per
                // candidate path. They only reject a candidate for a reason the original order of tests would also have
                // found, or (needClear only) because the amplitude is already below the 0.7 the path must keep: every
                // factor is <= 1, so the path can only get quieter, and a 0.1 % slack covers float rounding. Candidates that
                // survive run exactly the original arithmetic.
                const size_t NN = (size_t)N * (size_t)N;
                if (w.legState.size() < NN || w.legAmp3.size() < NN || w.b0State.size() < NN || w.b0Amp3.size() < NN || w.b2State.size() < NN || w.b2Amp3.size() < NN || w.ang.size() < NN || w.kList.size() < NN) { ++w.capacityMisses; finish(false); break; }
                units += (int64_t)(NN / 16) + N + 1;
                std::fill(w.legState.begin(), w.legState.begin() + (long)NN, (unsigned char)0);
                std::fill(w.b0State.begin(), w.b0State.begin() + (long)NN, (unsigned char)0);
                std::fill(w.b2State.begin(), w.b2State.begin() + (long)NN, (unsigned char)0);
                std::fill(w.ang.begin(), w.ang.begin() + (long)NN, kNoAngle);
                for (int u = 0; u < N; ++u) {
                    angS[u] = angR[u] = kNoAngle;
                    sealed[u] = g->jEnd(nd[u].wall, nd[u].end) - g->jBegin(nd[u].wall, nd[u].end) >= 2;  // a free end: the corner test returns at once
                    kCnt[u] = -1;
                }
                bestLen = 1e30; bj = bm = bk = -1;
                ia = ib = 0;
                stage = kTwo;
                break;
            }
            case kTwo: {  // two ends
                const auto& A = w.a;
                const auto& B = w.b;
                for (; ia < (int)A.size(); ++ia, ib = 0) {
                    const int j = A[(size_t)ia];
                    for (; ib < (int)B.size(); ++ib) {
                        if (out_of_budget()) return false;
                        units += 1;
                        const int k = B[(size_t)ib];
                        if (j == k || close(nd[j].p, nd[k].p)) continue;
                        const double len = norm(nd[j].p - S) + norm(nd[k].p - nd[j].p) + norm(R - nd[k].p);
                        if (len >= bestLen) continue;
                        if (!legClear(j, k)) continue;
                        if (prune && (double)w.ampA[(size_t)j * NB + 3] * w.ampB[(size_t)k * NB + 3] * w.legAmp3[id(j, k)] * kSlack < kLoud) continue;
                        if (!bend0(j, k) || !bend2(j, k)) continue;  // bendsOk would refuse it at its first test
                        float a[NB];
                        for (int b = 0; b < NB; ++b) a[b] = w.ampA[(size_t)j * NB + b] * w.ampB[(size_t)k * NB + b];
                        if (prune && a[3] < 0.7f) continue;
                        if (prune && (double)a[3] * w.legAmp3[id(j, k)] * w.b0Amp3[id(j, k)] * w.b2Amp3[id(j, k)] * kSlack < kLoud) continue;
                        {
                            const Bend bd[2] = {{nd[j].wall, nd[j].end, nd[j].p, S, nd[k].p}, {nd[k].wall, nd[k].end, nd[k].p, nd[j].p, R}};
                            const double r0[2] = {angToS(j), angNN(k, j)}, r1[2] = {angNN(j, k), angToR(k)};
                            if (!bends(bd, 2, a, r0, r1)) continue;
                        }
                        if (!leg(nd[j].p, nd[k].p, nd[j].wall, nd[k].wall, a)) continue;
                        if (needClear && a[3] < 0.7f) continue;
                        bestLen = len; bj = j; bm = -1; bk = k;
                        std::copy(a, a + NB, bestAmp);
                    }
                }
                if (bj >= 0) { stage = kFinal; break; }
                ia = 0; m3 = 0; phase = 0;  // three ends, only if two did not work
                stage = kThree;
                break;
            }
            case kThree: {
                const auto& A = w.a;
                const auto& B = w.b;
                for (; ia < (int)A.size(); ++ia, m3 = 0) {
                    const int j = A[(size_t)ia];
                    for (; m3 < N; ++m3, phase = 0) {
                        const int m = m3;
                        if (phase == 0) {
                            if (out_of_budget()) return false;
                            units += 1;
                            if (m == j || close(nd[j].p, nd[m].p)) continue;
                            l1 = norm(nd[j].p - S) + norm(nd[m].p - nd[j].p);
                            if (l1 >= bestLen) continue;
                            if (!legClear(j, m)) continue;
                            if (prune && (double)w.ampA[(size_t)j * NB + 3] * w.legAmp3[id(j, m)] * kSlack < kLoud) continue;
                            for (int b = 0; b < NB; ++b) a1[b] = w.ampA[(size_t)j * NB + b];
                            if (!bend0(j, m)) continue;  // a sealed corner is no way round
                            if (prune && (double)w.ampA[(size_t)j * NB + 3] * w.legAmp3[id(j, m)] * w.b0Amp3[id(j, m)] * kSlack < kLoud) continue;
                            phase = 1;
                            kb = 0;
                        }
                        if (phase == 1) {  // the ends k that can follow m whatever j is (worked out once per m, in B's order)
                            int* l = w.kList.data() + (size_t)m * (size_t)N;
                            if (kCnt[m] < 0) {
                                if (kb == 0) kc3 = 0;
                                for (; kb < (int)B.size(); ++kb) {
                                    if (out_of_budget()) return false;
                                    units += 1;
                                    const int k = B[(size_t)kb];
                                    if (k == m || close(nd[m].p, nd[k].p)) continue;
                                    if (!legClear(m, k)) continue;
                                    const size_t mk = id(m, k);
                                    if (prune && (double)w.ampB[(size_t)k * NB + 3] * w.legAmp3[mk] * kSlack < kLoud) continue;
                                    if (!bend2(m, k)) continue;
                                    if (prune && (double)w.ampB[(size_t)k * NB + 3] * w.legAmp3[mk] * w.b2Amp3[mk] * kSlack < kLoud) continue;
                                    l[kc3++] = k;
                                }
                                kCnt[m] = kc3;
                            }
                            if (kCnt[m] == 0) continue;
                            if (!leg(nd[j].p, nd[m].p, nd[j].wall, nd[m].wall, a1)) continue;
                            if (prune && a1[3] < 0.7f) continue;
                            const double q0 = (double)a1[3] * w.b0Amp3[id(j, m)];
                            if (prune && q0 * kSlack < kLoud) continue;
                            phase = 2;
                            ci = 0;
                        }
                        // phase 2: the third ends
                        const int* kl = w.kList.data() + (size_t)m * (size_t)N;
                        const int kc = kCnt[m];
                        for (; ci < kc; ++ci) {
                            if (out_of_budget()) return false;
                            units += 1;
                            const int k = kl[ci];
                            if (close(nd[j].p, nd[k].p)) continue;
                            const double len = l1 + norm(nd[k].p - nd[m].p) + norm(R - nd[k].p);
                            if (len >= bestLen) continue;
                            float a[NB];
                            for (int b = 0; b < NB; ++b) a[b] = a1[b] * w.ampB[(size_t)k * NB + b];
                            if (prune && a[3] < 0.7f) continue;
                            if (prune) {
                                const size_t mk = id(m, k);
                                const double q1 = (double)a[3] * w.legAmp3[mk] * w.b2Amp3[mk] * w.b0Amp3[id(j, m)];
                                if (q1 * kSlack < kLoud) continue;
                                float p1[NB];
                                for (int b = 0; b < NB; ++b) p1[b] = 1.f;
                                bool tie1 = false;
                                const Bend bmid{nd[m].wall, nd[m].end, nd[m].p, nd[j].p, nd[k].p};
                                const double c0 = angNN(m, j), c1 = angNN(m, k);
                                if (!corner(bmid, 0, 0, p1, &tie1, c0, c1)) continue;
                                if (q1 * bendCap3(bmid, c0, c1, p1[3]) * kSlack < kLoud) continue;
                                {   // the same bound with the face choices of the legs tied together, as in bendsOk's search
                                    const Bend bd3[3] = {{nd[j].wall, nd[j].end, nd[j].p, S, nd[m].p}, bmid, {nd[k].wall, nd[k].end, nd[k].p, nd[m].p, R}};
                                    const double r0[3] = {angToS(j), angNN(m, j), angNN(k, m)}, r1[3] = {angNN(j, m), angNN(m, k), angToR(k)};
                                    if ((double)a[3] * w.legAmp3[mk] * pathCap3(bd3, 3, r0, r1) * kSlack < kLoud) continue;
                                }
                            }
                            {
                                const Bend bd[3] = {{nd[j].wall, nd[j].end, nd[j].p, S, nd[m].p}, {nd[m].wall, nd[m].end, nd[m].p, nd[j].p, nd[k].p}, {nd[k].wall, nd[k].end, nd[k].p, nd[m].p, R}};
                                const double r0[3] = {angToS(j), angNN(m, j), angNN(k, m)}, r1[3] = {angNN(j, m), angNN(m, k), angToR(k)};
                                if (!bends(bd, 3, a, r0, r1)) continue;
                            }
                            if (!leg(nd[m].p, nd[k].p, nd[m].wall, nd[k].wall, a)) continue;
                            if (needClear && a[3] < 0.7f) continue;
                            bestLen = len; bj = j; bm = m; bk = k;
                            std::copy(a, a + NB, bestAmp);
                        }
                    }
                }
                stage = kFinal;
                break;
            }
            case kFinal: {
                if (bj < 0) { finish(false); break; }
                Vec3 pts[5];
                int n = 0;
                pts[n++] = S;
                pts[n++] = nd[bj].p;
                if (bm >= 0) pts[n++] = nd[bm].p;
                pts[n++] = nd[bk].p;
                pts[n++] = R;
                out.first = nd[bj].p;
                out.last = nd[bk].p;
                out.length = bestLen;
                out.inner = bestLen - norm(nd[bj].p - S) - norm(R - nd[bk].p);
                setAmp(pts, n, bestAmp);
                seq.n = 0;
                seq.wall[seq.n] = nd[bj].wall; seq.end[seq.n++] = nd[bj].end;
                if (bm >= 0) { seq.wall[seq.n] = nd[bm].wall; seq.end[seq.n++] = nd[bm].end; }
                seq.wall[seq.n] = nd[bk].wall; seq.end[seq.n++] = nd[bk].end;
                finish(true);
                break;
            }
            case kDone:
                break;
            }
        }
        return true;
    }
};

inline bool diffractedPath(const Geo& g, Vec3 S, Vec3 R, double c, DiffPath& out, bool needClear = false, DiffractionScratch* scratch = nullptr) {
    if(g.separatedShell(S,R,needClear))return false;
    if(!scratch){DiffractionScratch local;local.prepare((size_t)g.W);return diffractedPath(g,S,R,c,out,needClear,&local);}
    DiffractionSearch q;
    q.start(g, S, R, c, needClear, *scratch);
    q.run(-1);
    if (q.found) out = q.out;
    return q.found;
}

// E3d: the path along the given wall ends, for new source / receiver positions. Exactly the tests and the arithmetic the
// search applies to that candidate (the two- and three-end prunes only drop candidates these tests refuse). True with
// `out` filled if the path exists; false if it does not (a leg or a corner is now blocked, or needClear no longer holds).
// Cost: a few straight legs and corner tests (`units` is increased by the work, as in DiffractionSearch).
inline bool evalDiffSeq(const Geo& g, Vec3 S, Vec3 R, double c, bool needClear, const DiffSeq& sq, DiffPath& out, int64_t& units) {
    if (sq.n < 1 || sq.n > 3) return false;
    const int64_t legCost = (int64_t)g.W + (int64_t)g.plates.size() + 1;
    const double rx = R.x - S.x, ry = R.y - S.y, len2 = std::max(1e-12, rx * rx + ry * ry);
    Vec3 p[3];
    for (int i = 0; i < sq.n; ++i) {
        if (sq.wall[i] < 0 || sq.wall[i] >= g.W) return false;
        const Wall& w = g.s.walls[(size_t)sq.wall[i]];
        const double ex = sq.end[i] ? w.bx : w.ax, ey = sq.end[i] ? w.by : w.ay;
        double t = ((ex - S.x) * rx + (ey - S.y) * ry) / len2;
        t = std::min(1.0, std::max(0.0, t));
        const double z = S.z + t * (R.z - S.z);
        if (z < 0 || z > g.H) return false;
        p[i] = Vec3{ex, ey, z};
    }
    auto close = [](Vec3 a, Vec3 b) { return norm(a - b) < 1e-6; };
    auto setAmp = [&](const Vec3* pts, int n, const float* legAmp) {
        for (int b = 0; b < NB; ++b) {
            double db = 0;
            for (int k = 1; k + 1 < n; ++k) {
                const double delta = norm(pts[k] - pts[k - 1]) + norm(pts[k + 1] - pts[k]) - norm(pts[k + 1] - pts[k - 1]);
                db += knifeEdgeDb(delta, kBandHz[b], c, true);
            }
            out.amp[b] = legAmp[b] * (float)std::pow(10.0, -db / 20.0);
        }
    };
    const int* wl = sq.wall;
    const int* en = sq.end;
    if (sq.n == 1) {
        units += 2 * legCost + 2 * (int64_t)g.W + 8;
        float amp[NB];
        for (int b = 0; b < NB; ++b) amp[b] = 1.f;
        if (!transmitLeg(g, S, p[0], wl[0], -1, amp) || !transmitLeg(g, p[0], R, wl[0], -1, amp)) return false;
        { const Bend bd{wl[0], en[0], p[0], S, R}; if (!bendsOk(g, &bd, 1, amp)) return false; }
        if (needClear && amp[3] < 0.7f) return false;
        litEdgeAttenuation(g, S, p[0], c, amp);
        litEdgeAttenuation(g, p[0], R, c, amp);
        const Vec3 pts[3] = {S, p[0], R};
        out.first = out.last = p[0];
        out.inner = 0;
        out.length = norm(p[0] - S) + norm(R - p[0]);
        setAmp(pts, 3, amp);
        return true;
    }
    const int j = 0, k = sq.n - 1;
    float ampA[NB], ampB[NB];
    for (int b = 0; b < NB; ++b) ampA[b] = ampB[b] = 1.f;
    units += 3 * legCost + 16;
    if (!transmitLeg(g, S, p[j], wl[j], -1, ampA)) return false;
    if (!transmitLeg(g, p[k], R, wl[k], -1, ampB)) return false;
    if (close(p[j], p[k])) return false;
    float a[NB];
    double len;
    if (sq.n == 2) {
        for (int b = 0; b < NB; ++b) a[b] = ampA[b] * ampB[b];
        const Bend bd[2] = {{wl[j], en[j], p[j], S, p[k]}, {wl[k], en[k], p[k], p[j], R}};
        if (!bendsOk(g, bd, 2, a)) return false;
        if (!transmitLeg(g, p[j], p[k], wl[j], wl[k], a)) return false;
        if (needClear && a[3] < 0.7f) return false;
        len = norm(p[j] - S) + norm(p[k] - p[j]) + norm(R - p[k]);
    } else {
        units += legCost;
        if (close(p[j], p[1]) || close(p[1], p[k])) return false;
        float b1[NB];
        for (int b = 0; b < NB; ++b) b1[b] = ampA[b];
        if (!transmitLeg(g, p[j], p[1], wl[j], wl[1], b1)) return false;
        for (int b = 0; b < NB; ++b) a[b] = b1[b] * ampB[b];
        const Bend bd[3] = {{wl[j], en[j], p[j], S, p[1]}, {wl[1], en[1], p[1], p[j], p[k]}, {wl[k], en[k], p[k], p[1], R}};
        if (!bendsOk(g, bd, 3, a)) return false;
        if (!transmitLeg(g, p[1], p[k], wl[1], wl[k], a)) return false;
        if (needClear && a[3] < 0.7f) return false;
        len = norm(p[j] - S) + norm(p[1] - p[j]) + norm(p[k] - p[1]) + norm(R - p[k]);
    }
    Vec3 pts[5];
    int n = 0;
    pts[n++] = S;
    for (int i = 0; i < sq.n; ++i) pts[n++] = p[i];
    pts[n++] = R;
    out.first = p[j];
    out.last = p[k];
    out.length = len;
    out.inner = len - norm(p[j] - S) - norm(R - p[k]);
    setAmp(pts, n, a);
    return true;
}

// Directivity gain of every octave band (the pattern may change with frequency: `pattern` at 125 Hz, `patternHf` at 8 kHz).
// The angle is evaluated once.
inline void dirGainBands(const Transducer& t, Vec3 d, double* g /* NB values */) {
    const double a = t.azimuthDeg * kPi / 180.0;
    double c;
    if (t.elevationDeg == 0.0) {
        c = d.x * std::cos(a) + d.y * std::sin(a);
    } else {
        const double e = t.elevationDeg * kPi / 180.0, ce = std::cos(e);
        c = d.x * ce * std::cos(a) + d.y * ce * std::sin(a) + d.z * std::sin(e);
    }
    const double gdb = t.gainDb == 0.0 ? 1.0 : std::pow(10.0, t.gainDb / 20.0);
    for (int b = 0; b < NB; ++b) {
        const double p = t.patternHf < 0 ? t.pattern : t.pattern + (t.patternHf - t.pattern) * ((double)b / (double)(NB - 1));
        g[b] = ((1 - p) + p * c) * gdb;
    }
}

inline double dirGain(const Transducer& t, Vec3 d) {
    const double a = t.azimuthDeg * kPi / 180.0;
    double c;
    if (t.elevationDeg == 0.0) {
        c = d.x * std::cos(a) + d.y * std::sin(a);  // horizontal axis (the common case, cheap)
    } else {
        const double e = t.elevationDeg * kPi / 180.0, ce = std::cos(e);
        c = d.x * ce * std::cos(a) + d.y * ce * std::sin(a) + d.z * std::sin(e);
    }
    return ((1 - t.pattern) + t.pattern * c) * std::pow(10.0, t.gainDb / 20.0);
}

// Does a wall block or attenuate sound (false for pure openings with tau >= 0.999 in all bands)?
inline bool wallBlocks(const Geo& g, const Wall& w) {
    const Material& m = g.s.mats[(size_t)w.mat];
    for (int b = 0; b < NB; ++b) if (m.tau[(size_t)b] < 0.999) return true;
    return false;
}

// Rule C & Rule D geometric tolerances (metres).
constexpr double kWallLineTol = 0.02;         // 2 cm tolerance around blocking surface planes
constexpr double kWallSurfaceOffset = 0.002;  // 2 mm clearance inward from surface plane
constexpr double kCapsuleMargin = 0.01;       // 1 cm minimum inward margin for spatial capsules

// Fixed-size RT-safe surface-side history (no heap allocation in process()).
struct SurfaceSideHist {
    static constexpr int kMaxHistWalls = 128;
    int8_t wall[kMaxHistWalls]{};
    int8_t plate[kMaxPlates]{};
    int8_t floor = 0;
    int8_t ceil = 0;
    uint64_t worldSerial = 0;
    void reset(uint64_t serial = 0) {
        std::fill_n(wall, kMaxHistWalls, int8_t(0));
        std::fill_n(plate, kMaxPlates, int8_t(0));
        floor = 0;
        ceil = 0;
        worldSerial = serial;
    }
};

// True when `p` lies inside an architectural enclosure (closed room, courtyard below H, corridor with open doorway,
// faceted hall, multi-chamber shell). False for free-standing baffle walls and outside poses.
inline bool isInsideEnclosure(const Geo& g, Vec3 p) {
    if (g.W <= 0) return false;
    if (g.s.floorMat >= 0 || floorMatAt(g.s, p.x, p.y) >= 0) {
        if (p.z < -1e-6) return false;
    }
    if (p.z > g.H + 1e-6) return false;
    const double zq = (p.z >= 0.0 && p.z <= g.H && g.H > 2e-4)
        ? std::min(std::max(p.z, 1e-4), g.H - 1e-4)
        : p.z;
    constexpr int K = 16;
    int hitAny = 0, bothHitPairs = 0, oddCount = 0, evenNonZero = 0;
    for (int k = 0; k < K; ++k) {
        const double th = (double)k * (kPi / (double)K) + 0.001;
        const double dx = std::cos(th), dy = std::sin(th);
        int cPos = 0, cNeg = 0;
        for (int i = 0; i < g.W; ++i) {
            const Wall& w = g.s.walls[(size_t)i];
            if (!wallBlocks(g, w)) continue;
            if (zq < std::max(0.0, w.z0) - 1e-9 || zq > std::min(w.z1, g.H) + 1e-9) continue;
            const double ex = w.bx - w.ax, ey = w.by - w.ay;
            const double den = cross2(dx, dy, ex, ey);
            if (std::fabs(den) < 1e-12) continue;
            const double t = cross2(w.ax - p.x, w.ay - p.y, ex, ey) / den;
            const double u = cross2(w.ax - p.x, w.ay - p.y, dx, dy) / den;
            if (u < 0.0 || u > 1.0) continue;
            if (t > 1e-9) ++cPos;
            else if (t < -1e-9) ++cNeg;
        }
        hitAny += (cPos > 0) + (cNeg > 0);
        if (cPos > 0 && cNeg > 0) ++bothHitPairs;
        oddCount += (cPos & 1) + (cNeg & 1);
        if (cPos > 0 && !(cPos & 1)) ++evenNonZero;
        if (cNeg > 0 && !(cNeg & 1)) ++evenNonZero;
    }
    if (bothHitPairs == K) return true;
    return hitAny >= 17 && bothHitPairs >= 1 && oddCount > evenNonZero;
}

// Rule D: Keep poses within kWallLineTol (2 cm) of blocking surfaces on the side they came from (or nearest
// enclosure interior when history is empty, i.e. first pose / after jump). Poses farther than 2 cm from every
// blocking surface are returned bit-for-bit untouched.
inline Vec3 resolveWallLinePose(const Geo& g, Vec3 pos, SurfaceSideHist& hist, uint64_t worldSerial = 0) {
    if (hist.worldSerial != worldSerial) hist.reset(worldSerial);
    const int NW = std::min(g.W, SurfaceSideHist::kMaxHistWalls);
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < g.W; ++i) {
            const Wall& w = g.s.walls[(size_t)i];
            if (!wallBlocks(g, w)) continue;
            const double lo = std::max(0.0, w.z0), hi = std::min(w.z1, g.H);
            if (hi - lo <= 1e-6) continue;
            if (pos.z < lo - kWallLineTol || pos.z > hi + kWallLineTol) continue;
            const double ex = w.bx - w.ax, ey = w.by - w.ay;
            const double len = std::hypot(ex, ey);
            if (len < 1e-9) continue;
            const double ux = ex / len, uy = ey / len;
            const double nx = -uy, ny = ux;
            const double dx = pos.x - w.ax, dy = pos.y - w.ay;
            const double along = dx * ux + dy * uy;
            const double sd = dx * nx + dy * ny;
            const bool freeA = g.jBegin(i, 0) == g.jEnd(i, 0);
            const bool freeB = g.jBegin(i, 1) == g.jEnd(i, 1);
            double beyond = 1e9;
            if (along >= 0.0 && along <= len) beyond = 0.0;
            else if (along < 0.0 && freeA) beyond = -along;
            else if (along > len && freeB) beyond = along - len;
            if (beyond == 0.0 && std::fabs(sd) > kWallLineTol) {
                if (i < NW) hist.wall[i] = (sd > 0.0) ? int8_t(1) : int8_t(-1);
                continue;
            }
            if (beyond >= kWallLineTol || std::fabs(sd) > kWallLineTol) continue;
            if (pass > 0 && beyond > 0.0) continue;
            int8_t side = (i < NW) ? hist.wall[i] : int8_t(0);
            if (side == 0) {
                const double inset = std::min(0.1, 0.25 * len);
                const double alongProbe = std::min(len - inset, std::max(inset, along));
                const double px = w.ax + alongProbe * ux, py = w.ay + alongProbe * uy;
                const double zp = (lo + 1e-3 < hi - 1e-3) ? std::min(std::max(pos.z, lo + 1e-3), hi - 1e-3) : 0.5 * (lo + hi);
                for (double step : {0.05, 0.15}) {
                    const bool inPos = isInsideEnclosure(g, {px + nx * step, py + ny * step, zp});
                    const bool inNeg = isInsideEnclosure(g, {px - nx * step, py - ny * step, zp});
                    if (inPos != inNeg) { side = inPos ? int8_t(1) : int8_t(-1); break; }
                }
                if (side == 0) side = (sd >= 0.0) ? int8_t(1) : int8_t(-1);
                if (i < NW) hist.wall[i] = side;
            }
            const double sdSide = sd * (double)side;
            if (sdSide < kWallSurfaceOffset) {
                const double taper = 1.0 - beyond / kWallLineTol;
                const double push = (kWallSurfaceOffset - sdSide) * taper;
                pos.x += nx * ((double)side * push);
                pos.y += ny * ((double)side * push);
            }
        }
    }
    if (g.s.floorMat >= 0 || floorMatAt(g.s, pos.x, pos.y) >= 0) {
        if (std::fabs(pos.z) > kWallLineTol) {
            hist.floor = (pos.z > 0.0) ? int8_t(1) : int8_t(-1);
        } else {
            if (hist.floor == 0) hist.floor = int8_t(1);
            const double zSide = pos.z * (double)hist.floor;
            if (zSide < kWallSurfaceOffset) pos.z = (double)hist.floor * kWallSurfaceOffset;
        }
    }
    if (g.hasCeil && g.matAt(g.ceilId(), pos.x, pos.y) != nullptr) {
        const double dz = pos.z - g.H;
        if (std::fabs(dz) > kWallLineTol) {
            hist.ceil = (dz > 0.0) ? int8_t(1) : int8_t(-1);
        } else {
            if (hist.ceil == 0) hist.ceil = int8_t(-1);
            const double zSide = dz * (double)hist.ceil;
            if (zSide < kWallSurfaceOffset) pos.z = g.H + (double)hist.ceil * kWallSurfaceOffset;
        }
    }
    const int NP = std::min((int)g.plates.size(), kMaxPlates);
    for (int k = 0; k < NP; ++k) {
        const Zone& pl = *g.plates[(size_t)k];
        const Material& pm = g.s.mats[(size_t)pl.mat];
        bool blocks = false;
        for (int b = 0; b < NB; ++b) if (pm.tau[(size_t)b] < 0.999) { blocks = true; break; }
        if (!blocks || !pointInZone(pl, pos.x, pos.y)) continue;
        const double dz = pos.z - pl.z;
        if (std::fabs(dz) > kWallLineTol) {
            hist.plate[k] = (dz > 0.0) ? int8_t(1) : int8_t(-1);
        } else {
            if (hist.plate[k] == 0) {
                const bool inAbove = isInsideEnclosure(g, {pos.x, pos.y, pl.z + 0.05});
                const bool inBelow = isInsideEnclosure(g, {pos.x, pos.y, pl.z - 0.05});
                if (inAbove != inBelow) hist.plate[k] = inAbove ? int8_t(1) : int8_t(-1);
                else hist.plate[k] = (dz >= 0.0) ? int8_t(1) : int8_t(-1);
            }
            const double zSide = dz * (double)hist.plate[k];
            if (zSide < kWallSurfaceOffset) pos.z = pl.z + (double)hist.plate[k] * kWallSurfaceOffset;
        }
    }
    return pos;
}

// Rule C: When `centerPos` is inside an enclosure, continuously pull any spatially offset receiver capsule
// (excluding capsules coincident with `centerPos`) that crosses or enters within kCapsuleMargin (1 cm) of a
// blocking wall/floor/ceiling/plate back to the centre's side. Outside listeners remain untouched.
inline void containCapsulesInRoom(const Geo& g, Vec3 centerPos, Transducer* rcv, int count) {
    if (count <= 0 || g.W <= 0) return;
    if (!isInsideEnclosure(g, centerPos)) return;
    const bool hasFloor = (g.s.floorMat >= 0 || floorMatAt(g.s, centerPos.x, centerPos.y) >= 0);
    const bool hasCeilMat = (g.hasCeil && g.matAt(g.ceilId(), centerPos.x, centerPos.y) != nullptr);
    const int NP = (int)g.plates.size();

    for (int r = 0; r < count; ++r) {
        Vec3& cap = rcv[r].pos;
        if (norm(cap - centerPos) < 1e-6) continue;
        for (int pass = 0; pass < 2; ++pass) {
            const double rx = cap.x - centerPos.x, ry = cap.y - centerPos.y;
            const double zMin = std::min(centerPos.z, cap.z), zMax = std::max(centerPos.z, cap.z);
            for (int i = 0; i < g.W; ++i) {
                const Wall& w = g.s.walls[(size_t)i];
                if (!wallBlocks(g, w)) continue;
                const double lo = std::max(0.0, w.z0), hi = std::min(w.z1, g.H);
                if (hi - lo <= 1e-6) continue;
                if (zMax < lo - kCapsuleMargin || zMin > hi + kCapsuleMargin) continue;
                const double ex = w.bx - w.ax, ey = w.by - w.ay;
                const double len = std::hypot(ex, ey);
                if (len < 1e-9) continue;
                const double ux = ex / len, uy = ey / len;
                const double nx = -uy, ny = ux;

                const double sdC = (centerPos.x - w.ax) * nx + (centerPos.y - w.ay) * ny;
                const double sdP = (cap.x - w.ax) * nx + (cap.y - w.ay) * ny;
                const double alongC = (centerPos.x - w.ax) * ux + (centerPos.y - w.ay) * uy;
                const double alongP = (cap.x - w.ax) * ux + (cap.y - w.ay) * uy;

                double sideC = 0.0;
                if (std::fabs(sdC) > 1e-9) sideC = (sdC > 0.0) ? 1.0 : -1.0;
                else {
                    const double inset = std::min(0.1, 0.25 * len);
                    const double ap = std::min(len - inset, std::max(inset, alongC));
                    const double px = w.ax + ap * ux, py = w.ay + ap * uy;
                    const double zp = (lo + 1e-3 < hi - 1e-3) ? std::min(std::max(centerPos.z, lo + 1e-3), hi - 1e-3) : 0.5 * (lo + hi);
                    const bool inPos = isInsideEnclosure(g, {px + nx * 0.05, py + ny * 0.05, zp});
                    const bool inNeg = isInsideEnclosure(g, {px - nx * 0.05, py - ny * 0.05, zp});
                    sideC = (inPos && !inNeg) ? 1.0 : (inNeg && !inPos) ? -1.0 : 1.0;
                }

                const double sdPSide = sdP * sideC;
                if (sdPSide >= kCapsuleMargin) continue;

                bool crossesClosed = false;
                const bool freeA = g.jBegin(i, 0) == g.jEnd(i, 0);
                const bool freeB = g.jBegin(i, 1) == g.jEnd(i, 1);
                if (!freeA || !freeB) {
                    const double den = cross2(rx, ry, ex, ey);
                    if (std::fabs(den) > 1e-12) {
                        const double tCross = cross2(w.ax - centerPos.x, w.ay - centerPos.y, ex, ey) / den;
                        const double uCross = cross2(w.ax - centerPos.x, w.ay - centerPos.y, rx, ry) / den;
                        if (tCross >= -1e-6 && tCross <= 1.0 + 1e-6 && uCross >= -1e-6 && uCross <= 1.0 + 1e-6) {
                            if ((alongP < 0.0 && !freeA) || (alongP > len && !freeB)) crossesClosed = true;
                        }
                    }
                }

                double beyond = 1e9;
                if (alongP >= 0.0 && alongP <= len) beyond = 0.0;
                else if (crossesClosed) beyond = 0.0;
                else if (alongP < 0.0 && freeA) beyond = -alongP;
                else if (alongP > len && freeB) beyond = alongP - len;

                if (beyond >= kWallLineTol) continue;
                if (pass > 0 && beyond > 0.0) continue;

                const double taper = (beyond <= 0.0) ? 1.0 : (1.0 - beyond / kWallLineTol);
                const double push = (kCapsuleMargin - sdPSide) * taper;
                cap.x += nx * (sideC * push);
                cap.y += ny * (sideC * push);
            }
        }
        if (hasFloor && centerPos.z >= -1e-6 && cap.z < kCapsuleMargin) cap.z = kCapsuleMargin;
        if (hasCeilMat && centerPos.z <= g.H + 1e-6 && cap.z > g.H - kCapsuleMargin) cap.z = g.H - kCapsuleMargin;
        for (int k = 0; k < NP; ++k) {
            const Zone& pl = *g.plates[(size_t)k];
            const Material& pm = g.s.mats[(size_t)pl.mat];
            bool blocks = false;
            for (int b = 0; b < NB; ++b) if (pm.tau[(size_t)b] < 0.999) { blocks = true; break; }
            if (!blocks) continue;
            const double dzC = centerPos.z - pl.z;
            if (std::fabs(dzC) <= 1e-9) continue;
            const double sideC = (dzC > 0.0) ? 1.0 : -1.0;
            const double dzP = (cap.z - pl.z) * sideC;
            if (dzP < kCapsuleMargin) {
                const double dz = cap.z - centerPos.z;
                const double tCross = std::fabs(dz) > 1e-12 ? (pl.z - centerPos.z) / dz : -1.0;
                const double ix = centerPos.x + tCross * (cap.x - centerPos.x);
                const double iy = centerPos.y + tCross * (cap.y - centerPos.y);
                if (pointInZone(pl, cap.x, cap.y) || (tCross >= 0.0 && tCross <= 1.0 && pointInZone(pl, ix, iy))) {
                    cap.z = pl.z + sideC * kCapsuleMargin;
                }
            }
        }
    }
}

}  // namespace verb
