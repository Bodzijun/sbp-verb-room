// E17: independent joint-state recording, deterministic bake, state lookup, off-pass fallback, offline render, and bake-time scaling.

namespace {

static std::pair<double, double> e17ClickMetrics(const std::vector<std::vector<float>>& outs, double sr) {
    double hfMax = -300.0, jumpMax = 0.0;
    for (const auto& x : outs) {
        struct Bq { double b0, b1, b2, a1, a2, x1, x2, y1, y2; };
        std::vector<Bq> f;
        for (double q : {0.5098, 0.6013, 0.9000, 2.5629}) {
            const double w = 2.0 * kPi * 4000.0 / sr;
            const double al = std::sin(w) / (2.0 * q);
            const double cw = std::cos(w);
            const double a0 = 1.0 + al;
            f.push_back(Bq{(1.0 + cw) / 2.0 / a0, -(1.0 + cw) / a0, (1.0 + cw) / 2.0 / a0, -2.0 * cw / a0, (1.0 - al) / a0, 0, 0, 0, 0});
        }
        const size_t W = 64, n = x.size();
        double hf = 0.0, tot = 0.0;
        for (size_t i = 0; i < n; ++i) {
            double y = x[i];
            for (auto& b : f) {
                const double v = b.b0 * y + b.b1 * b.x1 + b.b2 * b.x2 - b.a1 * b.y1 - b.a2 * b.y2;
                b.x2 = b.x1; b.x1 = y; b.y2 = b.y1; b.y1 = v; y = v;
            }
            hf += y * y;
            tot += (double)x[i] * x[i];
            if ((i + 1) % W == 0) {
                if (i / W >= 16 && tot > 1e-18)
                    hfMax = std::max(hfMax, 10.0 * std::log10((hf + 1e-30) / (tot + 1e-30)));
                hf = tot = 0.0;
            }
        }
        for (size_t w0 = 16 * W; w0 + W <= n; w0 += W) {
            double A = 0.0;
            for (size_t i = (w0 >= 256 ? w0 - 256 : 0); i < std::min(n, w0 + W + 256); ++i)
                A = std::max(A, (double)std::fabs(x[i]));
            for (size_t i = w0; i < w0 + W; ++i)
                jumpMax = std::max(jumpMax, std::fabs((double)x[i] - x[i - 1]) / (2.0 * kPi * 997.0 / sr * std::max(A, 1e-6)));
        }
    }
    return {hfMax, jumpMax};
}

static Scene e17MakeScene(int nSrc, bool openContour, bool withDir) {
    Scene sc = builtinSceneNamed("Studio");
    sc.clutter.count = 0;
    sc.roughness = 0.0;
    sc.sources.resize((size_t)nSrc);
    for (int s = 0; s < nSrc; ++s) {
        sc.sources[(size_t)s] = sc.sources[0];
        sc.sources[(size_t)s].name = "S" + std::to_string(s);
        sc.sources[(size_t)s].pos = {1.3 + 1.4 * s, 1.4, 1.4};
        sc.sources[(size_t)s].azimuthDeg = 10.0 - 40.0 * s;
    }
    if (withDir) {
        const auto& dirs = builtinDirectivities();
        if (!dirs.empty()) {
            const auto& prof = dirs[std::min<size_t>(1, dirs.size() - 1)];
            sc.sources[0].instrument = prof->id;
            sc.sources[0].directivity = prof;
            sc.sources[0].instrumentAmount = 1.0;
        }
    }
    sc = withListenerAt(sc, {2.0, 2.2, 1.5});
    sc.listenerYaw = 0.0;
    if (openContour) {
        if (!sc.walls.empty()) sc.walls.pop_back();
        sc.ceilMat = -1;
    }
    return sc;
}

} // namespace

