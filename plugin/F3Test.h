#pragma once

static int testF3() {
    using namespace verbp;
    const int before = fails;
    {
        verb::BakedPath listener, source;
        listener.object = -1;
        listener.poses = {{{2, 4, 1.5, 0}}, {{3, 4, 1.5, 0}}};
        listener.distance = {0, 1};
        source.object = 0;
        source.poses = {{{1, 1, 1.5, 0}}, {{3, 1, 1.5, 0}}};
        source.distance = {0, 1}; // joint distance for the recorded case
        source.yawMatch = {true, .25, 0.};
        verb::BakedTable table;
        table.movingPaths = {listener, source};
        table.buildJointCurve();
        double live[] = {2.5, 4, 1.5, 0, 1.75, 1, 1.5, 180};
        double phase = 0.;
        check(table.projectDeclared(live, phase) && std::abs(phase - .5) < 1e-9 &&
              table.projectJoint(live, phase) && std::abs(phase - .5) < 1e-9 &&
              source.project(live + 4, phase) && source.matchesAt(live + 4, phase) &&
              source.tracePose(.5)[0] == 2.25,
              "F3 offset points match at their live centre and trace at their baked angle");
        live[5] += .03;
        check(!table.projectDeclared(live, phase) && !table.projectJoint(live, phase) &&
              !source.project(live + 4, phase) && !source.matchesAt(live + 4, .5),
              "F3 Manual policy still rejects a 3 cm departure from the path");
        table.movingPaths.back().yawMatch = {};
        table.buildJointCurve();
        live[4] = 2.; live[5] = 1.;
        check(!table.projectDeclared(live, phase) && !table.projectJoint(live, phase),
              "F3 computed orientations still reject a yaw mismatch");
        verb::BakeByteWriter writer;
        writer.path(source);
        verb::BakeByteReader reader{writer.b.data(), writer.b.size()};
        verb::BakedPath restored;
        check(reader.path(restored) && restored == source && reader.pos == writer.b.size(),
              "F3 v2 path round-trips optional Manual point offsets");
    }
    for (int scenario = 0; scenario < 3; ++scenario) {
        const bool recorded = scenario != 0, rotationOnly = scenario == 2;
        const char* label = rotationOnly ? "recorded-rotation-only" : recorded ? "recorded" : "declared";
        auto processor = std::make_unique<VerbProcessor>();
        auto& p = *processor;
        p.setTailSettings({8000, false});
        p.setPlayConfigDetails(2, 2, 48000, 1024);
        p.prepareToPlay(48000, 1024);
        if (!waitReady(p, 30)) { check(false, "F3 processor ready"); continue; }
        p.loadSceneText(p4test::kScene, "F3 corridor", true);
        if (!waitReady(p, 30)) { check(false, "F3 scene ready"); continue; }
        MovementPath l, s;
        l.points = {{2, 4, 1.5}, {3, 4, 1.5}};
        s.points = {{1, 1, 1.5}, {3, 1, 1.5}};
        l.group = 0;
        s.group = recorded ? 1 : 0;
        p.setMovementPath(kListener, l);
        p.setMovementPath(0, s);
        E19Play host;
        p.setPlayHead(&host);
        juce::AudioBuffer<float> audio(2, 1024);
        juce::MidiBuffer midi;
        int toneSample = 0;
        bool feedTone = false;
        auto block = [&](int samples) {
            audio.setSize(2, samples, false, false, true);
            audio.clear();
            if (feedTone) for (int i = 0; i < samples; ++i)
                audio.setSample(0, i, .25f * (float)std::sin(2. * verb::kPi * 83. * toneSample++ / 48000.));
            p.processBlock(audio, midi);
        };
        auto engine = E9bAccess::engine(p);
        constexpr int steps = 128;
        auto yaw = [&](int i) {
            // One continuous envelope. Coarse callbacks apply the steep part as a snap.
            return recorded ? std::clamp((i - 60.) * 15., 0., 120.) : i * 120. / steps;
        };
        auto pose = [&](int i, double az) {
            const double u = (double)i / steps;
            setP(p, VerbProcessor::pathParamId(recorded ? kListener : 0), u);
            if (recorded) setP(p, VerbProcessor::pathParamId(0), rotationOnly ? 0. : u);
            setP(p, "s1az", az);
        };
        pose(0, 0);
        if (recorded) {
            check(p.getEditorSession().run(juce::JSON::parse(R"({"op":"path.recordPass"})")).ok, "F3 record command");
            const auto until = juce::Time::getMillisecondCounterHiRes() + 5000;
            while (engine->recordedPassStats().state != 1 && juce::Time::getMillisecondCounterHiRes() < until) {
                block(128);
                juce::Thread::sleep(5);
            }
            host.playing = true;
            block(128);
            for (int i = 8; i <= steps; i += 8) { pose(i, yaw(i)); block(1024); }
            host.playing = false;
            block(128);
            const auto data = engine->exportRecordedPass();
            std::printf("  F3 capture samples=%d objects=%d\n", data.samples, data.objects);
            check(data.samples > 16 && data.samples < 4096, "F3 recorded pass ready, below cap");
            for (int k = 1; k < data.samples; ++k) {
                const double* x = data.poses.data() + k * data.objects * 4;
                const double* prev = x - data.objects * 4;
                if (x[7] != prev[7]) std::printf("  F3 capture source yaw %.3f -> %.3f at x=%.5f -> %.5f\n", prev[7], x[7], prev[4], x[4]);
            }
            pose(0, 0);
            block(128);
        }
        check(p.startPathBake(-1) && p4test::waitBaked(p, 60), "F3 bake completes");
        const auto tables = engine->bakedTablesSnapshot();
        if (tables.size() != 1) { check(false, "F3 exactly one table"); continue; }
        check(tables[0]->path.recorded == recorded, "F3 expected table type");
        auto replay = [&](int stride, bool jump, bool measureStop = false) {
            const auto hash = engine->bakeStats().hash;
            setP(p, "soloTail", 1);
            pose(0, 0);
            feedTone = jump;
            toneSample = 0;
            if (measureStop) {
                // Start the settle test from a steady pose after the seek back from the jump run.
                const double ready = juce::Time::getMillisecondCounterHiRes() + 700.;
                do {
                    block(128);
                    juce::Thread::sleep(2);
                } while (juce::Time::getMillisecondCounterHiRes() < ready ||
                         (!engine->tailIsCurrent() && juce::Time::getMillisecondCounterHiRes() < ready + 5000.));
            } else {
                for (int i = 0; i < 48; ++i) block(128);
            }
            int hits = 0, off = 0;
            std::array<std::vector<float>, 2> output;
            host.playing = true;
            for (int i = stride; i <= steps; i += stride) {
                const double az = jump ? (i < steps / 2 ? 0. : 180.) : yaw(i);
                pose(i, az);
                const auto prev = engine->bakeStats().lookups;
                block(stride * 128);
                const auto lookups = engine->bakeStats().lookups - prev;
                hits += (int)lookups;
                const bool offPass = engine->recordedPassStats().offPass;
                off += offPass;
                if (jump) for (int ch = 0; ch < 2; ++ch)
                    output[ch].insert(output[ch].end(), audio.getReadPointer(ch), audio.getReadPointer(ch) + audio.getNumSamples());
                if (lookups < (uint64_t)stride && (i <= 8 || (i >= 52 && i <= 76) || i == steps)) {
                    const double u = (double)i / steps;
                    const double live[]{2 + u, 4, 1.5, 0, rotationOnly ? 1. : 1 + 2*u, 1, 1.5, az};
                    const auto& table = *tables[0];
                    double phase = 0;
                    const bool projection = recorded ? table.projectJoint(live, phase) :
                        table.hasDeclaredJoint() ? table.projectDeclared(live, phase) :
                        table.path.project(live + 4 * (table.path.object + 1), phase) &&
                            table.movingPaths.back().matchesAt(live + 4, phase);
                    double closest = 1e30, yawErr = 0, nearestJointPos = 0, nearestJointYaw = 0, jointBest = 1e30;
                    if (recorded) {
                        const auto& lp = table.movingPaths[0]; const auto& sp = table.movingPaths[1];
                        for (size_t k = 1; k < sp.poses.size(); ++k) {
                            const auto& a = sp.poses[k - 1]; const auto& b = sp.poses[k];
                            const double sourceSpan = b[0] - a[0], listenerSpan = lp.poses[k][0] - lp.poses[k-1][0];
                            if (sourceSpan == 0. && listenerSpan == 0.) continue;
                            const double f = std::clamp(rotationOnly ?
                                (live[0] - lp.poses[k-1][0]) / listenerSpan : (live[4] - a[0]) / sourceSpan, 0., 1.);
                            const double delta = std::abs(live[4] - a[0] - f * (b[0] - a[0]));
                            auto angularError = [&](double t) {
                                return std::abs(std::remainder(az - a[3] - t * std::remainder(b[3] - a[3], 360.), 360.));
                            };
                            if (delta < closest) { closest = delta; yawErr = angularError(f); }
                            const double ld = lp.poses[k][0] - lp.poses[k-1][0], sd = b[0] - a[0];
                            const double ad = std::remainder(b[3] - a[3], 360.) * .01;
                            const double le = live[0] - lp.poses[k-1][0], se = live[4] - a[0];
                            const double ae = std::remainder(az - a[3], 360.) * .01;
                            const double t = std::clamp((le*ld + se*sd + ae*ad) / (ld*ld + sd*sd + ad*ad), 0., 1.);
                            const double pos = std::hypot(le - t*ld, se - t*sd), angle = angularError(t);
                            const double joint = std::hypot(pos, .01 * angle);
                            if (joint < jointBest) { jointBest = joint; nearestJointPos = pos; nearestJointYaw = angle; }
                        }
                    } else {
                        closest = 0;
                        yawErr = std::abs(std::remainder(az - table.movingPaths.back().at(u)[3], 360.));
                    }
                    std::printf("  F3 %s step=%d hits=%llu offPass=%d projected=%d source_X_error=%.5fm source_yaw_error=%.3fdeg joint_best=%.5fm (XY=%.5fm, source_yaw=%.3fdeg)\n",
                                label, i, (unsigned long long)lookups, offPass, projection, closest, yawErr,
                                recorded ? jointBest : 0., nearestJointPos, nearestJointYaw);
                }
                // Give the real worker time to observe the moving poses and consume the mid-motion jump.
                juce::Thread::sleep(stride * 2);
            }
            feedTone = false;
            host.playing = false;
            std::printf("  F3 %s stride=%d jump=%d applied=%d/%d offPass=%d/%d hash=%llu->%llu\n",
                        label, stride, jump, hits, steps, off, steps/stride, (unsigned long long)hash,
                        (unsigned long long)engine->bakeStats().hash);
            check(hits == steps && off == 0, "F3 baked tail applied over the entire motion");
            check(hash == engine->bakeStats().hash && !engine->bakedTablesSnapshot().empty(), "F3 automation preserves the published bake");
            if (jump) {
                bool finite = true;
                int clicks = 0;
                double peak = 0.;
                for (int ch = 0; ch < 2; ++ch) {
                    for (float sample : output[ch]) finite &= std::isfinite(sample);
                    const auto detected = bakeclick::detect(output[ch], ch);
                    peak = std::max(peak, detected.peakOut);
                    for (const auto& click : detected.clicks)
                        clicks += std::abs((ptrdiff_t)click.pos - (steps / 2 - 1) * 128) <= 1024;
                }
                std::printf("  F3 %s 180-degree jump: finite=%d peak=%.6g clicks within 1024 samples=%d\n", label, finite, peak, clicks);
                check(finite && peak > 1e-5 && clicks == 0, "F3 180-degree jump is audible, finite and click-free");
                check(std::abs(p.liveAzimuth(0) - 180.) < .01, "F3 live azimuth still follows the host");
            }
        };
        if (recorded) replay(8, false);
        replay(1, true);
        replay(1, false, true);
        const auto beforeStop = engine->motionStats().transitionsDone;
        const auto until = juce::Time::getMillisecondCounterHiRes() + 15000.;
        while (juce::Time::getMillisecondCounterHiRes() < until) {
            block(128);
            if (engine->tailIsCurrent() && engine->motionStats().transitionsDone > beforeStop) break;
            juce::Thread::sleep(1);
        }
        const auto afterStop = engine->motionStats().transitionsDone;
        std::printf("  F3 %s accurate transitions after stop=%llu\n", label, afterStop - beforeStop);
        check(engine->tailIsCurrent() && afterStop == beforeStop + 1, "F3 one accurate update after stop");
        // Clear the tone and transition history completely before comparing the settled impulse.
        for (int i = 0; i < 6 * 48000 / 128; ++i) block(128);
        const auto ir = p4test::impulse(p, "s1path", rotationOnly ? 0. : 1.);
        const auto hash = engine->bakeStats().hash;
        auto reloaded = std::make_unique<VerbProcessor>();
        auto& q = *reloaded;
        q.setPlayConfigDetails(2, 2, 48000, 1024);
        q.prepareToPlay(48000, 1024);
        q.setKeepRoutingEnabled(false);
        juce::String error;
        const bool loaded = recorded ? p4test::loadStateXml(q, p4test::stateXml(p))
                                     : q.loadRoomPresetText(p.getRoomPresetText(), error) && waitReady(q, 30);
        check(loaded && p4test::waitBaked(q, 10), "F3 saved scene and table reload");
        if (loaded) {
            auto restoredEngine = E9bAccess::engine(q);
            check(restoredEngine->bakeStats().hash == hash &&
                  restoredEngine->bakeStats().restored == 1, "F3 reload restores the original table");
            const auto restoreDeadline = juce::Time::getMillisecondCounterHiRes() + 15000.;
            while (juce::Time::getMillisecondCounterHiRes() < restoreDeadline &&
                   !(restoredEngine->tailIsCurrent() && restoredEngine->motionStats().transitionsDone > 0))
                p4test::pump(q, 8);
            check(restoredEngine->tailIsCurrent() && restoredEngine->motionStats().transitionsDone > 0,
                  "F3 reload reaches the same settled path endpoint");
            p4test::pump(q, 8);
            const auto restored = p4test::impulse(q, "s1path", rotationOnly ? 0. : 1.);
            double energy = 0., restoredEnergy = 0.;
            bool finite = true;
            for (float v : ir) { energy += (double)v * v; finite &= std::isfinite(v); }
            for (float v : restored) { restoredEnergy += (double)v * v; finite &= std::isfinite(v); }
            const double diff = p4test::maxDiff(ir, restored);
            std::printf("  F3 %s impulse energy=%.9g reload=%.9g max_diff=%.9g tail_hash=%llx reload=%llx\n",
                        label, energy, restoredEnergy, diff, (unsigned long long)engine->lateStateHash(),
                        (unsigned long long)restoredEngine->lateStateHash());
            check(finite && energy > 1e-9 && restoredEnergy > 1e-9 && diff <= 2e-6,
                  "F3 settled impulse matches after reload within 2e-6");
        }
        p.setPlayHead(nullptr);
    }
    if (const char* legacy = std::getenv("VERB_F3_LEGACY_PRESET")) {
        const auto contents = repoFile(legacy).loadFileAsString();
        auto xml = juce::parseXML(contents);
        auto* state = p4test::verbStateOf(xml.get());
        const auto* cache = state ? state->getChildByName("BakeCache") : nullptr;
        const int version = cache ? cache->getIntAttribute("v") : 0;
        juce::MemoryBlock blob;
        std::vector<verb::BakedTable> tables;
        if (version == 2) {
            check(blob.fromBase64Encoding(cache->getStringAttribute("data")) &&
                  verb::bakeDeserializeTables(static_cast<const uint8_t*>(blob.getData()), blob.getSize(), tables) &&
                  !tables.empty() && !tables.front().path.yawMatch.free,
                  "F3 reads the pre-F3 VRBK v2 without Manual flags");
        } else {
            check(version == 1, "F3 fixture carries an older-version optional cache");
        }
        auto processor = std::make_unique<VerbProcessor>();
        auto& p = *processor;
        p.setPlayConfigDetails(2, 2, 48000, 128);
        p.prepareToPlay(48000, 128);
        p.setKeepRoutingEnabled(false);
        juce::String why;
        const bool loaded = p.loadRoomPresetText(contents, why) && waitReady(p, 30);
        check(loaded, "F3 loads the pre-F3 preset");
        if (loaded && version == 2) {
            check(!tables.empty() && p4test::waitBaked(p, 10) &&
                  E9bAccess::engine(p)->bakeStats().hash == tables.front().hash(),
                  "F3 restores the pre-F3 Along path bake without retracing");
        } else if (loaded) {
            const auto impulse = p4test::impulse(p, "lpath", .5);
            double energy = 0.;
            for (float sample : impulse) energy += (double)sample * sample;
            std::printf("  F3 legacy VRBK version=%d impulse energy=%.9g\n", version, energy);
            check(std::isfinite(energy) && energy > 1e-9 && E9bAccess::engine(p)->bakeStats().hash == 0,
                  "F3 legacy preset plays with the v1 cache skipped");
        }
    }
    return fails == before ? 0 : 1;
}
