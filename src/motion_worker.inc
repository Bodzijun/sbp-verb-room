    // Called by the same coordinator as the legacy worker. The reference is
    // the authored, wall-resolved pose, so render speed and transport do not
    // change the pose or seed of a photo that is still being prepared.
    bool motionWorkerStep() {
        const auto request = motionRequest.load(std::memory_order_acquire);
        if (!(request & 1)) {
            if (motionPending.load(std::memory_order_acquire) >= 0) return true;
            if (motionWorkerWasOn) {
                sLastGeom = 0;
                for (auto& source : srcs) source->lastGeom = 0;
                anchorValid = false;
                motionWorkerWasOn = false;
            }
            return false;
        }
        if (!motionWorkerWasOn) {
            const auto action = bakeAction.load(std::memory_order_acquire);
            stopBakeTasks();
            if (bakeRequested.load(std::memory_order_acquire))
                endBake(BakeState::Cancelled, 0, action);
        }
        motionWorkerWasOn = true;
        if (motionPending.load(std::memory_order_acquire) >= 0 || transState.load(std::memory_order_acquire) != 0)
            return true;
        if (shared && lateGseenPub.load(std::memory_order_acquire) != lateGserial.load(std::memory_order_acquire))
            return true;
        const int count = shared ? R : S;
        for (int i = 0; i < count; ++i) {
            const auto& t = motionConv(i);
            if (t.pending.load(std::memory_order_acquire) >= 0 ||
                t.publishedHandover.load(std::memory_order_acquire) >= 0) return true;
        }

        std::shared_ptr<World> world;
        {
            std::lock_guard<std::mutex> lock(worldMx);
            world = latest;
        }
        if (poseScaleReady.load(std::memory_order_acquire) != world->constructionPoseScale) return true;
        const int current = motionPublished.load(std::memory_order_acquire);
        if (current >= 0) {
            const auto& photo = motionPhotos[current];
            if (photo.request == request && photo.scene == world->serial && photo.quality == workerQuality) return true;
        }
        const auto cancelled = [&] {
            return !running || motionRequest.load(std::memory_order_acquire) != request ||
                geomSerial.load(std::memory_order_acquire) != world->serial ||
                qualitySerial.load(std::memory_order_acquire) != workerQuality;
        };
        if (cancelled()) return true;

        const int target = current == 0 ? 1 : 0;
        MotionPhoto photo;
        photo.request = request;
        photo.scene = world->serial;
        photo.quality = workerQuality;
        try {
            const auto& scene = world->sc;
            Vec3 listener = scene.listenerPos;
            if (!scene.hasListener) {
                listener = {};
                for (const auto& receiver : scene.receivers) listener = listener + receiver.pos;
                listener = listener * (1. / R);
            }
            double lst[4] = {listener.x, listener.y, listener.z, scene.hasListener ? scene.listenerYaw : yaw0};
            SurfaceSideHist listenerSide;
            resolvePose4(world->geo, world->serial, listenerSide, lst, lst);
            photo.listener = {lst[0], lst[1], lst[2]};
            photo.sources = scene.sources;
            std::vector<std::array<double, 4>> poses((size_t)S);
            for (int s = 0; s < S; ++s) {
                auto& source = photo.sources[(size_t)s];
                auto& pose = poses[(size_t)s];
                pose = {source.pos.x, source.pos.y, source.pos.z, source.azimuthDeg};
                SurfaceSideHist side;
                resolvePose4(world->geo, world->serial, side, pose.data(), pose.data());
                source.pos = {pose[0], pose[1], pose[2]};
            }
            std::vector<Transducer> receivers;
            resolvedReceivers(world->geo, lst, receivers, !shared);
            if (!workerGrid || workerGridScene != world->serial || workerGridQuality != workerTraceQuality ||
                workerGrid->channels() != std::clamp(R, 1, kMaxMotionChannels)) {
                Vec3 emitterAnchor = photo.sources.empty() ? photo.listener : photo.sources.front().pos;
                if (shared && photo.sources.size() > 1) {
                    Vec3 sum{};
                    for (const auto& src : photo.sources) sum = sum + src.pos;
                    const Vec3 centroid = sum * (1. / static_cast<double>(photo.sources.size()));
                    if (isInsideEnclosure(world->geo, centroid)) {
                        emitterAnchor = centroid;
                    } else {
                        double bestDist = 1e30;
                        for (const auto& src : photo.sources) {
                            const double d = norm(src.pos - centroid);
                            if (d < bestDist) { bestDist = d; emitterAnchor = src.pos; }
                        }
                    }
                }
                auto grid = std::make_shared<MotionGrid>(world->geo, emitterAnchor, photo.listener,
                    cfg.erOrder, std::clamp(cfg.tailRays, 4000, 16000), tailExecution(cancelled), receivers);
                if (cancelled()) return true;
                workerGrid = std::move(grid);
                workerGridScene = world->serial;
                workerGridQuality = workerTraceQuality;
            }
            photo.grid = workerGrid;
            photo.listenerChannelEnvelopes.resize((size_t)R);
            photo.listenerEnvelope = photo.grid->listenerFieldAndChannels(
                photo.listener, photo.listenerChannelEnvelopes.data(), R);
            photo.envelopes.reserve((size_t)S);
            photo.channelEnvelopes.resize((size_t)S * (size_t)R);
            for (int s = 0; s < S; ++s) {
                const auto& source = photo.sources[(size_t)s];
                MotionEnvelope* chBase = &photo.channelEnvelopes[(size_t)s * (size_t)R];
                auto envelope = photo.grid->sourceFieldAndChannels(source.pos, chBase, R);
                photo.grid->applyCloseness(envelope, source, photo.listener, chBase, R);
                photo.envelopes.push_back(envelope);
            }
            photo.length = settledTailLength(*world, lst, receivers, cancelled, &poses);
            if (shared) {
                if (!sharedRebuild(false, nullptr, cancelled, false, 0, &photo)) return true;
            } else {
                Scene sceneAtPhoto = scene;
                sceneAtPhoto.length = photo.length;
                for (int s = 0; s < S; ++s) {
                    const auto& t = motionConv(s);
                    const int slot = (t.curAtomic.load(std::memory_order_acquire) + 1) % 3;
                    buildTail(s, sceneAtPhoto, photo.sources[(size_t)s], receivers, slot, world->serial,
                              cancelled, false, false, true);
                    photo.slots.push_back(slot);
                }
            }
            if (cancelled()) return true;
            photo.photoWinEnergy.assign((size_t)S * (size_t)R, MotionEnvelope{});
            if (!shared) {
                for (int s = 0; s < S; ++s) {
                    const auto& t = motionConv(s);
                    const auto& slot = t.slot[photo.slots[(size_t)s]];
                    if (slot.bakeWinEnergy.size() >= (size_t)kBakeWinCount * (size_t)R * NB) {
                        for (int r = 0; r < R; ++r)
                            for (int w = 0; w < kBakeWinCount; ++w)
                                for (int b = 0; b < NB; ++b)
                                    photo.photoWinEnergy[(size_t)s * (size_t)R + (size_t)r][w][b] =
                                        static_cast<float>(slot.bakeWinEnergy[((size_t)w * (size_t)R + (size_t)r) * NB + b]);
                    }
                }
            } else if (srcHist.size() >= (size_t)S) {
                for (int s = 0; s < S; ++s) {
                    const auto& h = srcHist[(size_t)s];
                    if ((int)h.h.size() < R) continue;
                    for (int r = 0; r < R; ++r)
                        for (int b = 0; b < NB; ++b) {
                            const auto& bins = h.h[(size_t)r][(size_t)b];
                            for (size_t k = 0; k < bins.size(); ++k) {
                                const double tSec = ((double)k + 0.5) * h.binDur;
                                int w = 0;
                                while (w + 1 < kBakeWinCount && tSec >= kBakeWinEndSec[w]) ++w;
                                photo.photoWinEnergy[(size_t)s * (size_t)R + (size_t)r][w][b] += static_cast<float>(bins[k]);
                            }
                        }
                }
            }
            std::lock_guard<std::mutex> lock(irSnapshotMx);
            for (int i = 0; i < count; ++i) {
                auto& t = motionConv(i);
                if (shared) t.bakeSource = i;
                auto& slot = t.slot[photo.slots[(size_t)i]];
                const size_t scratch = (size_t)t.R * kBakeWinCount * t.bins;
                if (t.motionPreRe.empty()) {
                    t.motionPreRe.resize(scratch);
                    t.motionPreIm.resize(scratch);
                }
                if (shared) {
                    slot.bakeEqGain.assign(1, {});
                    for (auto& window : slot.bakeEqGain.front()) window.fill(1.f);
                    slot.bakeEqLp.assign(1, {});
                    slot.bakeEqSeen.assign(1, ~0ull);
                }
                std::fill(t.freshPrev.begin(), t.freshPrev.end(), 0.f);
                std::fill(t.freshIn.begin(), t.freshIn.end(), 0.f);
                std::fill(t.freshNz.begin(), t.freshNz.end(), uint8_t(0));
                handoverCorrelation(t, t.slot[t.curAtomic.load(std::memory_order_acquire)], slot);
            }
            if (cancelled()) return true;
            motionPhotos[target] = std::move(photo);
            motionPending.store(target, std::memory_order_release);
        } catch (const std::runtime_error&) {
            // A cancelled trace never publishes its partially written slots.
            ++stCancelled;
        }
        return true;
    }
