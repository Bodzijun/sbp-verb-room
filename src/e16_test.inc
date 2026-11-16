// E16: multi-source path bake (Part 1) and coupled listener+source path bake (Part 2).

namespace {

static std::vector<Transducer> e16ReceiversAt(const Scene& base, const std::array<double, 4>& lst) {
    Vec3 origin = base.hasListener ? base.listenerPos : Vec3{};
    if (!base.hasListener && !base.receivers.empty()) {
        for (const auto& r : base.receivers) origin = origin + r.pos;
        origin = origin * (1.0 / (double)base.receivers.size());
    }
    const double dyaw = lst[3] - base.listenerYaw;
    const double rad = dyaw * kPi / 180.0;
    const double cy = std::cos(rad), sy = std::sin(rad);
    auto out = base.receivers;
    for (auto& rc : out) {
        const Vec3 d = rc.pos - origin;
        rc.pos = {lst[0] + d.x * cy - d.y * sy, lst[1] + d.x * sy + d.y * cy, lst[2] + d.z};
        rc.azimuthDeg += dyaw;
    }
    return out;
}

static double e16MaxErrDb(const std::vector<double>& pred, const std::vector<double>& ref, int S, int R, size_t* badCount = nullptr) {
    double worst = 0.0;
    size_t bad = 0;
    for (int r = 0; r < R; ++r) {
        for (int b = 0; b < NB; ++b) {
            double peak = 0.0, sumPred = 0.0, sumRef = 0.0;
            for (int s = 0; s < S; ++s) {
                const size_t k = ((size_t)s * (size_t)R + (size_t)r) * NB + (size_t)b;
                peak = std::max(peak, ref[k]);
            }
            for (int s = 0; s < S; ++s) {
                const size_t k = ((size_t)s * (size_t)R + (size_t)r) * NB + (size_t)b;
                sumPred += pred[k];
                sumRef += ref[k];
                if (ref[k] >= peak * 1e-4 && peak > 0.0) {
                    const double err = std::abs(10.0 * std::log10(std::max(1e-30, pred[k]) / std::max(1e-30, ref[k])));
                    worst = std::max(worst, err);
                    if (err > 1.0) ++bad;
                }
            }
            if (sumRef > 0.0) {
                const double err = std::abs(10.0 * std::log10(std::max(1e-30, sumPred) / std::max(1e-30, sumRef)));
                worst = std::max(worst, err);
                if (err > 1.0) ++bad;
            }
        }
    }
    if (badCount) *badCount = bad;
    return worst;
}

struct E16BakeRun {
    double ms = 0.0;
    size_t nodesSum = 0;
    uint64_t hash = 0;
    double maxErrDb = 0.0;
    double maxAppliedErrDb = 0.0;
    double maxAudioRmsErrDb = 0.0;
};

static E16BakeRun runE16BakeAccuracy(
    const char* label,
    const Scene& baseScene,
    const std::vector<BakedPath>& paths,
    const std::vector<std::vector<double>>& tuples,
    bool shared,
    unsigned workers = 0,
    int rays = 12000,
    bool checkAudio = true
) {
    E16BakeRun res;
    EngineConfig cfg = e4aConfig(shared);
    cfg.tailRays = rays;
    cfg.adaptiveTail = true;
    cfg.rebuildWorkers = workers;

    Engine eng(baseScene, cfg);
    const int S = eng.numSources(), R = eng.numReceivers();
    eng.setBakedPaths(paths);
    check(eng.bakeStats().state == Engine::BakeState::NotBaked, (std::string(label) + ": no auto-bake after setBakedPaths").c_str());

    eng.requestPathBake(workers);
    std::vector<std::vector<float>> input((size_t)S, std::vector<float>(128, 0.01f));
    std::vector<std::vector<float>> output((size_t)R, std::vector<float>(128, 0.f));
    std::vector<const float*> ip((size_t)S);
    std::vector<float*> op((size_t)R);
    for (int s = 0; s < S; ++s) ip[(size_t)s] = input[(size_t)s].data();
    for (int r = 0; r < R; ++r) op[(size_t)r] = output[(size_t)r].data();

    auto step = [&]() {
        eng.process(ip.data(), op.data(), 128);
        eng.runWorkerStep();
    };
    bool monotonic = true;
    int prevPct = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
    while (eng.bakeStats().percent < 100 && std::chrono::steady_clock::now() < deadline) {
        step();
        const int pct = eng.bakeStats().percent;
        if (pct < prevPct) monotonic = false;
        prevPct = pct;
    }
    const auto st = eng.bakeStats();
    res.ms = st.milliseconds;
    res.hash = st.hash;
    check(st.percent == 100 && (st.state == Engine::BakeState::Baked || st.state == Engine::BakeState::Approximate),
          (std::string(label) + ": bake completes").c_str());
    check(monotonic, (std::string(label) + ": unified monotonic progress").c_str());

    const auto tables = eng.bakedTablesSnapshot();
    const auto scSnap = eng.sceneSnapshot();
    const Scene& traceSc = scSnap ? *scSnap : baseScene;
    for (const auto& t : tables) res.nodesSum += t->u.size();

    const int refRays = std::max(rays, 16000);
    std::mt19937 rng(12345);
    std::normal_distribution<float> nd(0.f, 0.05f);

    for (size_t ti = 0; ti < tuples.size(); ++ti) {
        const auto& phases = tuples[ti];
        // Target poses for all objects: listener first, then sources 0..S-1
        std::array<double, 4> lstPose{
            baseScene.hasListener ? baseScene.listenerPos.x : baseScene.receivers.front().pos.x,
            baseScene.hasListener ? baseScene.listenerPos.y : baseScene.receivers.front().pos.y,
            baseScene.hasListener ? baseScene.listenerPos.z : baseScene.receivers.front().pos.z,
            baseScene.listenerYaw
        };
        std::vector<std::array<double, 4>> srcPose((size_t)S);
        for (int s = 0; s < S; ++s) {
            srcPose[(size_t)s] = {baseScene.sources[(size_t)s].pos.x, baseScene.sources[(size_t)s].pos.y, baseScene.sources[(size_t)s].pos.z, baseScene.sources[(size_t)s].azimuthDeg};
        }
        // Gather phases per path
        size_t pi = 0;
        std::vector<double> pathPhase(paths.size(), 0.0);
        if (phases.size() == 1 && paths.size() > 1) {
            // Coupled single-parameter mode
            for (size_t k = 0; k < paths.size(); ++k) pathPhase[k] = phases[0];
        } else {
            for (size_t k = 0; k < paths.size() && pi < phases.size(); ++k) pathPhase[k] = phases[pi++];
        }
        for (size_t k = 0; k < paths.size(); ++k) {
            const auto v = paths[k].at(pathPhase[k]);
            if (paths[k].object < 0) lstPose = v;
            else if (paths[k].object < S) srcPose[(size_t)paths[k].object] = v;
        }

        // 1. Predicted energy from baked tables
        std::vector<double> pred((size_t)S * (size_t)R * NB, 0.0);
        std::vector<uint8_t> covered((size_t)S, 0);
        for (const auto& t : tables) {
            double u = 0.0;
            // Find phase matching t->path
            for (size_t k = 0; k < paths.size(); ++k) {
                if (paths[k].object == t->path.object) { u = pathPhase[k]; break; }
            }
            std::vector<double> scratch(t->stride());
            if (t->lookup(u, scratch.data())) {
                for (size_t si = 0; si < t->groupSources.size(); ++si) {
                    const int s = t->groupSources[si];
                    if (s >= 0 && s < S) {
                        for (int r = 0; r < R; ++r)
                            for (int b = 0; b < NB; ++b)
                                pred[((size_t)s * (size_t)R + (size_t)r) * NB + (size_t)b] =
                                    scratch[(si * (size_t)R + (size_t)r) * NB + (size_t)b];
                        covered[(size_t)s] = 1;
                    }
                }
            }
        }
        // Reference full trace at these exact poses
        const auto rc = e16ReceiversAt(traceSc, lstPose);
        std::vector<double> ref((size_t)S * (size_t)R * NB, 0.0);
        for (int s = 0; s < S; ++s) {
            Transducer src = traceSc.sources[(size_t)s];
            src.pos = {srcPose[(size_t)s][0], srcPose[(size_t)s][1], srcPose[(size_t)s][2]};
            src.azimuthDeg = srcPose[(size_t)s][3];
            const auto e = traceTailEnergy(traceSc, src, rc, cfg.erOrder, refRays, traceSc.seed + 131 * s);
            for (int r = 0; r < R; ++r)
                for (int b = 0; b < NB; ++b)
                    ref[((size_t)s * (size_t)R + (size_t)r) * NB + (size_t)b] = e[(size_t)r][(size_t)b];
            if (!covered[(size_t)s]) {
                for (int r = 0; r < R; ++r)
                    for (int b = 0; b < NB; ++b)
                        pred[((size_t)s * (size_t)R + (size_t)r) * NB + (size_t)b] = e[(size_t)r][(size_t)b];
            }
        }
        res.maxErrDb = std::max(res.maxErrDb, e16MaxErrDb(pred, ref, S, R));

        if (checkAudio) {
            // Move Engine cleanly to target poses and process audio
            eng.setListener(lstPose[0], lstPose[1], lstPose[2], lstPose[3]);
            for (int s = 0; s < S; ++s) {
                eng.setSourcePosition(s, srcPose[(size_t)s][0], srcPose[(size_t)s][1], srcPose[(size_t)s][2]);
                eng.setSourceAzimuth(s, srcPose[(size_t)s][3]);
            }
            eng.notifyJump();
            const auto lookupsBefore = eng.bakeStats().lookups;
            for (int blk = 0; blk < 4; ++blk) eng.process(ip.data(), op.data(), 128);
            const auto lookupsAfter = eng.bakeStats().lookups;
            check(lookupsAfter > lookupsBefore, (std::string(label) + ": audio consumes baked table at test pose").c_str());

            if (shared) {
                // Compare Engine::lateAppliedEnergy() [(r*S + s)*7 + b] against pred [(s*R + r)*7 + b]
                const auto applied = eng.lateAppliedEnergy();
                if (!applied.empty()) {
                    std::vector<double> appliedSRB((size_t)S * (size_t)R * NB, 0.0);
                    for (int r = 0; r < R; ++r)
                        for (int s = 0; s < S; ++s)
                            for (int b = 0; b < NB; ++b)
                                appliedSRB[((size_t)s * (size_t)R + (size_t)r) * NB + (size_t)b] =
                                    applied[((size_t)r * (size_t)S + (size_t)s) * NB + (size_t)b];
                    res.maxAppliedErrDb = std::max(res.maxAppliedErrDb, e16MaxErrDb(appliedSRB, pred, S, R));
                }
            } else if (ti == 0) {
                // Verify per-source audio equalization before worker rebuild vs cold static reference engine
                Scene refSc = traceSc;
                refSc = withListenerAt(refSc, {lstPose[0], lstPose[1], lstPose[2]});
                refSc.listenerYaw = lstPose[3];
                refSc.receivers = rc;
                for (int s = 0; s < S; ++s) {
                    refSc.sources[(size_t)s].pos = {srcPose[(size_t)s][0], srcPose[(size_t)s][1], srcPose[(size_t)s][2]};
                    refSc.sources[(size_t)s].azimuthDeg = srcPose[(size_t)s][3];
                }
                EngineConfig refCfg = cfg;
                refCfg.tailThread = false;
                Engine refEng(refSc, refCfg);
                eng.setSoloMask(4);
                refEng.setSoloMask(4);
                // Measure broadband RMS over 0.4 s of white noise (wait for 10 ms equalization ramp to settle first)
                std::vector<float> noise(128);
                std::vector<const float*> nip((size_t)S, noise.data());
                std::vector<std::vector<float>> eOut((size_t)R, std::vector<float>(128));
                std::vector<std::vector<float>> rOut((size_t)R, std::vector<float>(128));
                std::vector<float*> eOp((size_t)R), rOp((size_t)R);
                for (int r = 0; r < R; ++r) { eOp[(size_t)r] = eOut[(size_t)r].data(); rOp[(size_t)r] = rOut[(size_t)r].data(); }
                double eSum = 0.0, rSum = 0.0;
                for (int blk = 0; blk < 160; ++blk) {
                    for (float& v : noise) v = nd(rng);
                    eng.process(nip.data(), eOp.data(), 128);
                    refEng.process(nip.data(), rOp.data(), 128);
                    if (blk >= 20) {
                        for (int r = 0; r < R; ++r)
                            for (int j = 0; j < 128; ++j) {
                                eSum += (double)eOut[(size_t)r][(size_t)j] * eOut[(size_t)r][(size_t)j];
                                rSum += (double)rOut[(size_t)r][(size_t)j] * rOut[(size_t)r][(size_t)j];
                            }
                    }
                }
                const double rmsErr = std::abs(10.0 * std::log10(std::max(1e-30, eSum) / std::max(1e-30, rSum)));
                res.maxAudioRmsErrDb = std::max(res.maxAudioRmsErrDb, rmsErr);
                eng.setSoloMask(0);
            }
        }
    }

    std::printf("  [E16] %-42s shared=%d ms=%6.1f nodes=%3zu traceErr=%.3f dB", label, (int)shared, res.ms, res.nodesSum, res.maxErrDb);
    if (shared && checkAudio) std::printf(" appliedErr=%.4f dB", res.maxAppliedErrDb);
    if (!shared && checkAudio) std::printf(" audioRmsErr=%.3f dB", res.maxAudioRmsErrDb);
    std::printf("\n");

    check(res.maxErrDb <= 1.0, (std::string(label) + ": table accuracy <= 1 dB on >=-40 dB entries and sum").c_str());
    if (shared && checkAudio) check(res.maxAppliedErrDb <= 0.1, (std::string(label) + ": shared lateAppliedEnergy matches table").c_str());
    if (!shared && checkAudio) check(res.maxAudioRmsErrDb <= 1.0, (std::string(label) + ": per-source audio wet RMS <= 1 dB").c_str());
    return res;
}

static BakedPath makeLinearPath(int object, int group, Vec3 a, Vec3 b, double yawA = 0.0, double yawB = 0.0) {
    BakedPath p;
    p.object = object;
    p.group = group;
    p.poses = {{{a.x, a.y, a.z, yawA}}, {{b.x, b.y, b.z, yawB}}};
    p.distance = {0.0, norm(b - a)};
    return p;
}

} // namespace

