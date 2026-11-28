    TailConv& motionConv(int i) const noexcept {
        return shared ? *sconv[(size_t)i] : srcs[(size_t)i]->tail;
    }

    void motionBeginPeriod(uint64_t request) noexcept {
        const bool enabled = (request & 1) || motionIncoming >= 0;
        if (motionAudio != enabled) motionNeutralUntil = gcount + B;
        motionAudio = enabled;
        if (!motionAudio && shared && motionActive >= 0) motionStoreSharedFeed(motionActive);
        if (!motionAudio) motionActive = -1;
        if (motionIncoming >= 0) return;
        const int bank = motionPending.load(std::memory_order_acquire);
        if (bank < 0) return;
        const auto& photo = motionPhotos[bank];
        if (photo.request != request || photo.scene != geomSerial.load(std::memory_order_acquire) ||
            photo.quality != qualitySerial.load(std::memory_order_acquire)) {
            motionPending.store(-1, std::memory_order_release);
            return;
        }
        if (photo.scene != wd->serial) return;
        motionIncoming = bank;
        auto& gains = motionGains[bank];
        for (auto& envelope : gains.windows)
            for (auto& window : envelope) window.fill(1.f);
        for (auto& feed : gains.feed) feed.fill(1.f);
        for (auto& feed : gains.feedStart) feed.fill(1.f);
        for (auto& step : gains.feedStep) step.fill(0.f);
        for (int i = 0; i < (shared ? R : S); ++i) {
            auto& t = motionConv(i);
            t.hoSlot = photo.slots[(size_t)i];
            t.hoAge = 0;
            t.inputStart = gcount;
            t.inputFadeBlocks = std::max(1, static_cast<int>(std::lround(.15 * sr / B)));
            t.cAA = t.cBB = t.cAB = 0.;
            t.longN = t.longAt = 0;
            t.legacyHandover = false;
            t.publishedHandover.store(t.hoSlot, std::memory_order_release);
        }
        roomTailLength.store(photo.length, std::memory_order_relaxed);
        motionReadyScene.store(photo.scene, std::memory_order_relaxed);
        motionReadyQuality.store(photo.quality, std::memory_order_relaxed);
        motionReadyRequest.store(photo.request, std::memory_order_release);
        stTransStarted.fetch_add(1, std::memory_order_relaxed);
    }

    void motionFinishBlock() noexcept {
        if (motionIncoming < 0) return;
        for (int i = 0; i < (shared ? R : S); ++i)
            if (motionConv(i).hoSlot >= 0) return;
        motionActive = motionIncoming;
        motionIncoming = -1;
        if (shared) motionStoreSharedFeed(motionActive);
        motionPublished.store(motionActive, std::memory_order_release);
        motionPending.store(-1, std::memory_order_release);
        stTransDone.fetch_add(1, std::memory_order_relaxed);
    }

    void motionStoreSharedFeed(int bank) noexcept {
        const auto& base = motionPhotos[bank].feed;
        const auto& gains = motionGains[bank].feed;
        for (int r = 0; r < R; ++r)
            for (int s = 0; s < S; ++s) {
                const size_t rs = (size_t)r * (size_t)S + (size_t)s;
                const size_t gainIdx = gains.size() >= (size_t)R * (size_t)S ? rs : (size_t)s;
                for (int b = 0; b < NB; ++b) {
                    const size_t i = rs * 8 + b;
                    lateGuse[i] = lateGlocal[i] = base[i] * gains[gainIdx][b];
                }
            }
        lateGtarget = lateGlocal.data();
    }

    static float motionRatio(float now, float ref, float floor) noexcept {
        if (now == ref) return 1.f;
        return std::clamp((now + floor) / std::max(1e-20f, ref + floor), .126f, 3.98f);
    }

    float motionSlew(float now, float target) const noexcept {
        return std::clamp(target, now / motionMaxStep, now * motionMaxStep);
    }

    float motionBandFloor(const MotionEnvelope& envelope, int band) const noexcept {
        float peak = 0.f;
        for (const auto& window : envelope) peak = std::max(peak, window[band]);
        return (shared ? .01f : .04f) * peak;
    }

    static void motionComputeShares(const MotionEnvelope* chEnv, int nCh, bool isAmp,
                                    MotionEnvelope* outWinShare,
                                    std::array<std::array<float, NB>, kMaxMotionChannels>& outBandShare,
                                    std::array<float, kMaxMotionChannels>& outBroadShare) noexcept {
        const float unif = 1.f / static_cast<float>(std::max(1, nCh));
        if (nCh <= 1) {
            outBroadShare[0] = 1.f;
            outBandShare[0].fill(1.f);
            for (int w = 0; w < kBakeWinCount; ++w) outWinShare[0][w].fill(1.f);
            return;
        }
        std::array<float, kMaxMotionChannels> broadE{};
        float broadSum = 0.f;
        for (int r = 0; r < nCh; ++r) {
            float eR = 0.f;
            for (int w = 0; w < kBakeWinCount; ++w)
                for (int b = 0; b < NB; ++b) {
                    const float v = std::max(0.f, chEnv[r][w][b]);
                    eR += isAmp ? (v * v) : v;
                }
            broadE[(size_t)r] = eR;
            broadSum += eR;
        }
        for (int r = 0; r < nCh; ++r) {
            outBroadShare[(size_t)r] = broadSum > 1e-20f
                ? (broadE[(size_t)r] + 0.005f * broadSum * unif) / (1.005f * broadSum)
                : unif;
        }
        for (int b = 0; b < NB; ++b) {
            std::array<float, kMaxMotionChannels> bandE{};
            float bandSum = 0.f, peakWinSum = 0.f;
            std::array<float, kBakeWinCount> winSum{};
            for (int w = 0; w < kBakeWinCount; ++w) {
                float ws = 0.f;
                for (int r = 0; r < nCh; ++r) {
                    const float v = std::max(0.f, chEnv[r][w][b]);
                    const float e = isAmp ? (v * v) : v;
                    bandE[(size_t)r] += e;
                    ws += e;
                }
                winSum[(size_t)w] = ws;
                bandSum += ws;
                peakWinSum = std::max(peakWinSum, ws);
            }
            for (int r = 0; r < nCh; ++r) {
                outBandShare[(size_t)r][(size_t)b] = bandSum > 1e-20f
                    ? (bandE[(size_t)r] + 0.02f * bandSum * outBroadShare[(size_t)r]) / (1.02f * bandSum)
                    : outBroadShare[(size_t)r];
            }
            for (int w = 0; w < kBakeWinCount; ++w) {
                const float ws = winSum[(size_t)w];
                const float prior = 0.04f * ws + 0.015f * peakWinSum;
                const float denom = ws + prior;
                for (int r = 0; r < nCh; ++r) {
                    const float v = std::max(0.f, chEnv[r][w][b]);
                    const float e = isAmp ? (v * v) : v;
                    outWinShare[r][w][b] = denom > 1e-20f
                        ? (e + prior * outBandShare[(size_t)r][(size_t)b]) / denom
                        : outBandShare[(size_t)r][(size_t)b];
                }
            }
        }
    }

    void motionUpdateGains(int bank, Vec3 listener) noexcept {
        if (bank < 0) return;
        const auto& photo = motionPhotos[bank];
        auto& gains = motionGains[bank];
        if (shared) {
            std::copy(gains.feed.begin(), gains.feed.end(), gains.feedStart.begin());
            for (auto& step : gains.feedStep) step.fill(0.f);
        }
        if (photo.scene != wd->serial) return; // hold the old acoustic field while a new world prepares
        if (photo.envelopes.size() < (size_t)S || (shared && photo.feed.size() < (size_t)R * S * 8)) return;

        const int nCh = std::clamp(R, 1, kMaxMotionChannels);
        const bool hasChGrid = (photo.channelEnvelopes.size() >= (size_t)S * (size_t)R) &&
                               (photo.listenerChannelEnvelopes.size() >= (size_t)R);

        std::array<MotionEnvelope, kMaxMotionChannels> listenerNowCh{};
        const auto listenerField = photo.grid->listenerFieldAndChannels(listener, listenerNowCh.data(), nCh);
        MotionEnvelope listenerRatio{}, pPhoto{}, desiredScalar{}, fedScalar{};
        std::array<MotionEnvelope, kMaxMotionChannels> listenerChRatio{}, desiredCh{}, fedCh{};
        for (int b = 0; b < NB; ++b) {
            const float floor = motionBandFloor(photo.listenerEnvelope, b);
            for (int w = 0; w < kBakeWinCount; ++w)
                listenerRatio[w][b] = motionRatio(listenerField[w][b], photo.listenerEnvelope[w][b], floor);
        }
        const double listenerDisp = norm(listener - photo.listener);
        if (hasChGrid) {
            for (int r = 0; r < nCh; ++r) {
                const auto& lRef = photo.listenerChannelEnvelopes[(size_t)r];
                for (int b = 0; b < NB; ++b) {
                    const float floor = std::max(motionBandFloor(lRef, b), 0.1f * motionBandFloor(photo.listenerEnvelope, b));
                    for (int w = 0; w < kBakeWinCount; ++w) {
                        listenerChRatio[(size_t)r][w][b] = listenerDisp > 1e-4
                            ? motionRatio(listenerNowCh[(size_t)r][w][b], lRef[w][b], floor)
                            : 1.f;
                    }
                }
            }
        }

        float totalDrive = 0.f;
        if (shared) {
            for (float drive : motionDrive) totalDrive += drive;
            // Compute the shared IR's 4-window energy fraction pPhoto[w][b]
            // weighted by each source's traced photo anchor power G^2.
            for (int b = 0; b < NB; ++b) {
                float totalPhotoBand = 0.f;
                for (int s = 0; s < S; ++s) {
                    float g2 = 0.f;
                    for (int r = 0; r < R; ++r) {
                        const float g = photo.feed[((size_t)r * S + s) * 8 + b];
                        g2 += g * g;
                    }
                    g2 /= static_cast<float>(std::max(1, R));
                    const auto& refEnv = photo.envelopes[(size_t)s];
                    float gridSum = 0.f;
                    for (int w = 0; w < kBakeWinCount; ++w) gridSum += refEnv[w][b] * refEnv[w][b];
                    const float scale = gridSum > 1e-20f ? (g2 / gridSum) : 0.f;
                    for (int w = 0; w < kBakeWinCount; ++w) {
                        const float eWin = scale * refEnv[w][b] * refEnv[w][b];
                        pPhoto[w][b] += eWin;
                        totalPhotoBand += eWin;
                    }
                }
                if (totalPhotoBand > 1e-20f) {
                    for (int w = 0; w < kBakeWinCount; ++w) pPhoto[w][b] /= totalPhotoBand;
                } else {
                    for (int w = 0; w < kBakeWinCount; ++w) pPhoto[w][b] = 1.f / static_cast<float>(kBakeWinCount);
                }
            }
        }

        for (int s = 0; s < S; ++s) {
            const auto index = (size_t)s;
            const auto& anchor = photo.sources[index];
            Transducer source;
            source.pos = spPnowCache[index];
            source.azimuthDeg = srcs[index]->solvedPose[3];
            source.elevationDeg = anchor.elevationDeg;
            source.pattern = spPatCache[index];
            source.patternHf = anchor.patternHf;
            source.directivityView = spDirCache[index];
            source.instrumentAmount = spAmtCache[index];
            source.aimAzDeg = spAimAzCache[index];
            source.aimElDeg = spAimElCache[index];
            source.aimRollDeg = spAimRollCache[index];

            std::array<MotionEnvelope, kMaxMotionChannels> nowCh{};
            auto now = photo.grid->sourceFieldAndChannels(source.pos, nowCh.data(), nCh);
            photo.grid->applyCloseness(now, source, listener, nowCh.data(), nCh);
            if (hasChGrid && listenerDisp > 1e-4) {
                for (int r = 0; r < nCh; ++r)
                    for (int w = 0; w < kBakeWinCount; ++w)
                        for (int b = 0; b < NB; ++b)
                            nowCh[(size_t)r][w][b] *= listenerChRatio[(size_t)r][w][b];
            }

            const auto& reference = photo.envelopes[index];
            const auto power = [](const Transducer& t, int band) {
                if (t.profile()) return 1.;
                const double p = t.patternHf < 0. ? t.pattern
                    : t.pattern + (t.patternHf - t.pattern) * (double(band) / (NB - 1));
                return (1. - p) * (1. - p) + p * p / 3.;
            };

            // Compute regularized channel shares for current pose and photo anchor
            std::array<MotionEnvelope, kMaxMotionChannels> pNowWin{}, pPhotoWin{};
            std::array<std::array<float, NB>, kMaxMotionChannels> pNowBand{}, pPhotoBand{};
            std::array<float, kMaxMotionChannels> pNowBroad{}, pPhotoBroad{};
            const bool hasPhotoWin = photo.photoWinEnergy.size() >= (size_t)(s + 1) * (size_t)R;

            if (hasChGrid) {
                motionComputeShares(nowCh.data(), nCh, true, pNowWin.data(), pNowBand, pNowBroad);
                if (hasPhotoWin) {
                    motionComputeShares(&photo.photoWinEnergy[index * (size_t)R], nCh, false,
                                        pPhotoWin.data(), pPhotoBand, pPhotoBroad);
                } else {
                    motionComputeShares(&photo.channelEnvelopes[index * (size_t)R], nCh, true,
                                        pPhotoWin.data(), pPhotoBand, pPhotoBroad);
                }
            }

            const double totalDisp = norm(source.pos - anchor.pos) + listenerDisp;
            const float alpha = static_cast<float>(std::clamp(totalDisp / 0.35, 0.0, 1.0));
            const float minShare = 1e-4f / static_cast<float>(std::max(1, nCh));

            for (int b = 0; b < NB; ++b) {
                const float radiation = static_cast<float>(std::sqrt(power(source, b) / std::max(1e-20, power(anchor, b))));
                const float floor = motionBandFloor(reference, b);
                std::array<float, kBakeWinCount> ratios{};
                float before = 0.f, after = 0.f;
                for (int w = 0; w < kBakeWinCount; ++w) {
                    const float ratio = std::clamp(motionRatio(now[w][b], reference[w][b], floor) *
                                                   listenerRatio[w][b] * radiation, .126f, 3.98f);
                    ratios[w] = ratio;
                    const float ref = reference[w][b];
                    before += ref * ref;
                    after += ref * ref * ratio * ratio;
                }

                // Compute per-channel directional multiplier dWin[w][r] for each window w=0..3
                std::array<std::array<float, kMaxMotionChannels>, kBakeWinCount> dWin{};
                std::array<std::array<float, kMaxMotionChannels>, kBakeWinCount> effTargetShare{};
                for (int w = 0; w < kBakeWinCount; ++w) {
                    for (int r = 0; r < nCh; ++r) {
                        dWin[w][(size_t)r] = 1.f;
                        effTargetShare[w][(size_t)r] = 1.f / static_cast<float>(std::max(1, nCh));
                    }
                    if (!hasChGrid || nCh <= 1) continue;

                    float normSum = 0.f;
                    for (int r = 0; r < nCh; ++r) {
                        const float pDenom = pPhotoWin[(size_t)r][w][b];
                        const float pNum = (1.f - alpha) * pDenom + alpha * pNowWin[(size_t)r][w][b];
                        const float d = std::clamp(std::sqrt(std::max(minShare, pNum) / std::max(minShare, pDenom)),
                                                   0.25f, 4.0f);
                        dWin[w][(size_t)r] = d;
                        normSum += pDenom * d * d;
                    }
                    // Energy-preserving normalization so total power across all R channels matches MO4b
                    if (normSum > 1e-12f) {
                        const float invNorm = 1.f / std::sqrt(normSum);
                        for (int r = 0; r < nCh; ++r) {
                            dWin[w][(size_t)r] *= invNorm;
                            effTargetShare[w][(size_t)r] = pPhotoWin[(size_t)r][w][b] * dWin[w][(size_t)r] * dWin[w][(size_t)r];
                        }
                    } else {
                        for (int r = 0; r < nCh; ++r)
                            effTargetShare[w][(size_t)r] = pNowWin[(size_t)r][w][b];
                    }
                }

                if (!shared) {
                    for (int r = 0; r < R; ++r) {
                        const int rc = std::min(r, nCh - 1);
                        for (int w = 0; w < kBakeWinCount; ++w) {
                            auto& gain = gains.windows[index * (size_t)R + (size_t)r][w][b];
                            const float target = std::clamp(ratios[w] * dWin[w][(size_t)rc], .063f, 6.0f);
                            gain = motionSlew(gain, target);
                        }
                    }
                    continue;
                }

                // Shared tail (S >= 5): per-source per-channel feed gain + per-channel 4-window equalizer
                const float feedScalar = before > 1e-20f ? std::sqrt(after / before) : radiation;
                float g2 = 0.f;
                for (int r = 0; r < R; ++r) {
                    const float g = photo.feed[((size_t)r * S + s) * 8 + b];
                    g2 += g * g;
                }
                const float g2Sum = g2;
                g2 /= static_cast<float>(std::max(1, R));

                std::array<float, kMaxMotionChannels> dFeed{};
                for (int r = 0; r < nCh; ++r) dFeed[(size_t)r] = 1.f;
                if (hasChGrid && nCh > 1 && g2Sum > 1e-20f) {
                    float feedNormSum = 0.f;
                    for (int r = 0; r < nCh; ++r) {
                        const float pPhotoFeedR = std::max(minShare, pPhotoBand[(size_t)r][(size_t)b]);
                        const float pNowFeedR = pNowBand[(size_t)r][(size_t)b];
                        const float pTgtR = (1.f - alpha) * pPhotoFeedR + alpha * pNowFeedR;
                        const float d = std::clamp(std::sqrt(std::max(minShare, pTgtR) / pPhotoFeedR), 0.25f, 4.0f);
                        dFeed[(size_t)r] = d;
                        feedNormSum += pPhotoFeedR * d * d;
                    }
                    if (feedNormSum > 1e-12f) {
                        const float invNorm = 1.f / std::sqrt(feedNormSum);
                        for (int r = 0; r < nCh; ++r) dFeed[(size_t)r] *= invNorm;
                    }
                }

                // Use the dry power of each source from the last control period,
                // weighted by the traced photo anchor power g2 of source s in band b.
                const float drive = totalDrive > 1e-12f ? motionDrive[index] : 1.f;
                const float anchorScale = before > 1e-20f ? (g2 / before) : 0.f;

                for (int w = 0; w < kBakeWinCount; ++w) {
                    const float ref = reference[w][b];
                    const float winAnchorE = anchorScale * ref * ref;
                    desiredScalar[w][b] += drive * winAnchorE * ratios[w] * ratios[w];
                    fedScalar[w][b] += drive * (g2 * pPhoto[w][b]) * (feedScalar * feedScalar);
                }

                for (int r = 0; r < R; ++r) {
                    const int rc = std::min(r, nCh - 1);
                    const size_t rs = (size_t)r * (size_t)S + index;
                    auto& feed = gains.feed[rs][b];
                    const float targetFeed = std::clamp(feedScalar * dFeed[(size_t)rc], .063f, 6.0f);
                    feed = motionSlew(feed, targetFeed);
                    gains.feedStep[rs][b] = (feed - gains.feedStart[rs][b]) / cfg.ctrlInterval;

                    for (int w = 0; w < kBakeWinCount; ++w) {
                        const float ref = reference[w][b];
                        const float winAnchorE = anchorScale * ref * ref;
                        if (!hasChGrid || nCh <= 1) {
                            desiredCh[(size_t)rc][w][b] += drive * winAnchorE * ratios[w] * ratios[w];
                            fedCh[(size_t)rc][w][b] += drive * (g2 * pPhoto[w][b]) * feed * feed;
                        } else {
                            const float chDesiredShare = effTargetShare[w][(size_t)rc] * static_cast<float>(nCh);
                            const float chFedShare = std::max(minShare, pPhotoBand[(size_t)rc][(size_t)b]) * static_cast<float>(nCh);
                            desiredCh[(size_t)rc][w][b] += drive * (winAnchorE * ratios[w] * ratios[w]) * chDesiredShare;
                            fedCh[(size_t)rc][w][b] += drive * (g2 * pPhoto[w][b] * chFedShare) * (targetFeed * targetFeed);
                        }
                    }
                }
            }
        }
        if (shared) {
            for (int w = 0; w < kBakeWinCount; ++w) {
                for (int b = 0; b < NB; ++b) {
                    const float wScalar = fedScalar[w][b] > 1e-20f
                        ? std::clamp(std::sqrt(desiredScalar[w][b] / fedScalar[w][b]), .126f, 3.98f)
                        : 1.f;
                    if (!hasChGrid || nCh <= 1) {
                        for (int r = 0; r < R; ++r) {
                            auto& gain = gains.windows[(size_t)r][w][b];
                            gain = motionSlew(gain, wScalar);
                        }
                        continue;
                    }
                    float fedSum = 0.f;
                    for (int r = 0; r < nCh; ++r) fedSum += fedCh[(size_t)r][w][b];
                    std::array<float, kMaxMotionChannels> dShared{};
                    float normCheck = 0.f;
                    for (int r = 0; r < nCh; ++r) {
                        const float rawRatio = fedCh[(size_t)r][w][b] > 1e-20f
                            ? std::sqrt(desiredCh[(size_t)r][w][b] / fedCh[(size_t)r][w][b])
                            : wScalar;
                        const float d = wScalar > 1e-6f ? std::clamp(rawRatio / wScalar, 0.25f, 4.0f) : 1.f;
                        dShared[(size_t)r] = d;
                        const float pFedR = fedSum > 1e-20f ? (fedCh[(size_t)r][w][b] / fedSum) : (1.f / static_cast<float>(nCh));
                        normCheck += pFedR * d * d;
                    }
                    if (normCheck > 1e-12f) {
                        const float invNorm = 1.f / std::sqrt(normCheck);
                        for (int r = 0; r < nCh; ++r) dShared[(size_t)r] *= invNorm;
                    }
                    for (int r = 0; r < R; ++r) {
                        const int rc = std::min(r, nCh - 1);
                        auto& gain = gains.windows[(size_t)r][w][b];
                        const float target = std::clamp(wScalar * dShared[(size_t)rc], .063f, 6.0f);
                        gain = motionSlew(gain, target);
                    }
                }
            }
        }
    }

    void motionPeriod(Vec3 listener) noexcept {
        if (shared) {
            for (int s = 0; s < S; ++s) {
                motionDrive[(size_t)s] = .85f * motionDrive[(size_t)s] + .15f * motionDriveSum[(size_t)s] / cfg.ctrlInterval;
                motionDriveSum[(size_t)s] = 0.f;
            }
        }
        motionUpdateGains(motionActive, listener);
        motionUpdateGains(motionIncoming, listener);
    }

    void motionEqualize(const TailConv& t, const Slot& slot, int r, int bank) const noexcept {
        if (bank < 0 || slot.partitions == 0) return;
        const auto& wins = motionGains[bank].windows;
        size_t winIdx = 0;
        if (shared) {
            const int outCh = t.bakeSource >= 0 ? t.bakeSource : r;
            winIdx = (size_t)std::clamp(outCh, 0, (int)wins.size() - 1);
        } else {
            const size_t sr = (size_t)std::max(0, t.bakeSource) * (size_t)R + (size_t)r;
            winIdx = wins.size() > sr ? sr : (size_t)std::clamp(t.bakeSource, 0, (int)wins.size() - 1);
        }
        const auto& target = wins[winIdx];
        auto& previous = slot.bakeEqGain[(size_t)r];
        eqOldWin = previous;
        eqNewWin = previous = target;
        eqSlot = &slot;
        eqR = r;
        eqPending = true;
    }

    void motionNeutralize(const Slot& slot, int r) const noexcept {
        if (!slot.partitions || slot.bakeEqGain.size() <= (size_t)r) return;
        auto& previous = slot.bakeEqGain[(size_t)r];
        MotionEnvelope neutral;
        for (auto& window : neutral) window.fill(1.f);
        if (previous == neutral) return;
        eqOldWin = previous;
        eqNewWin = previous = neutral;
        eqSlot = &slot;
        eqR = r;
        eqPending = true;
    }

    void motionAccumulateSlice(TailConv& t, int r, int q) const {
        const auto& slot = t.slot[t.cur];
        int first = 1, last = 1;
        sliceBounds(slot.partitions, q, first, last);
        const int head = q == 3 ? t.head : (t.head + 1) % t.P;
        int windowStart = 0;
        for (int w = 0; w < kBakeWinCount; ++w) {
            const int windowEnd = std::min(slot.partitions, eqWinEndK[(size_t)w]);
            const int lo = std::max(windowStart, first), hi = std::min(windowEnd, last);
            const size_t offset = ((size_t)r * kBakeWinCount + w) * t.bins;
            float* re = t.motionPreRe.data() + offset;
            float* im = t.motionPreIm.data() + offset;
            if (hi > lo)
                accumulateFdlRangeAdd(t, slot, r, lo, hi, t.fdlRe.data(), t.fdlIm.data(),
                                      t.fdlNz.data(), head, re, im);
            if (q == 3 && windowStart == 0 && windowEnd > 0)
                accumulateFdlRangeAdd(t, slot, r, 0, 1, t.fdlRe.data(), t.fdlIm.data(),
                                      t.fdlNz.data(), head, re, im);
            windowStart = windowEnd;
        }
    }

    // Shared feed: both photos have their own per-source, per-channel ramp and physical
    // normalization. The causal handover only scales each mixed feed once.
    void motionSharedFeed(int r, int ph, int n, int bank, float* out) noexcept {
        std::fill_n(out, n, 0.f);
        const auto& base = bank >= 0 ? motionPhotos[bank].feed : lateGuse;
        for (int s = 0; s < S; ++s) {
            const size_t rs = (size_t)r * (size_t)S + (size_t)s;
            const float* anchor = &base[rs * 8];
            const size_t gainIdx = (bank >= 0 && motionGains[bank].feedStart.size() >= (size_t)R * (size_t)S)
                ? rs : (size_t)s;
            float g[NB], dg[NB];
            for (int band = 0; band < NB; ++band) {
                const float start = bank >= 0 ? motionGains[bank].feedStart[gainIdx][band] : 1.f;
                const float step = bank >= 0 ? motionGains[bank].feedStep[gainIdx][band] : 0.f;
                g[band] = anchor[band] * (start + step * ph);
                dg[band] = anchor[band] * step;
            }
            const auto& ring = srcs[(size_t)s]->ring;
            const size_t first = (size_t)gcount & ringMask;
            int j = 0;
#if VERB_REQUIRE_AVX2
            if (first + (size_t)n <= ringMask + 1) {
                const auto vg = _mm256_setr_ps(g[0], g[1], g[2], g[3], g[4], g[5], g[6], 0.f);
                const auto vd = _mm256_setr_ps(dg[0], dg[1], dg[2], dg[3], dg[4], dg[5], dg[6], 0.f);
                const float* f = ring.data() + first * 8;
                for (; j + 4 <= n; j += 4, f += 32) {
                    const auto a = _mm256_fmadd_ps(vd, _mm256_set1_ps((float)j), vg);
                    const auto b = _mm256_fmadd_ps(vd, _mm256_set1_ps(float(j + 1)), vg);
                    const auto c = _mm256_fmadd_ps(vd, _mm256_set1_ps(float(j + 2)), vg);
                    const auto d = _mm256_fmadd_ps(vd, _mm256_set1_ps(float(j + 3)), vg);
                    const auto x = _mm256_hadd_ps(_mm256_mul_ps(a, _mm256_loadu_ps(f)), _mm256_mul_ps(b, _mm256_loadu_ps(f + 8)));
                    const auto y = _mm256_hadd_ps(_mm256_mul_ps(c, _mm256_loadu_ps(f + 16)), _mm256_mul_ps(d, _mm256_loadu_ps(f + 24)));
                    const auto sum = _mm256_hadd_ps(x, y);
                    const auto four = _mm_add_ps(_mm256_castps256_ps128(sum), _mm256_extractf128_ps(sum, 1));
                    _mm_storeu_ps(out + j, _mm_add_ps(_mm_loadu_ps(out + j), four));
                }
            }
#endif
            for (; j < n; ++j) {
                const float* f = ring.data() + (((size_t)gcount + j) & ringMask) * 8;
                float value = 0.f;
                for (int b = 0; b < NB; ++b)
                    value += f[b] * (g[b] + dg[b] * j);
                out[j] += value;
            }
        }
    }

    void motionSharedInput(int ph, int n) noexcept {
        const bool frozen = requestedFreeze.load(std::memory_order_relaxed);
        for (int r = 0; r < R; ++r) {
            auto& t = motionConv(r);
            const int incoming = t.hoSlot >= 0 ? motionIncoming : -1;
            const int active = motionIncoming >= 0 && incoming < 0 ? motionIncoming : motionActive;
            float* old = motionFeedScratch.data();
            float* fresh = motionFeedScratch.data() + cfg.ctrlInterval;
            if (!frozen) {
                motionSharedFeed(r, ph, n, active, old);
                if (incoming >= 0) motionSharedFeed(r, ph, n, incoming, fresh);
            }
            for (int j = 0; j < n; ++j) {
                const size_t sample = (gcount + j) % B;
                float w = 0.f;
                if (incoming >= 0) {
                    const double u = std::min(1., double(gcount + j - t.inputStart) / (t.inputFadeBlocks * B));
                    w = static_cast<float>(.5 - .5 * std::cos(kPi * u));
                    t.freshIn[sample] = frozen ? 0.f : w * fresh[j];
                }
                t.inAcc[sample] = frozen ? 0.f : (1.f - w) * old[j];
            }
        }
    }
