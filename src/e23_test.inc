// E23: path bake accuracy gate (<= 1 dB) and cost with 16-channel AmbiX (3rd order) receivers.

static void testE23Bake() {
    std::printf("E23: path bake with 4/9/16 AmbiX receivers (accuracy gate <= 1 dB)\n");
    const std::vector<std::vector<double>> pairs2 = {{0.17, 0.83}, {0.61, 0.29}, {0.37, 0.71}, {0.89, 0.13}, {0.43, 0.67}};
    struct Case { Layout lay; const char* name; };
    for (const Case cs : {Case{Layout::AmbiX1, "AmbiX1 (4 ch)"}, Case{Layout::AmbiX2, "AmbiX2 (9 ch)"}, Case{Layout::AmbiX3, "AmbiX3 (16 ch)"}}) {
        Scene sc = builtinSceneNamed("Studio");
        sc.clutter.count = 0;
        sc.roughness = 0.0;
        sc.sources.resize(1);
        sc.sources[0].name = "S0"; sc.sources[0].pos = {1.5, 1.5, 1.4}; sc.sources[0].azimuthDeg = 20.0;
        Transducer s1 = sc.sources[0]; s1.name = "S1"; s1.pos = {4.2, 1.8, 1.3}; s1.azimuthDeg = -60.0;
        sc.sources.push_back(s1);
        sc = withListenerAt(sc, {3.0, 2.8, 1.5});
        sc.listenerYaw = 0.0;
        sc.receivers = makeReceiverArray(cs.lay, sc.listenerPos, 0.1);
        const std::vector<BakedPath> paths2 = {
            makeLinearPath(0, -1, {1.2, 1.2, 1.4}, {2.4, 3.6, 1.4}, 20.0, 45.0),
            makeLinearPath(1, -1, {4.5, 1.4, 1.3}, {3.8, 4.1, 1.5}, -60.0, -110.0)};
        for (bool shared : {false, true}) {
            const std::string label = std::string("E23 ") + cs.name + (shared ? " shared" : " per-source");
            const int before = fails;
            const auto r = runE16BakeAccuracy(label.c_str(), sc, paths2, pairs2, shared);
            const int R = (int)sc.receivers.size();
            const double perNode = 2.0 * 2 * R * NB * 8;  // energy + amplitude tables, S = 2 sources, doubles
            std::printf("  [E23-BAKE] %s: R=%d bake %.1f ms, nodes %zu, tables ~%.1f KiB (energy+amplitude), table err %.3f dB, audio rms err %.3f dB, applied err %.4f dB, gate %s\n",
                        label.c_str(), R, r.ms, r.nodesSum, (double)r.nodesSum * perNode / 1024.0, r.maxErrDb, r.maxAudioRmsErrDb, r.maxAppliedErrDb,
                        (fails == before && r.maxErrDb <= 1.0) ? "PASS" : "FAIL");
            check(r.maxErrDb <= 1.0, (label + ": table error within 1 dB").c_str());
        }
    }
}

// Engine-level scaling checks with 16 receivers: per-source tails, shared tail, Creative wet (width, pre-delay), Freeze.
static void testE23Scaling() {
    std::printf("E23: 16-channel scaling (per-source and shared tails, wet width/pre-delay, freeze)\n");
    for (bool shared : {false, true}) {
        Scene sc = builtinSceneNamed("Studio");
        sc.clutter.count = 0;
        sc.roughness = 0.0;
        sc.sources.resize(1);
        sc.sources[0].name = "S0"; sc.sources[0].pos = {1.5, 1.5, 1.4};
        for (int k = 1; k < 3; ++k) { Transducer t = sc.sources[0]; t.name = "S" + std::to_string(k); t.pos = {1.5 + 1.1 * k, 1.5 + 0.7 * k, 1.4}; sc.sources.push_back(t); }
        sc = withListenerAt(sc, {3.0, 2.8, 1.5});
        sc.receivers = makeReceiverArray(Layout::AmbiX3, sc.listenerPos, 0.1);
        EngineConfig cfg = e4aConfig(shared);
        cfg.tailRays = 6000;
        Engine eng(sc, cfg);
        eng.setCreativeWet(20.0, 150.0, false, 0, 300);
        const int S = eng.numSources(), R = eng.numReceivers();
        check(R == 16, "E23 scaling: 16 receivers in the engine");
        std::vector<std::vector<float>> in((size_t)S, std::vector<float>(128, 0.f)), out((size_t)R, std::vector<float>(128, 0.f));
        std::vector<const float*> ip((size_t)S); std::vector<float*> op((size_t)R);
        for (int s = 0; s < S; ++s) ip[(size_t)s] = in[(size_t)s].data();
        for (int r = 0; r < R; ++r) op[(size_t)r] = out[(size_t)r].data();
        std::mt19937 rng(7); std::uniform_real_distribution<float> U(-0.3f, 0.3f);
        std::vector<double> energy((size_t)R, 0.0), frozen((size_t)R, 0.0);
        bool finite = true;
        const int blocks = 48000 * 3 / 128;
        for (int b = 0; b < blocks; ++b) {
            const bool fz = b >= 48000 * 2 / 128;
            if (b == 48000 * 2 / 128) eng.setCreativeWet(20.0, 150.0, true, 0, 300);
            for (int s = 0; s < S; ++s) for (float& v : in[(size_t)s]) v = fz ? 0.f : U(rng);
            eng.process(ip.data(), op.data(), 128);
            for (int r = 0; r < R; ++r)
                for (float v : out[(size_t)r]) { finite &= std::isfinite(v); (fz ? frozen : energy)[(size_t)r] += (double)v * v; }
        }
        int quiet = 0, quietFrozen = 0;
        for (int r = 0; r < R; ++r) { if (!(energy[(size_t)r] > 1e-9)) ++quiet; if (!(frozen[(size_t)r] > 1e-9)) ++quietFrozen; }
        std::printf("  [E23-SCALE] %s tail, 3 sources, 16 ch: finite=%d, silent channels live=%d frozen=%d\n", shared ? "shared" : "per-source", (int)finite, quiet, quietFrozen);
        check(finite && quiet == 0, shared ? "E23 scaling shared tail: all 16 channels live and finite" : "E23 scaling per-source tails: all 16 channels live and finite");
        check(quietFrozen == 0, shared ? "E23 scaling shared tail: freeze holds energy on all 16 channels" : "E23 scaling per-source tails: freeze holds energy on all 16 channels");
    }
}