static void testE17Pass() {
    std::printf("E17: recorded-pass bake, state lookup, off-pass fallback, offline parity & timing\n");

    // 1. Accuracy gate (48k rays full trace, both tail modes) across all required scene variants + E16 priority check
    struct Case { const char* name; int nSrc; bool open; bool dir; };
    const Case cases[] = {
        {"L+1src_unrelated", 1, false, false},
        {"L+2src_unrelated", 2, false, false},
        {"L+1src_instrument", 1, false, true},
        {"L+1src_open_contour", 1, true, false},
    };
    for (bool shared : {false, true}) {
        for (const auto& c : cases) {
            Scene sc = e17MakeScene(c.nSrc, c.open, c.dir);
            auto cfg = e4aConfig(shared);
            cfg.tailRays = 48000;
            cfg.adaptiveTail = true;
            cfg.rebuildWorkers = 0;
            Engine e(sc, cfg);
            const int S = e.numSources(), R = e.numReceivers();
            std::vector<BakedPath> declared = {
                makeLinearPath(-1, -1, {2.0, 2.2, 1.5}, {3.6, 3.0, 1.5}),
                makeLinearPath(0, -1, {1.3, 1.4, 1.4}, {2.8, 3.6, 1.4})
            };
            e.setBakedPaths(declared);
            check(e.bakeStats().state == Engine::BakeState::Blocked && e.bakeStats().waitReason == 10,
                  "E17 uncoupled declared config is Blocked before recording");

            std::vector<std::vector<float>> in((size_t)S, std::vector<float>(128, 0.01f)), out((size_t)R, std::vector<float>(128));
            std::vector<const float*> ip((size_t)S);
            std::vector<float*> op((size_t)R);
            for (int s = 0; s < S; ++s) ip[(size_t)s] = in[(size_t)s].data();
            for (int r = 0; r < R; ++r) op[(size_t)r] = out[(size_t)r].data();
            auto step = [&] { e.process(ip.data(), op.data(), 128); };

            auto setPose = [&](double t) {
                const double lx = 2.0 + 1.8 * t;
                const double ly = 2.2 + 0.9 * std::sin(kPi * t);
                e.setListener(lx, ly, 1.5, 25.0 * t);
                const double sx0 = 1.3 + 2.2 * (t * t);
                const double sy0 = 1.4 + 2.4 * t;
                e.setSourcePosition(0, sx0, sy0, 1.4);
                e.setSourceAzimuth(0, 15.0 + 45.0 * t);
                if (S >= 2) {
                    const double sx1 = 3.9 - 1.6 * std::sqrt(t);
                    const double sy1 = 3.8 - 1.9 * t;
                    e.setSourcePosition(1, sx1, sy1, 1.5);
                    e.setSourceAzimuth(1, -30.0 - 50.0 * t);
                }
            };

            // Verify aborted <2 sample recording leaves empty state
            e.armRecordedPass();
            e.recordPassAudio(false);
            e.stopRecordedPass();
            e.recordPassAudio(false);
            check(e.recordedPassStats().state == 0 && e.recordedPassStats().samples == 0,
                  "E17 stopping before 2 samples clears passState cleanly");

            // Record realistic 10 s pass (multi-metre movement)
            e10Allocations = 0;
            e.armRecordedPass();
            e10Audio = true; e.recordPassAudio(false); e10Audio = false;
            check(e.recordedPassStats().state == 1, "E17 armed waits for host playing");
            const int blocks = 10 * 375;
            for (int i = 0; i <= blocks; ++i) {
                setPose((double)i / blocks);
                e10Audio = true;
                e.recordPassAudio(true);
                e.process(ip.data(), op.data(), 128);
                e10Audio = false;
            }
            e10Audio = true; e.recordPassAudio(false); e10Audio = false;
            check(e.recordedPassStats().state == 3 && e.recordedPassStats().samples > 100,
                  "E17 decimated multi-metre pass captured and stopped cleanly");

            // Also works without ANY declared MovementPaths (DAW automation case)
            e.setBakedPaths({});
            check(e.bakeStats().state == Engine::BakeState::NotBaked && e.bakeStats().percent == 0,
                  "E17 DAW-automation recorded pass does not auto-bake");

            auto bake = [&](unsigned workers) {
                e.requestPathBake(workers);
                const auto lim = std::chrono::steady_clock::now() + std::chrono::seconds(180);
                while (e.bakeStats().percent < 100 && std::chrono::steady_clock::now() < lim) {
                    step();
                    e.runWorkerStep();
                }
                return e.bakedTableSnapshot();
            };
            auto t = bake(0);
            check(t && t->path.recorded && t->path.group == 2000 && e.bakeStats().percent == 100,
                  (std::string("E17 ") + c.name + " bake completes").c_str());
            if (!t) continue;

            const auto modeled = *e.sceneSnapshot();
            double worstErr = 0.0, worstAppliedErr = 0.0;
            for (double phase : {0.137, 0.319, 0.541, 0.723, 0.887}) {
                std::vector<double> joint(4 * ((size_t)S + 1)), pred(t->stride()), ref(t->stride());
                for (const auto& p : t->movingPaths) {
                    const auto v = p.at(phase);
                    for (int k = 0; k < 4; ++k) joint[4 * (size_t)(p.object + 1) + (size_t)k] = v[(size_t)k];
                    if (p.object < 0) e.setListener(v[0], v[1], v[2], v[3]);
                    else { e.setSourcePosition(p.object, v[0], v[1], v[2]); e.setSourceAzimuth(p.object, v[3]); }
                }
                double u = -1.0;
                check(t->projectJoint(joint.data(), u) && std::abs(u - phase) < 1e-6, "E17 projection recovers state parameter");
                check(t->lookup(u, pred.data()), "E17 lookup succeeds");
                const auto rc = e16ReceiversAt(modeled, {joint[0], joint[1], joint[2], joint[3]});
                for (int s = 0; s < S; ++s) {
                    auto src = modeled.sources[(size_t)s];
                    src.pos = {joint[4 + 4 * s], joint[5 + 4 * s], joint[6 + 4 * s]};
                    src.azimuthDeg = joint[7 + 4 * s];
                    const auto en = traceTailEnergy(modeled, src, rc, cfg.erOrder, 48000, modeled.seed + 131 * s);
                    for (int r = 0; r < R; ++r)
                        for (int b = 0; b < NB; ++b)
                            ref[((size_t)s * (size_t)R + (size_t)r) * NB + (size_t)b] = en[(size_t)r][(size_t)b];
                }
                worstErr = std::max(worstErr, e16MaxErrDb(pred, ref, S, R));

                e.notifyJump();
                const auto lookupsBefore = e.bakeStats().lookups;
                for (int k = 0; k < 600 && e.motionStats().transState != 0; ++k) step();
                for (int k = 0; k < 5; ++k) step();
                check(e.bakeStats().lookups > lookupsBefore && !e.recordedPassStats().offPass, "E17 RT process() uses baked table on pass");

                if (shared) {
                    const auto applied = e.lateAppliedEnergy();
                    if (!applied.empty()) {
                        std::vector<double> appSRB(t->stride());
                        for (int r = 0; r < R; ++r)
                            for (int s = 0; s < S; ++s)
                                for (int b = 0; b < NB; ++b)
                                    appSRB[((size_t)s * (size_t)R + (size_t)r) * NB + (size_t)b] =
                                        applied[((size_t)r * (size_t)S + (size_t)s) * NB + (size_t)b];
                        const double appErr = e16MaxErrDb(appSRB, pred, S, R);
                        if (appErr > 0.1) std::printf("  [diag] %s phase=%.3f appErr=%.4f transState=%d\n", c.name, phase, appErr, e.motionStats().transState);
                        worstAppliedErr = std::max(worstAppliedErr, appErr);
                    }
                }
            }
            std::printf("  E17_ACCURACY %-20s shared=%d nodes=%3zu ms=%7.1f traceErrDb=%.4f\n",
                        c.name, (int)shared, t->u.size(), e.bakeStats().milliseconds, worstErr);
            check(worstErr <= 1.0, (std::string("E17 ") + c.name + " <= 1 dB vs 48k trace").c_str());
            if (shared) check(worstAppliedErr <= 0.1, (std::string("E17 ") + c.name + " shared lateAppliedEnergy matches table").c_str());

            // Check E16 priority (Brief item 4): when E16-allowed declared paths exist, E16 wins over recorded pass;
            // when declared is coupled group + extra uncoupled source (Blocked), recorded pass takes over.
            if (!shared && c.nSrc == 1 && !c.open && !c.dir) {
                const BakedPath s0Only = makeLinearPath(0, -1, {1.3, 1.4, 1.4}, {2.8, 3.6, 1.4});
                e.setBakedPaths({s0Only});
                auto tablesPrio = e.bakedTablesSnapshot();
                const auto statsPrio = e.bakeStats();
                check(!statsPrio.groups.empty() && statsPrio.groups[0].group != 2000,
                      "E17 E16 single-source path keeps priority over recorded pass");
                const std::vector<BakedPath> coupled = {
                    makeLinearPath(-1, 0, {2.0, 2.2, 1.5}, {3.6, 3.0, 1.5}),
                    makeLinearPath(0, 0, {1.3, 1.4, 1.4}, {2.8, 3.6, 1.4})
                };
                e.setBakedPaths(coupled);
                const auto cStats = e.bakeStats();
                check(!cStats.groups.empty() && cStats.groups[0].group == 0 && cStats.groups[0].movesListener && cStats.groups[0].group != 2000,
                      "E17 E16 coupled group keeps priority over recorded pass");
            } else if (shared && c.nSrc == 2) {
                const std::vector<BakedPath> coupledPlusExtra = {
                    makeLinearPath(-1, 0, {2.0, 2.2, 1.5}, {3.6, 3.0, 1.5}),
                    makeLinearPath(0, 0, {1.3, 1.4, 1.4}, {2.8, 3.6, 1.4}),
                    makeLinearPath(1, -1, {3.9, 3.8, 1.5}, {2.3, 1.9, 1.5})
                };
                e.setBakedPaths(coupledPlusExtra);
                check(!e.bakeStats().groups.empty() && e.bakeStats().groups[0].group == 2000,
                      "E17 coupled group + extra uncoupled source uses recorded pass");
            }

            check(e10Allocations == 0, "E17 zero audio allocations during record and lookup checks");
        }
    }

    // 2. Worker-count bit identity (1 vs 27 workers) + export/import round-trip identity
    {
        Scene sc = e17MakeScene(2, false, false);
        auto c1 = e4aConfig(true); c1.tailRays = 12000; c1.adaptiveTail = true; c1.rebuildWorkers = 1;
        auto cN = c1; cN.rebuildWorkers = 27;
        Engine a(sc, c1), b(sc, cN);
        std::vector<std::vector<float>> in(2, std::vector<float>(128, 0.01f)), out((size_t)a.numReceivers(), std::vector<float>(128));
        std::vector<const float*> ip = {in[0].data(), in[1].data()};
        std::vector<float*> op; for (auto& v : out) op.push_back(v.data());
        a.armRecordedPass(); a.recordPassAudio(false);
        for (int i = 0; i <= 1500; ++i) {
            const double u = (double)i / 1500.0;
            a.setListener(2.0 + 1.5 * u, 2.2 + 0.8 * std::sin(kPi * u), 1.5, 20.0 * u);
            a.setSourcePosition(0, 1.3 + 2.0 * u, 1.4 + 1.6 * u, 1.4);
            a.setSourcePosition(1, 3.8 - 1.2 * u, 3.5 - 1.5 * u, 1.5);
            a.recordPassAudio(true);
            a.process(ip.data(), op.data(), 128);
        }
        a.recordPassAudio(false);
        const auto exported = a.exportRecordedPass();
        check(exported.samples > 50 && exported.objects == 3 && b.importRecordedPass(exported),
              "E17 exportRecordedPass / importRecordedPass round-trips cleanly");
        check(b.bakeStats().state == Engine::BakeState::NotBaked && b.bakeStats().percent == 0,
              "E17 importRecordedPass does not auto-bake");
        a.setBakedPaths({}); b.setBakedPaths({});
        a.requestPathBake(1); b.requestPathBake(27);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while ((a.bakeStats().percent < 100 || b.bakeStats().percent < 100) && std::chrono::steady_clock::now() < deadline) {
            a.process(ip.data(), op.data(), 128); a.runWorkerStep();
            b.process(ip.data(), op.data(), 128); b.runWorkerStep();
        }
        check(a.bakeStats().percent == 100 && b.bakeStats().percent == 100 && a.bakeStats().hash != 0 && a.bakeStats().hash == b.bakeStats().hash,
              "E17 worker-count bit identity (1 vs 27 workers)");
    }

    // 3. Playback at 2x speed, backwards, looped, and self-crossing curve + smooth off-pass click check + offline render
    {
        Scene sc = e17MakeScene(1, false, false);
        auto cfg = e4aConfig(true);
        cfg.tailRays = 12000;
        cfg.adaptiveTail = true;
        Engine e(sc, cfg);
        const int R = e.numReceivers();
        const double sr = 48000.0;
        std::vector<float> in(128, 0.f);
        std::vector<std::vector<float>> out((size_t)R, std::vector<float>(128));
        const float* ip[1] = {in.data()};
        std::vector<float*> op; for (auto& v : out) op.push_back(v.data());
        uint64_t frame = 0;
        auto fillTone = [&] {
            for (int i = 0; i < 128; ++i) in[(size_t)i] = 0.05f * (float)std::sin(2.0 * kPi * 997.0 * (double)(frame + (uint64_t)i) / sr);
            frame += 128;
        };
        // Figure-8 self-crossing curve (t=0.25 and t=0.75 visit the exact centre crossing!)
        auto fig8 = [&](double t) {
            const double ang = 2.0 * kPi * t;
            const double lx = 2.8 + 1.0 * std::sin(ang);
            const double ly = 2.6 + 0.7 * std::sin(2.0 * ang);
            const double sx = 2.4 + 0.9 * std::sin(ang + 0.4);
            const double sy = 2.6 + 0.6 * std::sin(2.0 * ang + 0.8);
            e.setListener(lx, ly, 1.5, 0.0);
            e.setSourcePosition(0, sx, sy, 1.4);
        };
        e.armRecordedPass(); e.recordPassAudio(false);
        for (int i = 0; i <= 3000; ++i) {
            fig8((double)i / 3000.0);
            e.recordPassAudio(true);
            fillTone();
            e.process(ip, op.data(), 128);
        }
        e.recordPassAudio(false);
        e.setBakedPaths({});
        e.requestPathBake(4);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (e.bakeStats().percent < 100 && std::chrono::steady_clock::now() < deadline) {
            fillTone(); e.process(ip, op.data(), 128); e.runWorkerStep();
        }
        auto tbl = e.bakedTableSnapshot();
        check(tbl && e.bakeStats().percent == 100, "E17 self-crossing curve bakes cleanly");

        // Compare real-process applied energy at the same phase u0=0.37 during forward 2x, backwards, and looped playback!
        auto captureAppliedAt = [&](auto trajectory, double uSample) {
            for (int step = 0; step < 80; ++step) {
                const double t = trajectory((double)step / 79.0);
                fig8(t);
                fillTone();
                e.process(ip, op.data(), 128);
                if (std::abs(t - uSample) < 0.015) break;
            }
            fig8(uSample);
            e.notifyJump();
            for (int k = 0; k < 4; ++k) { fillTone(); e.process(ip, op.data(), 128); }
            check(!e.recordedPassStats().offPass, "E17 on curve during playback mode");
            return e.lateAppliedEnergy();
        };
        const auto appFwd2x = captureAppliedAt([](double s) { return std::min(1.0, 2.0 * s); }, 0.37);
        const auto appBack = captureAppliedAt([](double s) { return 1.0 - s; }, 0.37);
        const auto appLoop = captureAppliedAt([](double s) { return std::fmod(2.5 * s, 1.0); }, 0.37);
        check(!appFwd2x.empty() && appFwd2x == appBack && appFwd2x == appLoop,
              "E17 real process() at 2x speed, backwards, and looped yields bit-identical lateAppliedEnergy at same state");

        // Self-crossing ambiguity check: jump straight from u=0.25 approach to u=0.75 approach through the crossing
        fig8(0.75); e.notifyJump(); fillTone(); e.process(ip, op.data(), 128);
        check(!e.recordedPassStats().offPass, "E17 self-crossing point resolves without dropping off-pass");

        // Off-pass transition + return with 997 Hz tone and click gate (HF residual < -40 dB, jump ratio < 2.0)
        fig8(0.2); e.notifyJump();
        for (int k = 0; k < 80; ++k) { fillTone(); e.process(ip, op.data(), 128); }
        std::vector<std::vector<float>> clickRec((size_t)R);
        bool sawOff = false, sawReturn = false;
        e.setSoloMask(4);
        for (int k = 0; k < 400; ++k) {
            // Walk smoothly off curve in [100..220], stay off in [150..220], return smoothly onto curve in [220..300]
            double dev = 0.0;
            if (k >= 100 && k < 160) dev = 0.65 * (double)(k - 100) / 60.0;
            else if (k >= 160 && k < 240) dev = 0.65;
            else if (k >= 240 && k < 300) dev = 0.65 * (1.0 - (double)(k - 240) / 60.0);
            const double ang = 2.0 * kPi * 0.2;
            e.setListener(2.8 + 1.0 * std::sin(ang) + dev, 2.6 + 0.7 * std::sin(2.0 * ang), 1.5, 0.0);
            e.setSourcePosition(0, 2.4 + 0.9 * std::sin(ang + 0.4), 2.6 + 0.6 * std::sin(2.0 * ang + 0.8), 1.4);
            fillTone();
            e.process(ip, op.data(), 128);
            if ((k % 4) == 3) e.runWorkerStep();
            if (dev > 0.5) sawOff |= e.recordedPassStats().offPass;
            if (k > 330) sawReturn |= !e.recordedPassStats().offPass;
            for (int r = 0; r < R; ++r) clickRec[(size_t)r].insert(clickRec[(size_t)r].end(), out[(size_t)r].begin(), out[(size_t)r].end());
        }
        e.setSoloMask(0);
        const auto [hfMax, jumpMax] = e17ClickMetrics(clickRec, sr);
        std::printf("  E17 off-pass click check: sawOff=%d sawReturn=%d hfMax=%.1f dB jumpRatio=%.2f\n",
                    (int)sawOff, (int)sawReturn, hfMax, jumpMax);
        check(sawOff && sawReturn, "E17 smooth excursion goes off-pass to motion estimator and resumes table on return");
        check(hfMax < -40.0 && jumpMax < 2.0, "E17 off-pass fallback and return produce zero audible clicks (HF<-40 dB, jumpRatio<2)");

        // Offline render (faster than real time, zero sleep between process blocks): table lookup must apply continuously!
        EngineConfig offCfg = cfg;
        offCfg.tailThread = true;
        offCfg.manualWorker = false;
        Engine off(sc, offCfg);
        off.importRecordedPass(e.exportRecordedPass());
        off.setBakedPaths({});
        off.requestPathBake(4);
        const auto offDead = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (off.bakeStats().percent < 100 && std::chrono::steady_clock::now() < offDead)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(off.bakeStats().percent == 100, "E17 offline engine bakes imported pass");
        const auto lookups0 = off.bakeStats().lookups;
        const auto tRender0 = std::chrono::steady_clock::now();
        for (int i = 0; i <= 200; ++i) {
            const double t = 0.25 * (double)i / 200.0;
            const double ang = 2.0 * kPi * t;
            off.setListener(2.8 + 1.0 * std::sin(ang), 2.6 + 0.7 * std::sin(2.0 * ang), 1.5, 0.0);
            off.setSourcePosition(0, 2.4 + 0.9 * std::sin(ang + 0.4), 2.6 + 0.6 * std::sin(2.0 * ang + 0.8), 1.4);
            if (i == 0) off.notifyJump();
            fillTone();
            off.process(ip, op.data(), 128);
        }
        const double renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tRender0).count();
        check(renderMs < 200.0 && off.bakeStats().lookups > lookups0 + 150 && !off.recordedPassStats().offPass,
              "E17 offline faster-than-real-time render uses baked recorded table continuously");
    }

    // 4. Real background-worker zero-allocation check (recording + baking + lookup concurrent with E10Audio)
    {
        Scene sc = e17MakeScene(1, false, false);
        EngineConfig cfg = e4aConfig(true);
        cfg.manualWorker = false;
        cfg.tailRays = 12000;
        cfg.adaptiveTail = true;
        Engine eng(sc, cfg);
        E10Audio audio(eng);
        e10Allocations = 0;
        eng.armRecordedPass();
        for (int i = 0; i < 60; ++i) {
            const double u = (double)i / 59.0;
            eng.setListener(2.0 + 1.5 * u, 2.2 + 0.6 * u, 1.5, 10.0 * u);
            eng.setSourcePosition(0, 1.3 + 1.8 * u, 1.4 + 1.2 * u, 1.4);
            e10Audio = true; eng.recordPassAudio(true); e10Audio = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        e10Audio = true; eng.recordPassAudio(false); e10Audio = false;
        eng.setBakedPaths({});
        eng.requestPathBake();
        const bool ok = audio.until([&] { return eng.bakeStats().percent == 100; }, 90.0);
        check(ok && std::isfinite(audio.peak) && audio.peak > 1e-8 && e10Allocations == 0,
              "E17 real-time E10Audio allocates zero memory during concurrent recording, baking, and lookup");

        // Invalidations: directivity pattern, quality, scene geometry, re-recording, cancel
        eng.setSourcePattern(0, 0.75);
        const auto d1 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (eng.bakeStats().state != Engine::BakeState::Outdated && std::chrono::steady_clock::now() < d1)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(eng.bakeStats().state == Engine::BakeState::Outdated, "E17 source pattern change outdates recorded bake");

        eng.armRecordedPass();
        check(eng.bakeStats().state == Engine::BakeState::Outdated, "E17 re-recording keeps table outdated until rebaked");
        eng.stopRecordedPass();
        eng.cancelPathBake();
        const auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (eng.bakeStats().state != Engine::BakeState::Cancelled && std::chrono::steady_clock::now() < d2)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(eng.bakeStats().state == Engine::BakeState::Cancelled, "E17 cancel sets Cancelled cleanly");
    }

    // 5. Bake-time scaling table for 10 s, 30 s, and 60 s passes (real pool workers, shared=true, 48000 rays adaptive)
    {
        std::printf("  [E17_TIME] Recorded-pass bake scaling (Studio L+1src, shared=true, 48000 rays, adaptive, real pool):\n");
        for (int durationSec : {10, 30, 60}) {
            Scene sc = e17MakeScene(1, false, false);
            EngineConfig cfg = e4aConfig(true);
            cfg.manualWorker = false;
            cfg.tailRays = 48000;
            cfg.adaptiveTail = true;
            Engine eng(sc, cfg);
            std::vector<float> in(128, 0.01f);
            std::vector<std::vector<float>> out((size_t)eng.numReceivers(), std::vector<float>(128));
            const float* ip[1] = {in.data()};
            std::vector<float*> op; for (auto& v : out) op.push_back(v.data());
            const double spanM = 1.6 + 0.08 * durationSec;
            const int blocks = durationSec * 375;
            eng.armRecordedPass();
            eng.recordPassAudio(false);
            for (int i = 0; i <= blocks; ++i) {
                const double t = (double)i / blocks;
                eng.setListener(1.6 + spanM * 0.5 * t, 2.0 + 0.8 * std::sin(kPi * t), 1.5, 25.0 * t);
                eng.setSourcePosition(0, 1.2 + spanM * 0.55 * t, 1.4 + spanM * 0.35 * t, 1.4);
                eng.setSourceAzimuth(0, 10.0 + 40.0 * t);
                eng.recordPassAudio(true);
                eng.process(ip, op.data(), 128);
            }
            eng.recordPassAudio(false);
            const int passSamples = eng.recordedPassStats().samples;
            eng.setBakedPaths({});
            eng.requestPathBake();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
            while (eng.bakeStats().percent < 100 && std::chrono::steady_clock::now() < deadline) {
                eng.process(ip, op.data(), 128);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const auto st = eng.bakeStats();
            const auto tbl = eng.bakedTableSnapshot();
            const size_t nodes = tbl ? tbl->u.size() : 0;
            std::printf("    E17_TIME duration=%2ds  passSamples=%4d  stations=%4llu  nodes=%3zu  ms=%7.1f  workers=%d\n",
                        durationSec, passSamples, (unsigned long long)st.stations, nodes, st.milliseconds, st.workers);
            check(st.percent == 100 && nodes >= 3 && passSamples >= 50, "E17 timing pass completed");
        }
    }
}
