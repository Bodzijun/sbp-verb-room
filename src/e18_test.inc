// E18: timed path bake (coupled group with mismatched start/end windows, rest-station skipping, invalidation, zero allocs).

static void testE18TimedBake() {
    std::printf("E18: timed coupled-group bake, rest-station skipping, invalidation & zero allocs\n");

    auto makeTimedScene = [&]() {
        Scene sc = builtinSceneNamed("Studio");
        sc.clutter.count = 0;
        sc.roughness = 0.0;
        sc.sources.resize(2);
        sc.sources[1] = sc.sources[0];
        sc.sources[0].name = "S0";
        sc.sources[0].pos = {1.3, 1.4, 1.4};
        sc.sources[0].azimuthDeg = 15.0;
        sc.sources[1].name = "S1";
        sc.sources[1].pos = {4.3, 3.6, 1.5};
        sc.sources[1].azimuthDeg = -60.0;
        sc = withListenerAt(sc, {2.8, 2.5, 1.5});
        sc.listenerYaw = 0.0;
        return sc;
    };

    const Scene sc = makeTimedScene();

    // 1. Coupled group with two sources having different windows:
    //    S0 ends at 75% (rests on [0.75, 1.0]); S1 runs 0..100% with an EaseInOut timing key at 40%.
    BakedPath p0 = makeLinearPath(0, 0, {1.3, 1.4, 1.4}, {2.6, 3.4, 1.4}, 15.0, 45.0);
    p0.startU = 0.0;
    p0.endU = 0.75;
    p0.timingSig = 0x101ULL;

    BakedPath p1 = makeLinearPath(1, 0, {4.3, 3.6, 1.5}, {3.2, 1.5, 1.5}, -60.0, -10.0);
    p1.startU = 0.0;
    p1.endU = 1.0;
    p1.poses = {{{4.3, 3.6, 1.5, -60.0}}, {{3.75, 2.55, 1.5, -35.0}}, {{3.2, 1.5, 1.5, -10.0}}};
    p1.distance = {0.0, 1.185326959, 2.370653918};
    p1.timingKeys = {{0.0, 0.0, 3}, {0.4, 0.5, 0}, {1.0, 1.0, 0}}; // EaseInOut to midpoint at t=40%
    p1.timingSig = 0x102ULL;

    const std::vector<BakedPath> timedPair = {p0, p1};
    // Verify u=0.85 rests p0 at its end while p1 is still moving toward its end.
    check(p0.at(0.85) == p0.at(1.0) && !(p1.at(0.85) == p1.at(1.0)),
          "E18 p0 rests above u=0.75 while coupled member p1 continues moving");

    const std::vector<std::vector<double>> probeU = {{0.10}, {0.40}, {0.85}};
    for (bool shared : {false, true}) {
        const auto run = runE16BakeAccuracy("E18_coupled_mismatched_windows", sc, timedPair, probeU, shared, 0, 12000, true);
        check(run.maxErrDb <= 1.0, "E18 coupled timed group passes <= 1 dB gate including rest probe u=0.85");
    }

    // 2. Pure rest outside active window: both members active on [0.20, 0.75] -> no refinement stations traced in [0, 0.20) or (0.75, 1.0].
    {
        BakedPath a0 = makeLinearPath(0, 0, {1.3, 1.4, 1.4}, {2.6, 3.4, 1.4}, 15.0, 45.0);
        a0.startU = 0.20; a0.endU = 0.75; a0.timingSig = 0x201ULL;
        BakedPath a1 = makeLinearPath(1, 0, {4.3, 3.6, 1.5}, {3.2, 1.5, 1.5}, -60.0, -10.0);
        a1.startU = 0.25; a1.endU = 0.70; a1.timingSig = 0x202ULL;

        EngineConfig cfg = e4aConfig(true);
        cfg.tailRays = 6000;
        cfg.adaptiveTail = true;
        cfg.rebuildWorkers = 0;
        Engine eng(sc, cfg);
        eng.setBakedPaths({a0, a1});
        eng.requestPathBake(0);
        std::vector<std::vector<float>> input(2, std::vector<float>(128, 0.01f)), output((size_t)eng.numReceivers(), std::vector<float>(128));
        const float* ip[2] = {input[0].data(), input[1].data()};
        std::vector<float*> op; for (auto& v : output) op.push_back(v.data());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (eng.bakeStats().percent < 100 && std::chrono::steady_clock::now() < deadline) {
            eng.process(ip, op.data(), 128);
            eng.runWorkerStep();
        }
        check(eng.bakeStats().percent == 100, "E18 narrow-window coupled group finishes bake");
        const auto tbl = eng.bakedTableSnapshot();
        check(tbl != nullptr && tbl->u.size() >= 2, "E18 narrow-window table has boundary nodes");
        if (tbl && tbl->u.size() >= 4) {
            int interiorRest = 0;
            for (double uv : tbl->u) {
                if ((uv > 1e-6 && uv < 0.20 - 1e-6) || (uv > 0.75 + 1e-6 && uv < 1.0 - 1e-6))
                    ++interiorRest;
            }
            check(interiorRest == 0 && tbl->u.front() == 0.0 && tbl->u.back() == 1.0,
                  "E18 skips tracing stations inside pure rest intervals while covering [0, 1]");
        }

        // Lookup in rest interval matches start/end energy cleanly and timing edit outdates table.
        if (tbl) {
            std::vector<double> e0(tbl->stride()), eRest0(tbl->stride()), e1(tbl->stride()), eRest1(tbl->stride());
            check(tbl->lookup(0.0, e0.data()) && tbl->lookup(0.10, eRest0.data()), "E18 lookup succeeds during start rest");
            check(tbl->lookup(1.0, e1.data()) && tbl->lookup(0.90, eRest1.data()), "E18 lookup succeeds during end rest");
            check(e0 == eRest0 && e1 == eRest1, "E18 rest interval reuses start/end energies consistently");
        }

        auto a1Edited = a1;
        a1Edited.endU = 0.80;
        a1Edited.timingSig = 0x203ULL;
        eng.setBakedPaths({a0, a1Edited});
        check(eng.bakeStats().state == Engine::BakeState::Outdated, "E18 editing path timing outdates the baked table");
    }

    // 3. Audio-thread zero-allocation check while timed coupled bake runs on background worker.
    {
        EngineConfig cfg = e4aConfig(true);
        cfg.manualWorker = false;
        cfg.tailRays = 12000;
        cfg.adaptiveTail = true;
        Engine eng(sc, cfg);
        eng.setBakedPaths(timedPair);
        eng.requestPathBake();
        E10Audio audio(eng);
        e10Allocations = 0;
        const bool ok = audio.until([&]() { return eng.bakeStats().percent == 100; }, 90.0);
        check(ok && std::isfinite(audio.peak) && audio.peak > 1e-8 && e10Allocations == 0,
              "E18 audio process allocates zero memory during timed coupled bake");
    }
}
