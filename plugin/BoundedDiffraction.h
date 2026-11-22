#pragma once
#include "geometry.hpp"
#include <functional>
// Read-only UI counterpart of geometry.hpp::diffractedPath. Identical geometry
// helpers and search order; deadline checks in every search loop. DSP unchanged.
namespace verbweb {
inline bool boundedDiffractedPath(const verb::Geo& g, verb::Vec3 S, verb::Vec3 R, double c, verb::DiffPath& out, bool needClear, const std::function<bool()>& stop, verb::DiffractionScratch* scratch = nullptr) {
    using namespace verb;
    if(stop())return false;
    if(g.separatedShell(S,R,needClear))return false;
    if(!scratch){DiffractionScratch local;local.prepare((size_t)g.W);return boundedDiffractedPath(g,S,R,c,out,needClear,stop,&local);}
    auto& work=*scratch;
    using Node=DiffractionScratch::Node;
    auto& nodes=work.nodes;nodes.clear();
    if(nodes.capacity()<(size_t)g.W*2) {++work.capacityMisses;return false;}
    const double dist3 = norm(R - S);
    const double rx = R.x - S.x, ry = R.y - S.y, len2 = std::max(1e-12, rx * rx + ry * ry);
    for (int i = 0; i < g.W; ++i) {
            if(stop())return false;
        const Wall& w = g.s.walls[i];
        for (int e = 0; e < 2; ++e) {
            const double ex = e ? w.bx : w.ax, ey = e ? w.by : w.ay;
            double t = ((ex - S.x) * rx + (ey - S.y) * ry) / len2;
            t = std::min(1.0, std::max(0.0, t));
            const double z = S.z + t * (R.z - S.z);
            if (z < 0 || z > g.H) continue;
            nodes.push_back({Vec3{ex, ey, z}, i, e});
        }
    }
    const int N = (int)nodes.size();
    auto setAmp = [&](const Vec3* pts, int n, const float* legAmp) {  // pts[0]=S ... pts[n-1]=R: knife-edge loss at every bend
        for (int b = 0; b < NB; ++b) {
            double db = 0;
            for (int k = 1; k + 1 < n; ++k) {
                const double delta = norm(pts[k] - pts[k - 1]) + norm(pts[k + 1] - pts[k]) - norm(pts[k + 1] - pts[k - 1]);
                db += knifeEdgeDb(delta, kBandHz[b], c, true);
            }
            out.amp[b] = legAmp[b] * (float)std::pow(10.0, -db / 20.0);
        }
    };
    // 1. one wall end (the smallest detours first)
    {
        struct Cand { double delta; int idx; };
        Cand best[4];
        int nb = 0;
        for (int j = 0; j < N; ++j) {
            if(stop())return false;
            const double delta = norm(nodes[j].p - S) + norm(R - nodes[j].p) - dist3;
            int pos = nb;
            while (pos > 0 && best[pos - 1].delta > delta) --pos;
            if (pos >= 4) continue;
            { const Bend bd{nodes[j].wall, nodes[j].end, nodes[j].p, S, R}; if (!bendsOk(g, &bd, 1, nullptr)) continue; }  // a sealed corner is no way round (and must not take a place among the best four)
            for (int k = std::min(nb, 3); k > pos; --k) best[k] = best[k - 1];
            best[pos] = {delta, j};
            nb = std::min(nb + 1, 4);
        }
        for (int k = 0; k < nb; ++k) {
            if(stop())return false;
            const Node& nd = nodes[best[k].idx];
            float amp[NB];
            for (int b = 0; b < NB; ++b) amp[b] = 1.f;
            if (!transmitLeg(g, S, nd.p, nd.wall, -1, amp) || !transmitLeg(g, nd.p, R, nd.wall, -1, amp)) continue;
            { const Bend bd{nd.wall, nd.end, nd.p, S, R}; if (!bendsOk(g, &bd, 1, amp)) continue; }
            if (needClear && amp[3] < 0.7f) continue;  // both legs must be in the open: the sound goes round, not through
            litEdgeAttenuation(g, S, nd.p, c, amp);  // other wall ends close beside the two legs still trim them,
            litEdgeAttenuation(g, nd.p, R, c, amp);  // so the level is continuous where a second corner takes over
            const Vec3 pts[3] = {S, nd.p, R};
            out.first = out.last = nd.p;
            out.inner = 0;
            out.length = norm(nd.p - S) + norm(R - nd.p);
            setAmp(pts, 3, amp);
            return true;
        }
    }
    if (N > 160) return false;  // a scene this busy: keep the cost bounded
    // needClear: a path whose mid-band amplitude is already below 0.7 can only get quieter (every factor is <= 1), so it
    // would be refused at the end anyway; dropping it early changes nothing but the cost (E3b).
    const bool prune = needClear && g.ampNonIncreasing;
    // 2. round several ends: which ends does the source see, which does the receiver see
    auto& A=work.a;auto& B=work.b;A.clear();B.clear();
    auto& ampA=work.ampA;auto& ampB=work.ampB;
    if(A.capacity()<(size_t)N || B.capacity()<(size_t)N || ampA.size()<(size_t)N*NB || ampB.size()<(size_t)N*NB) {++work.capacityMisses;return false;}
    for (int j = 0; j < N; ++j) {
            if(stop())return false;
        float a[NB], bb[NB];
        for (int b = 0; b < NB; ++b) a[b] = bb[b] = 1.f;
        if (transmitLeg(g, S, nodes[j].p, nodes[j].wall, -1, a) && !(prune && a[3] < 0.7f)) { A.push_back(j); std::copy(a, a + NB, ampA.begin() + (long)j * NB); }
        if (transmitLeg(g, nodes[j].p, R, nodes[j].wall, -1, bb) && !(prune && bb[3] < 0.7f)) { B.push_back(j); std::copy(bb, bb + NB, ampB.begin() + (long)j * NB); }
    }
    auto close = [](Vec3 a, Vec3 b) { return norm(a - b) < 1e-6; };
    double bestLen = 1e30;
    int bj = -1, bm = -1, bk = -1;
    float bestAmp[NB] = {};
    // ---- memos shared by the two- and three-end searches (E3b) ----
    // Each of these depends on the geometry only, so it is worked out once per call (lazily) instead of once per candidate
    // path. They only reject a candidate for a reason the original order of tests would also have found, or (needClear
    // only) because the amplitude is already below the 0.7 the path must keep: every factor is <= 1, so the path can only
    // get quieter, and a 0.1 % slack covers float rounding. Candidates that survive run exactly the original arithmetic.
    //   leg   : the straight leg between two nodes is clear (opaque walls) and how much of band 3 it lets through;
    //   bend0 : the corner test at the first node of a path (legs S -> u -> v), permissive part and its band-3 factor;
    //   bend2 : the same at the last node (legs u -> v -> R);
    //   ang   : atan2 of the direction between two nodes (the corner tests need them, and they are the costly part).
    auto& legState = work.legState;
    auto& legAmp3 = work.legAmp3;
    auto& b0State = work.b0State;
    auto& b0Amp3 = work.b0Amp3;
    auto& b2State = work.b2State;
    auto& b2Amp3 = work.b2Amp3;
    auto& angMemo = work.ang;
    const size_t NN = (size_t)N * (size_t)N;
    if (legState.size() < NN || legAmp3.size() < NN || b0State.size() < NN || b0Amp3.size() < NN || b2State.size() < NN || b2Amp3.size() < NN || angMemo.size() < NN || work.kList.size() < NN) { ++work.capacityMisses; return false; }
    std::fill(legState.begin(), legState.begin() + (long)NN, (unsigned char)0);
    std::fill(b0State.begin(), b0State.begin() + (long)NN, (unsigned char)0);
    std::fill(b2State.begin(), b2State.begin() + (long)NN, (unsigned char)0);
    std::fill(angMemo.begin(), angMemo.begin() + (long)NN, kNoAngle);
    double angS[160], angR[160];
    bool sealed[160];
    for (int u = 0; u < N; ++u) {
        angS[u] = angR[u] = kNoAngle;
        sealed[u] = g.jEnd(nodes[u].wall, nodes[u].end) - g.jBegin(nodes[u].wall, nodes[u].end) >= 2;  // a free end: the corner test returns at once
    }
    auto angNN = [&](int u, int v) {  // direction from node u to node v, as the corner test at u computes it
        if (!sealed[u]) return kNoAngle;
        double& a = angMemo[(size_t)u * (size_t)N + (size_t)v];
        if (std::isnan(a)) a = std::atan2(nodes[v].p.y - nodes[u].p.y, nodes[v].p.x - nodes[u].p.x);
        return a;
    };
    auto angToS = [&](int u) {
        if (!sealed[u]) return kNoAngle;
        if (std::isnan(angS[u])) angS[u] = std::atan2(S.y - nodes[u].p.y, S.x - nodes[u].p.x);
        return angS[u];
    };
    auto angToR = [&](int u) {
        if (!sealed[u]) return kNoAngle;
        if (std::isnan(angR[u])) angR[u] = std::atan2(R.y - nodes[u].p.y, R.x - nodes[u].p.x);
        return angR[u];
    };
    // Largest band-3 factor this bend can have: the permissive reading (legs on a wall count for neither side) or any of
    // the four readings of bendsOk's face search (each leg shifted a hair to one side). bendsOk's result for a whole path
    // is a product of one such reading per bend, so it is never larger than the product of these caps.
    auto bendCap3 = [&](const Bend& bd, double c0, double c1, float permissive3) {
        float best3 = permissive3;
        for (int si = -1; si <= 1; si += 2)
            for (int so = -1; so <= 1; so += 2) {
                float t[NB];
                for (int b = 0; b < NB; ++b) t[b] = 1.f;
                if (cornerCore(g, bd, si, so, t, nullptr, c0, c1)) best3 = std::max(best3, t[3]);
            }
        return best3;
    };
    // Upper bound of the band-3 factor bendsOk gives a whole path: the product of the permissive readings when no leg lies on
    // a wall, otherwise the best consistent choice of the faces the along-wall legs keep to (every combination, as bendsOk
    // tries them). The final amplitude is never larger than this (0 where bendsOk would refuse the path).
    auto pathCap3 = [&](const Bend* bd, int n, const double* r0, const double* r1) {
        double perm = 1.0;
        bool tieAny = false;
        float f[3][2][2];
        for (int k = 0; k < n; ++k) {
            float t[NB];
            for (int b = 0; b < NB; ++b) t[b] = 1.f;
            bool tie = false;
            if (!cornerCore(g, bd[k], 0, 0, t, &tie, r0[k], r1[k])) perm = 0.0; else perm *= t[3];
            tieAny = tieAny || tie;
            for (int si = 0; si < 2; ++si)
                for (int so = 0; so < 2; ++so) {
                    float u[NB];
                    for (int b = 0; b < NB; ++b) u[b] = 1.f;
                    f[k][si][so] = cornerCore(g, bd[k], si ? 1 : -1, so ? 1 : -1, u, nullptr, r0[k], r1[k]) ? u[3] : 0.f;
                }
        }
        double best = 0.0;
        for (int mask = 0; mask < (1 << (n + 1)); ++mask) {
            double pr = 1.0;
            for (int k = 0; k < n && pr > 0.0; ++k) pr *= f[k][(mask >> k & 1) ? 0 : 1][(mask >> (k + 1) & 1) ? 1 : 0];
            best = std::max(best, pr);
        }
        return tieAny ? best : perm;  // bendsOk uses the permissive reading only when no leg lies on a wall
    };
    auto legClear = [&](int u, int v) {
        const size_t id = (size_t)u * (size_t)N + (size_t)v;
        if (!legState[id]) {
            float t[NB];
            for (int b = 0; b < NB; ++b) t[b] = 1.f;
            const bool ok = transmitLeg(g, nodes[u].p, nodes[v].p, nodes[u].wall, nodes[v].wall, t);
            legState[id] = ok ? 1 : 2;
            legAmp3[id] = ok ? t[3] : 0.f;
        }
        return legState[id] == 1;
    };
    auto bend0 = [&](int u, int v) {  // corner at u on the way S -> u -> v: false = the corner test refuses it outright
        const size_t id = (size_t)u * (size_t)N + (size_t)v;
        if (!b0State[id]) {
            float pf[NB];
            for (int b = 0; b < NB; ++b) pf[b] = 1.f;
            bool tie = false;
            const Bend bd{nodes[u].wall, nodes[u].end, nodes[u].p, S, nodes[v].p};
            const double c0 = angToS(u), c1 = angNN(u, v);
            const bool ok = cornerCore(g, bd, 0, 0, pf, &tie, c0, c1);
            b0State[id] = ok ? 1 : 2;
            b0Amp3[id] = ok ? bendCap3(bd, c0, c1, pf[3]) : 0.f;
        }
        return b0State[id] == 1;
    };
    auto bend2 = [&](int u, int v) {  // corner at v on the way u -> v -> R
        const size_t id = (size_t)u * (size_t)N + (size_t)v;
        if (!b2State[id]) {
            float pf[NB];
            for (int b = 0; b < NB; ++b) pf[b] = 1.f;
            bool tie = false;
            const Bend bd{nodes[v].wall, nodes[v].end, nodes[v].p, nodes[u].p, R};
            const double c0 = angNN(v, u), c1 = angToR(v);
            const bool ok = cornerCore(g, bd, 0, 0, pf, &tie, c0, c1);
            b2State[id] = ok ? 1 : 2;
            b2Amp3[id] = ok ? bendCap3(bd, c0, c1, pf[3]) : 0.f;
        }
        return b2State[id] == 1;
    };
    constexpr double kLoud = (double)0.7f, kSlack = 1.001;
    // the ends that can follow a middle end m (in B's order): m -> k is a clear leg, loud enough, and the corner test at k
    // does not refuse it; none of this depends on the first end, so it is worked out once per m
    auto& kListBuf = work.kList;
    int kCnt[160];
    for (int u = 0; u < N; ++u) kCnt[u] = -1;
    auto kListOf = [&](int m) {
        int* l = kListBuf.data() + (size_t)m * (size_t)N;
        if (kCnt[m] < 0) {
            int c = 0;
            for (int k : B) {
                if (k == m || close(nodes[m].p, nodes[k].p)) continue;
                if (!legClear(m, k)) continue;
                const size_t mk = (size_t)m * (size_t)N + (size_t)k;
                if (prune && (double)ampB[(size_t)k * NB + 3] * legAmp3[mk] * kSlack < kLoud) continue;
                if (!bend2(m, k)) continue;
                if (prune && (double)ampB[(size_t)k * NB + 3] * legAmp3[mk] * b2Amp3[mk] * kSlack < kLoud) continue;
                l[c++] = k;
            }
            kCnt[m] = c;
        }
        return (const int*)l;
    };
    for (int j : A)  // two ends
        for (int k : B) {
            if(stop())return false;
            if (j == k || close(nodes[j].p, nodes[k].p)) continue;
            const double len = norm(nodes[j].p - S) + norm(nodes[k].p - nodes[j].p) + norm(R - nodes[k].p);
            if (len >= bestLen) continue;
            if (!legClear(j, k)) continue;
            if (prune && (double)ampA[(size_t)j * NB + 3] * ampB[(size_t)k * NB + 3] * legAmp3[(size_t)j * (size_t)N + (size_t)k] * kSlack < kLoud) continue;
            if (!bend0(j, k) || !bend2(j, k)) continue;  // bendsOk would refuse it at its first test
            float a[NB];
            for (int b = 0; b < NB; ++b) a[b] = ampA[(size_t)j * NB + b] * ampB[(size_t)k * NB + b];
            if (prune && a[3] < 0.7f) continue;
            if (prune && (double)a[3] * legAmp3[(size_t)j * (size_t)N + (size_t)k] * b0Amp3[(size_t)j * (size_t)N + (size_t)k] * b2Amp3[(size_t)j * (size_t)N + (size_t)k] * kSlack < kLoud) continue;
            {
                const Bend bd[2] = {{nodes[j].wall, nodes[j].end, nodes[j].p, S, nodes[k].p}, {nodes[k].wall, nodes[k].end, nodes[k].p, nodes[j].p, R}};
                const double r0[2] = {angToS(j), angNN(k, j)}, r1[2] = {angNN(j, k), angToR(k)};
                if (!bendsOk(g, bd, 2, a, r0, r1)) continue;
            }
            if (!transmitLeg(g, nodes[j].p, nodes[k].p, nodes[j].wall, nodes[k].wall, a)) continue;
            if (needClear && a[3] < 0.7f) continue;
            bestLen = len; bj = j; bm = -1; bk = k;
            std::copy(a, a + NB, bestAmp);
        }
    if (bj < 0)  // three ends, only if two did not work
        for (int j : A)
            for (int m = 0; m < N; ++m) {
            if(stop())return false;
                if (m == j || close(nodes[j].p, nodes[m].p)) continue;
                const double l1 = norm(nodes[j].p - S) + norm(nodes[m].p - nodes[j].p);
                if (l1 >= bestLen) continue;
                if (!legClear(j, m)) continue;
                if (prune && (double)ampA[(size_t)j * NB + 3] * legAmp3[(size_t)j * (size_t)N + (size_t)m] * kSlack < kLoud) continue;
                float a1[NB];
                for (int b = 0; b < NB; ++b) a1[b] = ampA[(size_t)j * NB + b];
                if (!bend0(j, m)) continue;  // a sealed corner is no way round
                if (prune && (double)ampA[(size_t)j * NB + 3] * legAmp3[(size_t)j * (size_t)N + (size_t)m] * b0Amp3[(size_t)j * (size_t)N + (size_t)m] * kSlack < kLoud) continue;
                const int* kl = kListOf(m);  // the ends k that can follow m whatever j is
                const int kc = kCnt[m];
                if (kc == 0) continue;
                if (!transmitLeg(g, nodes[j].p, nodes[m].p, nodes[j].wall, nodes[m].wall, a1)) continue;
                if (prune && a1[3] < 0.7f) continue;
                const double q0 = (double)a1[3] * b0Amp3[(size_t)j * (size_t)N + (size_t)m];
                if (prune && q0 * kSlack < kLoud) continue;
                for (int ci = 0; ci < kc; ++ci) {
                    const int k = kl[ci];
            if(stop())return false;
                    if (close(nodes[j].p, nodes[k].p)) continue;
                    const double len = l1 + norm(nodes[k].p - nodes[m].p) + norm(R - nodes[k].p);
                    if (len >= bestLen) continue;
                    float a[NB];
                    for (int b = 0; b < NB; ++b) a[b] = a1[b] * ampB[(size_t)k * NB + b];
                    if (prune && a[3] < 0.7f) continue;
                    if (prune) {
                        const size_t mk = (size_t)m * (size_t)N + (size_t)k;
                        const double q1 = (double)a[3] * legAmp3[mk] * b2Amp3[mk] * b0Amp3[(size_t)j * (size_t)N + (size_t)m];
                        if (q1 * kSlack < kLoud) continue;
                        float p1[NB];
                        for (int b = 0; b < NB; ++b) p1[b] = 1.f;
                        bool tie1 = false;
                        const Bend bmid{nodes[m].wall, nodes[m].end, nodes[m].p, nodes[j].p, nodes[k].p};
                        const double c0 = angNN(m, j), c1 = angNN(m, k);
                        if (!cornerCore(g, bmid, 0, 0, p1, &tie1, c0, c1)) continue;
                        if (q1 * bendCap3(bmid, c0, c1, p1[3]) * kSlack < kLoud) continue;
                        {   // the same bound with the face choices of the legs tied together, as in bendsOk's search
                            const Bend bd3[3] = {{nodes[j].wall, nodes[j].end, nodes[j].p, S, nodes[m].p}, bmid, {nodes[k].wall, nodes[k].end, nodes[k].p, nodes[m].p, R}};
                            const double r0[3] = {angToS(j), angNN(m, j), angNN(k, m)}, r1[3] = {angNN(j, m), angNN(m, k), angToR(k)};
                            if ((double)a[3] * legAmp3[mk] * pathCap3(bd3, 3, r0, r1) * kSlack < kLoud) continue;
                        }
                    }
                    {
                        const Bend bd[3] = {{nodes[j].wall, nodes[j].end, nodes[j].p, S, nodes[m].p}, {nodes[m].wall, nodes[m].end, nodes[m].p, nodes[j].p, nodes[k].p}, {nodes[k].wall, nodes[k].end, nodes[k].p, nodes[m].p, R}};
                        const double r0[3] = {angToS(j), angNN(m, j), angNN(k, m)}, r1[3] = {angNN(j, m), angNN(m, k), angToR(k)};
                        if (!bendsOk(g, bd, 3, a, r0, r1)) continue;
                    }
                    if (!transmitLeg(g, nodes[m].p, nodes[k].p, nodes[m].wall, nodes[k].wall, a)) continue;
                    if (needClear && a[3] < 0.7f) continue;
                    bestLen = len; bj = j; bm = m; bk = k;
                    std::copy(a, a + NB, bestAmp);
                }
            }
    if (bj < 0) return false;
    Vec3 pts[5];
    int n = 0;
    pts[n++] = S;
    pts[n++] = nodes[bj].p;
    if (bm >= 0) pts[n++] = nodes[bm].p;
    pts[n++] = nodes[bk].p;
    pts[n++] = R;
    out.first = nodes[bj].p;
    out.last = nodes[bk].p;
    out.length = bestLen;
    out.inner = bestLen - norm(nodes[bj].p - S) - norm(R - nodes[bk].p);
    setAmp(pts, n, bestAmp);
    return true;
}

}
