// Engine regression run via VERB_ONLY_F2=1 rt_test.
static void testF2Rebuild() {
    using Clock = std::chrono::steady_clock;
    constexpr int block = 512;
    std::istringstream sceneText(
        "sr 48000\nlength 0.5\nheight 3\n"
        "material lining 0.52 0.52 0.52 0.52 0.52 0.52 0.52 0.1 0.2\n"
        "floor lining\nceiling lining\n"
        "wall 0 0 6 0 lining\nwall 6 0 6 6 lining\n"
        "wall 6 6 0 6 lining\nwall 0 6 0 0 lining\n"
        "source S 2 2 1.5 0 0 0 0\nreceiver R 3 3 1.5 0 0 0 0\nlistener 3 3 1.5 0\n");
    const auto original = parseScene(sceneText, "F2 room");
    auto moved = original;
    moved.walls[1].ax = moved.walls[1].bx = moved.walls[0].bx = moved.walls[2].ax = 5;
    auto replaced = moved;
    replaced.height = 4;
    replaced.walls[1].by = 4;
    auto chamfer = replaced.walls[1];
    chamfer.ay = 4;
    chamfer.bx = 4;
    chamfer.by = 6;
    replaced.walls[2].ax = 4;
    replaced.walls.insert(replaced.walls.begin() + 2, chamfer);

    BakedPath listener, source;
    listener.group = source.group = 1;
    source.object = 0;
    listener.poses = {{{3, 3, 1.5, 0}}, {{3.04, 3, 1.5, 0}}};
    source.poses = {{{2, 2, 1.5, 0}}, {{2.04, 2, 1.5, 0}}};
    listener.distance = source.distance = {0, .04};
    const std::vector<BakedPath> paths{listener, source};
    auto wait = [](auto&& tick, auto&& ready, int seconds) {
        const auto deadline = Clock::now() + std::chrono::seconds(seconds);
        do {
            tick();
            if (ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (Clock::now() < deadline);
        return false;
    };

    uint64_t stoppedHash = 0;
    for (bool shared : {false, true}) for (bool audio : {false, true}) {
        std::printf("F2 shared=%d audio=%d\n", shared, audio);
        EngineConfig cfg;
        cfg.sharedTail = shared;
        cfg.tailRays = 8000;
        cfg.tailRefreshMs = 20;
        cfg.rebuildWorkers = 2;
        Engine engine(original, cfg);
        engine.setBakedPaths(paths);
        float input[block]{}, output[block]{};
        const float* in[] = {input};
        float* out[] = {output};
        int blocks = 0;
        auto silence = [&] {
            if (audio) { engine.process(in, out, block); ++blocks; }
        };
        auto bake = [&](const char* label) {
            const auto start = Clock::now();
            const int firstBlock = blocks;
            engine.requestPathBake(2);
            const bool done = wait(silence, [&] {
                const auto state = engine.bakeStats().state;
                return state == Engine::BakeState::Baked || state == Engine::BakeState::Approximate;
            }, 8);
            const auto stats = engine.bakeStats();
            std::printf("  %s: %.3fs blocks=%d state=%d reason=%d percent=%d hash=%016llx refresh=%d\n",
                label, std::chrono::duration<double>(Clock::now() - start).count(), blocks - firstBlock,
                (int)stats.state, stats.waitReason, stats.percent, (unsigned long long)stats.hash,
                engine.tailRefreshCount());
            check(done, label);
            return done;
        };
        if (!bake("initial group bakes")) continue;
        const auto initial = engine.bakedTableSnapshot();

        // Publish a fresh IR before the edit. With no callbacks the audio thread cannot take it.
        const int refresh = engine.tailRefreshCount();
        engine.setTailQuality(9000, false);
        check(wait(silence, [&] { return engine.tailRefreshCount() > refresh; }, 5),
              "worker publishes a tail refresh before the wall edit");
        if (!audio) check(!engine.tailIsCurrent(), "audio has not consumed the published refresh");
        check(engine.setScene(moved), "move wall with both corners sewn");
        bake("group bakes after wall moved");
        const auto afterMove = engine.bakedTableSnapshot();
        if (afterMove && initial) check(afterMove->geometry != initial->geometry &&
            afterMove->contentSig != initial->contentSig && afterMove->energy != initial->energy,
            "moved room has a different revision, signature and energy");
        engine.cancelPathBake();
        check(engine.setScene(replaced), "replace room with five walls and different height");
        const bool replacedBaked = bake("group bakes after replacement and Cancel");
        const auto afterReplace = engine.bakedTableSnapshot();
        if (!replacedBaked || !afterReplace) continue;
        check(afterReplace->movingPaths.size() == 2 && afterReplace->contentSig == engine.bakeContentSignatures().at(0) &&
            (!afterMove || (afterReplace->geometry != afterMove->geometry && afterReplace->energy != afterMove->energy)),
            "replacement bake has both paths and matches the new room");

        engine.clearPathBake();
        check(engine.bakeStats().state == Engine::BakeState::NotBaked && !engine.bakedTableSnapshot(),
              "Clear bake removes the table");
        engine.requestPathBake(2);
        check(wait(silence, [&] {
            const auto stats = engine.bakeStats();
            return stats.state == Engine::BakeState::Baking && stats.total > 0;
        }, 5), "stations start before Cancel");
        engine.cancelPathBake();
        check(engine.bakeStats().state == Engine::BakeState::Cancelled, "Cancel interrupts a requested bake");
        if (bake("group rebakes after Clear and Cancel"))
            check(engine.bakedTableSnapshot()->hash() == afterReplace->hash(), "rebake is deterministic");
        if (audio) check(stoppedHash == afterReplace->hash(), "stopped and running bake hashes match");
        else stoppedHash = afterReplace->hash();

        // Only now resume processing in the stopped case. This also exercises
        // consumption of the old pending IR followed by the edited room IR.
        auto pump = [&] { engine.process(in, out, block); };
        check(wait(pump, [&] { return engine.tailIsCurrent(); }, 5),
              "audio consumes the replacement room tail");
        const auto scene = serializeScene(replaced, 17);
        std::istringstream savedScene(scene);
        const auto loaded = parseScene(savedScene, "F2 reloaded room");
        check(loaded.walls.size() == 5 && loaded.height == 4 && serializeScene(loaded, 17) == scene,
              "replacement geometry, heights and materials survive scene reload");
        cfg.tailRays = 9000;
        cfg.tailThread = false;
        Engine reloaded(loaded, cfg);
        reloaded.setBakedPaths(paths);
        const auto blob = bakeSerializeTables({*afterReplace});
        std::vector<BakedTable> tables;
        check(bakeDeserializeTables(blob.data(), blob.size(), tables) && reloaded.installBakedTables(tables) &&
            reloaded.bakedTableSnapshot()->hash() == afterReplace->hash(), "VRBK restores the replacement table offline");
        auto impulse = [&](Engine& target) {
            std::vector<float> ir;
            for (int i = 0; i < 96; ++i) target.process(in, out, block);
            for (int i = 0; i < 96; ++i) {
                input[0] = i == 0 ? 1.f : 0.f;
                target.process(in, out, block);
                ir.insert(ir.end(), output, output + block);
            }
            return ir;
        };
        const auto live = impulse(engine);
        const auto restored = impulse(reloaded);
        double peakDiff = 0;
        for (size_t i = 0; i < live.size(); ++i)
            peakDiff = std::max(peakDiff, std::abs((double)live[i] - restored[i]));
        const double liveEnergy = energy(live, 0, live.size());
        const double reloadedEnergy = energy(restored, 0, restored.size());
        std::printf("  impulse: energy=%.12g/%.12g max abs diff=%.9g\n", liveEnergy, reloadedEnergy, peakDiff);
        check(finite(live) && finite(restored) && liveEnergy > 0 && reloadedEnergy > 0 && peakDiff <= 2e-6,
              "edited and reloaded room produce equivalent finite non-silent impulses");
    }
}