static void testE16Paths() {
    std::printf("E16: multi-source path bake & coupled listener+source bake\n");

    // Helper: Vocal booth or Studio with clean zero-clutter 3 sources
    auto makeMultiRoom = [&](int nSrc, bool openContour, bool withDir) {
        Scene sc = builtinSceneNamed("Studio");
        sc.clutter.count = 0;
        sc.roughness = 0.0;
        sc.sources.resize(1);
        sc.sources[0].name = "S0";
        sc.sources[0].pos = {1.5, 1.5, 1.4};
        sc.sources[0].azimuthDeg = 20.0;
        if (nSrc >= 2) {
            Transducer s1 = sc.sources[0];
            s1.name = "S1";
            s1.pos = {4.2, 1.8, 1.3};
            s1.azimuthDeg = -60.0;
            if (withDir) {
                const auto& dirs = builtinDirectivities();
                if (!dirs.empty()) {
                    const auto& prof = dirs[std::min<size_t>(1, dirs.size() - 1)];
                    s1.instrument = prof->id;
                    s1.directivity = prof;
                    s1.instrumentAmount = 1.0;
                }
            }
            sc.sources.push_back(s1);
        }
        if (nSrc >= 3) {
            Transducer s2 = sc.sources[0];
            s2.name = "S2";
            s2.pos = {2.5, 4.0, 1.6};
            s2.azimuthDeg = 90.0;
            sc.sources.push_back(s2);
        }
        sc = withListenerAt(sc, {3.0, 2.8, 1.5});
        sc.listenerYaw = 0.0;
        if (openContour) {
            sc.ceilMat = -1;
            if (sc.walls.size() >= 4) sc.walls.pop_back();
        }
        return sc;
    };

    // 1. Part 1: 2 independently moving sources, static listener (uncorrelated non-diadic pairs), both shared=false and shared=true
    const std::vector<std::vector<double>> pairs2 = {
        {0.17, 0.83}, {0.61, 0.29}, {0.37, 0.71}, {0.89, 0.13}, {0.43, 0.67}
    };
    const auto sc2 = makeMultiRoom(2, false, false);
    const std::vector<BakedPath> paths2 = {
        makeLinearPath(0, -1, {1.2, 1.2, 1.4}, {2.4, 3.6, 1.4}, 20.0, 45.0),
        makeLinearPath(1, -1, {4.5, 1.4, 1.3}, {3.8, 4.1, 1.5}, -60.0, -110.0)
    };
    for (bool shared : {false, true}) {
        runE16BakeAccuracy("P1_2src_indep", sc2, paths2, pairs2, shared);
    }

    // 2. Worker-count determinism for 2 independent sources
    for (bool shared : {false, true}) {
        EngineConfig c1 = e4aConfig(shared); c1.tailRays = 6000; c1.adaptiveTail = true; c1.rebuildWorkers = 1;
        EngineConfig cN = c1; cN.rebuildWorkers = 27;
        Engine a(sc2, c1), b(sc2, cN);
        a.setBakedPaths(paths2); b.setBakedPaths(paths2);
        a.requestPathBake(1); b.requestPathBake(27);
        std::vector<std::vector<float>> input(2, std::vector<float>(128, 0.01f)), output((size_t)a.numReceivers(), std::vector<float>(128));
        std::vector<const float*> ip = {input[0].data(), input[1].data()};
        std::vector<float*> op; for (auto& v : output) op.push_back(v.data());
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while ((a.bakeStats().percent < 100 || b.bakeStats().percent < 100) && std::chrono::steady_clock::now() < deadline) {
            a.process(ip.data(), op.data(), 128); a.runWorkerStep();
            b.process(ip.data(), op.data(), 128); b.runWorkerStep();
        }
        check(a.bakeStats().hash != 0 && a.bakeStats().hash == b.bakeStats().hash,
              shared ? "E16 multi-source shared deterministic hash 1 vs 27 workers" : "E16 multi-source per-source deterministic hash 1 vs 27 workers");
    }

    // 3. Part 1: 3 independently moving sources + instrument DirectivityProfile + open-contour scene
    const std::vector<std::vector<double>> triples3 = {
        {0.19, 0.73, 0.41}, {0.83, 0.27, 0.63}, {0.31, 0.59, 0.87}, {0.67, 0.13, 0.23}
    };
    const auto sc3open = makeMultiRoom(3, true, true);
    const std::vector<BakedPath> paths3 = {
        makeLinearPath(0, -1, {1.2, 1.2, 1.4}, {2.2, 3.2, 1.4}, 20.0, 20.0),
        makeLinearPath(1, -1, {4.5, 1.5, 1.3}, {3.8, 3.7, 1.5}, -60.0, -30.0),
        makeLinearPath(2, -1, {1.6, 4.1, 1.5}, {3.4, 4.3, 1.6}, 90.0, 120.0)
    };
    for (bool shared : {false, true}) {
        runE16BakeAccuracy("P1_3src_dir_open", sc3open, paths3, triples3, shared);
    }

    // 4. Part 1: Selective invalidation (path edit & pattern edit), partial rebake, untouched source remains active in audio
    for (bool shared : {false, true}) {
        EngineConfig cfg = e4aConfig(shared);
        cfg.tailRays = 6000;
        cfg.adaptiveTail = true;
        Engine eng(sc2, cfg);
        eng.setBakedPaths(paths2);
        eng.requestPathBake(3);
        std::vector<std::vector<float>> input(2, std::vector<float>(128, 0.01f)), output((size_t)eng.numReceivers(), std::vector<float>(128));
        std::vector<const float*> ip = {input[0].data(), input[1].data()};
        std::vector<float*> op; for (auto& v : output) op.push_back(v.data());
        auto step = [&]() { eng.process(ip.data(), op.data(), 128); eng.runWorkerStep(); };
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (eng.bakeStats().percent < 100 && std::chrono::steady_clock::now() < deadline) step();
        const auto snaps0 = eng.bakedTablesSnapshot();
        check(snaps0.size() == 2 && eng.bakeStats().groups.size() == 2, "E16 2-source bake produces 2 independent group tables");
        const uint64_t hash0Before = snaps0.empty() ? 0 : snaps0[0]->hash();

        // Edit ONLY source 1 path
        auto paths2Edit = paths2;
        paths2Edit[1] = makeLinearPath(1, -1, {4.6, 1.4, 1.3}, {3.9, 4.2, 1.5}, -60.0, -110.0);
        eng.setBakedPaths(paths2Edit);
        const auto stEdit = eng.bakeStats();
        check(stEdit.state == Engine::BakeState::Outdated && stEdit.groups.size() == 2 &&
              stEdit.groups[0].state == Engine::BakeState::Baked &&
              stEdit.groups[1].state == Engine::BakeState::Outdated,
              "E16 editing source 1 path invalidates only source 1 (source 0 stays Baked)");

        // Source 0 remains active on audio thread while source 1 is outdated
        const auto p0Mid = paths2[0].at(0.5);
        eng.setSourcePosition(0, p0Mid[0], p0Mid[1], p0Mid[2]);
        eng.setSourceAzimuth(0, p0Mid[3]);
        eng.notifyJump();
        const auto lookupsBefore = eng.bakeStats().lookups;
        eng.process(ip.data(), op.data(), 128);
        check(eng.bakeStats().lookups > lookupsBefore, "E16 untouched source 0 still served from baked table on audio thread");

        // Partial rebake keeps source 0 hash identical while rebuilding source 1
        eng.requestPathBake(3);
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (eng.bakeStats().percent < 100 && std::chrono::steady_clock::now() < deadline) step();
        const auto snaps1 = eng.bakedTablesSnapshot();
        check(eng.bakeStats().percent == 100 && snaps1.size() == 2 && snaps1[0]->hash() == hash0Before,
              "E16 partial rebake keeps untouched source 0 hash identical");

        // Pattern edit on source 1 invalidates only group 1
        eng.setSourcePattern(1, 0.85);
        eng.process(ip.data(), op.data(), 128);
        step();
        const auto stPat = eng.bakeStats();
        check(stPat.state == Engine::BakeState::Outdated && stPat.groups.size() == 2 &&
              stPat.groups[0].state == Engine::BakeState::Baked &&
              stPat.groups[1].state == Engine::BakeState::Outdated,
              "E16 changing source 1 pattern invalidates only source 1");
    }

    // 5. Blocked rules: uncoupled listener + source, or coupled(listener+s0) + uncoupled(s1)
    {
        EngineConfig cfg = e4aConfig(true);
        cfg.tailRays = 4000;
        Engine eng(sc2, cfg);
        std::vector<std::vector<float>> input(2, std::vector<float>(128, 0.01f)), output((size_t)eng.numReceivers(), std::vector<float>(128));
        std::vector<const float*> ip = {input[0].data(), input[1].data()};
        std::vector<float*> op; for (auto& v : output) op.push_back(v.data());

        BakedPath lUncoupled = makeLinearPath(-1, -1, {2.8, 2.8, 1.5}, {3.3, 2.8, 1.5}, 0.0, 0.0);
        BakedPath s0Uncoupled = makeLinearPath(0, -1, {1.2, 1.2, 1.4}, {2.0, 1.2, 1.4}, 0.0, 0.0);
        eng.setBakedPaths({lUncoupled, s0Uncoupled});
        eng.requestPathBake(1);
        eng.process(ip.data(), op.data(), 128); eng.runWorkerStep();
        check(eng.bakeStats().state == Engine::BakeState::Blocked && eng.bakeStats().waitReason == 10,
              "E16 uncoupled listener + source is Blocked");

        BakedPath lCoupled = makeLinearPath(-1, 0, {2.8, 2.8, 1.5}, {3.3, 2.8, 1.5}, 0.0, 0.0);
        BakedPath s0Coupled = makeLinearPath(0, 0, {1.2, 1.2, 1.4}, {2.0, 1.2, 1.4}, 0.0, 0.0);
        BakedPath s1Uncoupled = makeLinearPath(1, -1, {4.5, 1.5, 1.3}, {3.8, 3.7, 1.5}, -60.0, -30.0);
        eng.setBakedPaths({lCoupled, s0Coupled, s1Uncoupled});
        eng.requestPathBake(1);
        eng.process(ip.data(), op.data(), 128); eng.runWorkerStep();
        check(eng.bakeStats().state == Engine::BakeState::Blocked && eng.bakeStats().waitReason == 10,
              "E16 coupled listener+s0 with extra uncoupled moving s1 is Blocked");
    }

    // 6. Part 2: Coupled 1 source + listener (group = 0)
    {
        const auto sc1 = makeMultiRoom(1, false, false);
        const std::vector<BakedPath> coupled1 = {
            makeLinearPath(-1, 0, {2.6, 2.5, 1.5}, {3.6, 3.3, 1.5}, 0.0, 35.0),
            makeLinearPath(0, 0, {1.3, 1.3, 1.4}, {2.3, 3.1, 1.4}, 20.0, 50.0)
        };
        const std::vector<std::vector<double>> u1 = {{0.17}, {0.43}, {0.79}};
        for (bool shared : {false, true}) {
            runE16BakeAccuracy("P2_coupled_L+S0", sc1, coupled1, u1, shared);
        }

        // Off-curve mismatch fallback when source moves away from coupled pose while listener stays on curve
        {
            EngineConfig cfg = e4aConfig(true);
            cfg.tailRays = 6000;
            Engine eng(sc1, cfg);
            eng.setBakedPaths(coupled1);
            eng.requestPathBake(3);
            std::vector<float> in(128, 0.01f);
            std::vector<std::vector<float>> out((size_t)eng.numReceivers(), std::vector<float>(128));
            const float* ip[1] = {in.data()};
            std::vector<float*> op; for (auto& v : out) op.push_back(v.data());
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
            while (eng.bakeStats().percent < 100 && std::chrono::steady_clock::now() < deadline) {
                eng.process(ip, op.data(), 128); eng.runWorkerStep();
            }
            const auto lOn = coupled1[0].at(0.5);
            const auto sOn = coupled1[1].at(0.5);
            eng.setListener(lOn[0], lOn[1], lOn[2], lOn[3]);
            eng.setSourcePosition(0, sOn[0] + 0.25, sOn[1], sOn[2]); // off coupled curve
            eng.setSourceAzimuth(0, sOn[3]);
            eng.notifyJump();
            const auto old = eng.bakeStats().lookups;
            eng.process(ip, op.data(), 128);
            check(eng.bakeStats().lookups == old, "E16 coupled listener+source falls back cleanly when member is off curve");
        }

        // Closed coupled loop (listener + source) & worker hash determinism
        {
            BakedPath lClosed{-1, 1, true, {{{2.6, 2.5, 1.5, 0.0}}, {{3.4, 2.5, 1.5, 0.0}}, {{2.6, 2.5, 1.5, 0.0}}}, {0.0, 0.8, 1.6}};
            BakedPath sClosed{0, 1, true, {{{1.3, 1.4, 1.4, 10.0}}, {{2.1, 2.4, 1.4, 20.0}}, {{1.3, 1.4, 1.4, 10.0}}}, {0.0, 1.280624847, 2.561249695}};
            EngineConfig c1 = e4aConfig(true); c1.tailRays = 6000; c1.rebuildWorkers = 1;
            EngineConfig cN = c1; cN.rebuildWorkers = 27;
            Engine a(sc1, c1), b(sc1, cN);
            a.setBakedPaths({lClosed, sClosed}); b.setBakedPaths({lClosed, sClosed});
            a.requestPathBake(1); b.requestPathBake(27);
            std::vector<float> in(128, 0.01f);
            std::vector<std::vector<float>> out((size_t)a.numReceivers(), std::vector<float>(128));
            const float* ip[1] = {in.data()};
            std::vector<float*> op; for (auto& v : out) op.push_back(v.data());
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
            while ((a.bakeStats().percent < 100 || b.bakeStats().percent < 100) && std::chrono::steady_clock::now() < deadline) {
                a.process(ip, op.data(), 128); a.runWorkerStep();
                b.process(ip, op.data(), 128); b.runWorkerStep();
            }
            check(a.bakeStats().percent == 100 && a.bakeStats().hash != 0 && a.bakeStats().hash == b.bakeStats().hash,
                  "E16 closed coupled listener+source loop bakes deterministically (1 vs 27 workers)");
            auto ct = a.bakedTableSnapshot();
            if (ct) {
                std::vector<double> e0(ct->stride()), e1(ct->stride());
                ct->lookup(0.0, e0.data()); ct->lookup(1.0, e1.data());
                check(e0 == e1, "E16 closed coupled loop wrap at u=1 reuses u=0");
            }
        }
    }

    // 7. Part 2: Coupled 2 sources + listener (group = 2) with 3rd static source present
    {
        const auto sc3 = makeMultiRoom(3, false, false);
        const std::vector<BakedPath> coupled2PlusL = {
            makeLinearPath(-1, 2, {2.5, 2.5, 1.5}, {3.5, 3.2, 1.5}, -15.0, 25.0),
            makeLinearPath(0, 2, {1.2, 1.3, 1.4}, {2.0, 2.8, 1.4}, 20.0, 40.0),
            makeLinearPath(1, 2, {4.4, 1.6, 1.3}, {3.9, 3.7, 1.5}, -60.0, -90.0)
        };
        const std::vector<std::vector<double>> u2 = {{0.23}, {0.59}, {0.81}};
        for (bool shared : {false, true}) {
            runE16BakeAccuracy("P2_coupled_L+S0+S1_staticS2", sc3, coupled2PlusL, u2, shared);
        }
    }

    // 8. Audio-thread zero-allocation check while 2-source multi-path bake runs on real background worker
    {
        EngineConfig cfg = e4aConfig(true);
        cfg.manualWorker = false;
        cfg.tailRays = 12000;
        cfg.adaptiveTail = true;
        Engine eng(sc2, cfg);
        eng.setBakedPaths(paths2);
        eng.requestPathBake();
        E10Audio audio(eng);
        e10Allocations = 0;
        const bool ok = audio.until([&]() { return eng.bakeStats().percent == 100; }, 90.0);
        check(ok && std::isfinite(audio.peak) && audio.peak > 1e-8 && e10Allocations == 0,
              "E16 real-time audio process allocates zero memory and has zero glitches during 2-source multi-path bake");
    }

    // 9. Bake-time scaling table (real background worker thread pool): 1, 2, 3 sources & coupled listener+source
    {
        std::printf("  [E16_TIME] Measuring multi-path bake scaling (shared=true, 12000 rays, adaptive):\n");
        const auto sc3 = makeMultiRoom(3, false, false);
        auto measureRealBake = [&](const char* name, const std::vector<BakedPath>& ps) {
            EngineConfig cfg = e4aConfig(true);
            cfg.manualWorker = false;
            cfg.tailRays = 12000;
            cfg.adaptiveTail = true;
            Engine eng(sc3, cfg);
            eng.setBakedPaths(ps);
            eng.requestPathBake();
            std::vector<std::vector<float>> input(3, std::vector<float>(128, 0.01f)), output((size_t)eng.numReceivers(), std::vector<float>(128));
            std::vector<const float*> ip = {input[0].data(), input[1].data(), input[2].data()};
            std::vector<float*> op; for (auto& v : output) op.push_back(v.data());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
            while (eng.bakeStats().percent < 100 && std::chrono::steady_clock::now() < deadline) {
                eng.process(ip.data(), op.data(), 128);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const auto st = eng.bakeStats();
            const auto tables = eng.bakedTablesSnapshot();
            size_t nodes = 0;
            for (const auto& t : tables) nodes += t->u.size();
            std::printf("    %-28s ms=%7.1f  stations=%4llu  nodes=%3zu  workers=%d\n",
                        name, st.milliseconds, (unsigned long long)st.stations, nodes, st.workers);
            check(st.percent == 100, (std::string("E16 timing ") + name + " completed").c_str());
        };
        measureRealBake("1_source_path", {paths3[0]});
        measureRealBake("2_source_paths", {paths3[0], paths3[1]});
        measureRealBake("3_source_paths", {paths3[0], paths3[1], paths3[2]});
        measureRealBake("coupled_L+S0", {
            makeLinearPath(-1, 0, {2.6, 2.5, 1.5}, {3.6, 3.3, 1.5}, 0.0, 35.0),
            makeLinearPath(0, 0, {1.3, 1.3, 1.4}, {2.3, 3.1, 1.4}, 20.0, 50.0)
        });
        measureRealBake("coupled_L+S0+S1", {
            makeLinearPath(-1, 0, {2.6, 2.5, 1.5}, {3.6, 3.3, 1.5}, 0.0, 35.0),
            makeLinearPath(0, 0, {1.3, 1.3, 1.4}, {2.3, 3.1, 1.4}, 20.0, 50.0),
            makeLinearPath(1, 0, {4.4, 1.6, 1.3}, {3.9, 3.7, 1.5}, -60.0, -90.0)
        });
    }
}
