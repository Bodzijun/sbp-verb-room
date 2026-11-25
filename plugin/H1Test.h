#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "MovementPath.h"

namespace h1test {

inline juce::File h1RepoFile(const char* rel) {
    auto dir = juce::File::getCurrentWorkingDirectory();
    for (int depth = 0; depth < 6; ++depth) {
        auto cand = dir.getChildFile(rel);
        if (cand.exists()) return cand;
        auto parent = dir.getParentDirectory();
        if (parent == dir) break;
        dir = parent;
    }
    return juce::File::getCurrentWorkingDirectory().getChildFile(rel);
}

inline void pumpAudioAndDispatch(VerbProcessor& p, int blocks, double dispatchMsEach = 2.0) {
    const int ch = std::max(2, std::max(p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()));
    juce::AudioBuffer<float> buf(ch, 128);
    juce::MidiBuffer midi;
    for (int i = 0; i < blocks; ++i) {
        buf.clear();
        p.processBlock(buf, midi);
        if (dispatchMsEach > 0.0)
            juce::MessageManager::getInstance()->runDispatchLoopUntil((int)dispatchMsEach);
    }
}

inline bool waitTailSettled(VerbProcessor& p, double timeoutMs = 15000.0) {
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < timeoutMs) {
        pumpAudioAndDispatch(p, 2, 5.0);
        if (p.isTailCurrent()) return true;
        juce::Thread::sleep(5);
    }
    return p.isTailCurrent();
}

inline bool waitForSigChange(VerbProcessor& p, const std::vector<uint64_t>& prevSigs, double timeoutMs = 4000.0) {
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < timeoutMs) {
        if (auto eng = E9bAccess::engine(p)) {
            if (eng->bakeContentSignatures() != prevSigs) return true;
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil(15);
        juce::Thread::sleep(10);
    }
    if (auto eng = E9bAccess::engine(p)) return eng->bakeContentSignatures() != prevSigs;
    return false;
}

inline bool waitBakedStatus(VerbProcessor& p, double timeoutMs = 60000.0) {
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < timeoutMs) {
        pumpAudioAndDispatch(p, 4, 2.0);
        const auto st = p.pathBakeStatus();
        if (p.pathBakePercent() == 100 && st["state"].toString() == "baked")
            return true;
        juce::Thread::sleep(5);
    }
    return p.pathBakePercent() == 100 && p.pathBakeStatus()["state"].toString() == "baked";
}

inline bool waitRestoredBaked(VerbProcessor& p, double timeoutMs = 10000.0) {
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < timeoutMs) {
        pumpAudioAndDispatch(p, 4, 5.0);
        if (p.pathBakePercent() == 100 && p.pathBakeStatus()["state"].toString() == "baked")
            return true;
        juce::Thread::sleep(10);
    }
    return p.pathBakePercent() == 100 && p.pathBakeStatus()["state"].toString() == "baked";
}

struct ImpulseResult {
    bool finite = true;
    double energy = 0.0;
    std::vector<float> samples;
};

inline void settleSteadyForImpulse(VerbProcessor& p, double timeoutMs = 15000.0) {
    // Allow motionSettleSec (0.4 s) + motionFadeSec (0.75 s) to complete deterministically on both pre-save and reloaded engines.
    const int nIn = std::max(2, p.getTotalNumInputChannels());
    const int nOut = std::max(2, p.getTotalNumOutputChannels());
    const int bufCh = std::max(nIn, nOut);
    juce::AudioBuffer<float> buf(bufCh, 128);
    juce::MidiBuffer midi;
    for (int i = 0; i < 12; ++i) {
        buf.clear();
        p.processBlock(buf, midi);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    auto eng = E9bAccess::engine(p);
    const auto baseTrans = eng ? eng->motionStats().transitionsDone : 0ull;
    const auto baseLegacy = eng ? eng->motionStats().legacySwaps : 0ull;
    const double minAudioSec = 0.6;
    const int minBlocks = (int)(minAudioSec * 48000.0 / 128.0);
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    int blk = 0;
    while (juce::Time::getMillisecondCounterHiRes() - t0 < timeoutMs) {
        buf.clear();
        p.processBlock(buf, midi);
        ++blk;
        if (blk % 8 == 0) {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
            if (blk >= minBlocks && eng && p.isTailCurrent()) {
                const auto ms = eng->motionStats();
                if (ms.transState == 0 && (blk >= (int)(1.5 * 48000.0 / 128.0) || ms.transitionsDone > baseTrans || ms.legacySwaps > baseLegacy))
                    break;
            }
        }
    }
    for (int i = 0; i < 64; ++i) {
        buf.clear();
        p.processBlock(buf, midi);
    }
}

inline ImpulseResult renderImpulse(VerbProcessor& p, int blocks = 64, int blockSize = 128) {
    settleSteadyForImpulse(p);
    const int nIn = std::max(2, p.getTotalNumInputChannels());
    const int nOut = std::max(2, p.getTotalNumOutputChannels());
    const int bufCh = std::max(nIn, nOut);
    juce::AudioBuffer<float> buf(bufCh, blockSize);
    juce::MidiBuffer midi;
    ImpulseResult res;
    res.samples.reserve((size_t)blocks * (size_t)blockSize * 2);
    for (int b = 0; b < blocks; ++b) {
        buf.clear();
        if (b == 0) {
            buf.setSample(0, 0, 1.0f);
            buf.setSample(1, 0, 1.0f);
        }
        p.processBlock(buf, midi);
        for (int s = 0; s < blockSize; ++s) {
            const float l = buf.getSample(0, s);
            const float r = buf.getSample(1, s);
            if (!std::isfinite(l) || !std::isfinite(r)) res.finite = false;
            res.energy += (double)l * l + (double)r * r;
            res.samples.push_back(l);
            res.samples.push_back(r);
        }
    }
    return res;
}

inline float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = std::min(a.size(), b.size());
    float d = 0.f;
    for (size_t i = 0; i < n; ++i)
        d = std::max(d, std::abs(a[i] - b[i]));
    return d;
}

inline double relativeRmsError(const std::vector<float>& ref, const std::vector<float>& cur) {
    const size_t n = std::min(ref.size(), cur.size());
    double ed = 0.0, er = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double d = (double)cur[i] - (double)ref[i];
        ed += d * d;
        er += (double)ref[i] * ref[i];
    }
    return er > 0.0 ? std::sqrt(ed / er) : 1.0;
}

inline void checkTag(bool ok, const char* tag, const char* msg) {
    const std::string text = std::string(tag) + " " + msg;
    check(ok, text.c_str());
}

inline bool setupBakedProcessor(std::unique_ptr<VerbProcessor>& outProc, double patternVal, bool setLivePattern, const char* tag) {
    outProc = std::make_unique<VerbProcessor>();
    auto& p = *outProc;
    p.setTailSettings({4000, false});
    p.setPlayConfigDetails(2, 2, 48000, 128);
    p.prepareToPlay(48000, 128);
    checkTag(waitReady(p, 90), tag, "initial ready");

    verbp::MovementPath path0;
    path0.enabled = true;
    path0.smooth = true;
    path0.points = {{{1.5, 4.5, 1.5}}, {{3.0, 5.2, 1.5}}, {{4.5, 4.5, 1.5}}};
    checkTag(p.setMovementPath(0, path0), tag, "movement path set");

    if (setLivePattern) {
        auto eng0 = E9bAccess::engine(p);
        const auto sigInit = eng0 ? eng0->bakeContentSignatures() : std::vector<uint64_t>{};
        p.setSourcePattern(0, patternVal);
        if (patternVal != 0.0)
            checkTag(waitForSigChange(p, sigInit), tag, "setSourcePattern updates sig");
        else
            pumpAudioAndDispatch(p, 8, 40.0);
    }

    setP(p, "s1path", 0.35);
    waitTailSettled(p);
    checkTag(p.startPathBake(0) && waitBakedStatus(p), tag, "bake completes");
    waitTailSettled(p);
    return p.pathBakePercent() == 100 && p.pathBakeStatus()["state"].toString() == "baked";
}

inline void runRoundTripPatternCase(double patternVal, bool setLivePattern, bool testDoubleReload, const char* tag) {
    std::unique_ptr<VerbProcessor> pPtr;
    if (!setupBakedProcessor(pPtr, patternVal, setLivePattern, tag)) return;
    auto& p = *pPtr;
    auto engBefore = E9bAccess::engine(p);
    checkTag(engBefore != nullptr, tag, "engine non-null");
    if (!engBefore) return;

    const auto sigBefore = engBefore->bakeContentSignatures();
    const auto snapBefore = engBefore->bakedTablesSnapshot();
    checkTag(sigBefore.size() == 1 && snapBefore.size() == 1, tag, "1 baked table");
    if (sigBefore.empty() || snapBefore.empty()) return;
    const uint64_t hashBefore = snapBefore[0]->hash();
    const uint64_t contentSigBefore = snapBefore[0]->contentSig;
    checkTag(contentSigBefore == sigBefore[0], tag, "table contentSig matches live sig");

    const auto irBefore = renderImpulse(p);
    checkTag(irBefore.finite && irBefore.energy > 1e-8, tag, "pre-save impulse finite non-zero");

    juce::MemoryBlock stateMb;
    p.getStateInformation(stateMb);

    auto qPtr = std::make_unique<VerbProcessor>();
    auto& q = *qPtr;
    q.setTailSettings({4000, false});
    q.setPlayConfigDetails(2, 2, 48000, 128);
    q.prepareToPlay(48000, 128);
    q.setStateInformation(stateMb.getData(), (int)stateMb.getSize());
    checkTag(waitReady(q, 90), tag, "reload ready");

    const bool qBaked = waitRestoredBaked(q);
    checkTag(qBaked, tag, "reloaded state stays Baked");
    auto engAfter = E9bAccess::engine(q);
    checkTag(engAfter != nullptr, tag, "reloaded engine non-null");
    if (!engAfter) return;

    const auto statsAfter = engAfter->bakeStats();
    const auto sigAfter = engAfter->bakeContentSignatures();
    const auto snapAfter = engAfter->bakedTablesSnapshot();
    checkTag(statsAfter.state == verb::Engine::BakeState::Baked && statsAfter.restored == 1,
             tag, "BakeStats Baked and restored==1");
    checkTag(sigAfter == sigBefore, tag, "reloaded content signature matches pre-save");
    checkTag(snapAfter.size() == 1 && snapAfter[0]->hash() == hashBefore && snapAfter[0]->contentSig == contentSigBefore,
             tag, "reloaded table hash & contentSig match");

    waitTailSettled(q);
    const auto irAfter = renderImpulse(q);
    const float diff = maxAbsDiff(irBefore.samples, irAfter.samples);
    const double relErr = relativeRmsError(irBefore.samples, irAfter.samples);
    std::printf("  [%s] reload state=%s sigMatch=%d restored=%d irDiff=%.3e relRms=%.3e\n",
                tag, q.pathBakeStatus()["state"].toString().toRawUTF8(),
                (int)(sigAfter == sigBefore), statsAfter.restored, (double)diff, relErr);
    checkTag(irAfter.finite && irAfter.energy > 1e-8 && relErr <= 1e-3,
             tag, "reloaded impulse matches pre-save");

    if (testDoubleReload) {
        juce::MemoryBlock stateMb2;
        q.getStateInformation(stateMb2);
        auto rPtr = std::make_unique<VerbProcessor>();
        auto& r = *rPtr;
        r.setTailSettings({4000, false});
        r.setPlayConfigDetails(2, 2, 48000, 128);
        r.prepareToPlay(48000, 128);
        r.setStateInformation(stateMb2.getData(), (int)stateMb2.getSize());
        checkTag(waitReady(r, 90) && waitRestoredBaked(r), tag, "second reload stays Baked");
        if (auto engR = E9bAccess::engine(r)) {
            checkTag(engR->bakeContentSignatures() == sigBefore, tag, "second reload signature unchanged");
        }
    }
}

inline void runLegacyAndRejectionChecks() {
    // 1. Legacy untouched 0.3.0-style unfixed table (pattern=0.0, fixedPattern=false) accepted by saved-cache matcher;
    //    mutated signature strictly rejected; standalone Engine::setSourcePattern keeps fixedPattern=false behavior.
    std::unique_ptr<VerbProcessor> pPtr;
    if (setupBakedProcessor(pPtr, 0.0, false, "H1 legacy0")) {
        auto& p = *pPtr;
        auto pEng = E9bAccess::engine(p);
        const auto scSnap = pEng ? pEng->sceneSnapshot() : nullptr;
        const auto pTables = pEng ? pEng->bakedTablesSnapshot() : std::vector<std::shared_ptr<const verb::BakedTable>>{};
        check(pEng && scSnap && !pTables.empty(), "H1 captured scene snapshot and table for legacy check");
        if (pEng && scSnap && !pTables.empty()) {
            verb::EngineConfig ec;
            ec.erOrder = 1;
            ec.ctrlInterval = 128;
            ec.tailBlock = 512;
            ec.tailThread = true;
            ec.sharedTail = false;
            ec.tailRays = 8000;
            ec.adaptiveTail = false;
            ec.maxEarlyDelaySec = 0.75;
            verb::Scene scLeg = *scSnap;
            scLeg.clutter.count = 0;
            scLeg.roughness = 0.0;
            verb::Engine legacyEng(scLeg, ec);
            if (pTables[0]->staticPose.size() >= 4)
                legacyEng.setListener(pTables[0]->staticPose[0], pTables[0]->staticPose[1], pTables[0]->staticPose[2], pTables[0]->staticPose[3]);
            legacyEng.setBakedPaths(pTables[0]->movingPaths);
            const auto legSigs0 = legacyEng.bakeContentSignatures();
            const auto procSigs = pEng->bakeContentSignatures();
            check(legSigs0.size() == 1 && procSigs.size() == 1 && legSigs0[0] != procSigs[0],
                  "H1 standalone Engine uses fixedPattern=false distinct from VerbProcessor canonical fixedPattern=true");

            legacyEng.setSourcePattern(0, 0.7);
            const auto legSigsPat = legacyEng.bakeContentSignatures();
            check(legSigsPat.size() == 1 && legSigsPat[0] != legSigs0[0],
                  "H1 standalone Engine::setSourcePattern updates bakeContentSignatures");

            legacyEng.setSourcePattern(0, 0.0);
            legacyEng.requestPathBake(0);
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            while (juce::Time::getMillisecondCounterHiRes() - t0 < 30000.0) {
                if (legacyEng.bakeStats().state == verb::Engine::BakeState::Baked) break;
                juce::Thread::sleep(10);
            }
            const auto legTables = legacyEng.bakedTablesSnapshot();
            check(legacyEng.bakeStats().state == verb::Engine::BakeState::Baked && legTables.size() == 1,
                  "H1 standalone Engine produces unfixed legacy BakedTable");
            if (legTables.size() == 1) {
                p.clearPathBake();
                const double tc = juce::Time::getMillisecondCounterHiRes();
                while (juce::Time::getMillisecondCounterHiRes() - tc < 5000.0 && p.pathBakeStatus()["state"].toString() != "notBaked") {
                    pumpAudioAndDispatch(p, 4, 10.0);
                }
                std::vector<verb::BakedTable> installVec{*legTables[0]};
                const bool instOk = pEng->installBakedTables(installVec);
                std::printf("  [H1 legacy0] legSig=%llu procSig=%llu instOk=%d state=%d\n",
                            (unsigned long long)legTables[0]->contentSig,
                            (unsigned long long)(pEng->bakeContentSignatures().empty() ? 0ull : pEng->bakeContentSignatures()[0]),
                            (int)instOk, (int)pEng->bakeStats().state);
                check(instOk && pEng->bakeStats().state == verb::Engine::BakeState::Baked,
                      "H1 saved 0.3.0-style unfixed (pattern=0.0) table accepted cleanly");

                // End-to-end XML (b) state restore: state saved in 0.3.0/0.2.x with fixedPattern=false BakeCache
                {
                    juce::MemoryBlock stateOrig;
                    p.getStateInformation(stateOrig);
                    const juce::String xmlBase = p4test::stateXml(p);
                    const juce::String xmlLegacyB = p4test::blobToBakeCacheXml(xmlBase, verb::bakeSerializeTables(installVec), (int)verb::kBakePersistVersion);
                    VerbProcessor qLeg;
                    qLeg.setTailSettings({4000, false});
                    qLeg.setPlayConfigDetails(2, 2, 48000, 128);
                    qLeg.prepareToPlay(48000, 128);
                    check(waitReady(qLeg, 90) && p4test::loadStateXml(qLeg, xmlLegacyB) && waitRestoredBaked(qLeg),
                          "H1 end-to-end XML state with fixedPattern=false BakeCache (pattern=0.0) restores Baked");
                    if (auto qLegEng = E9bAccess::engine(qLeg)) {
                        check(qLegEng->bakeStats().restored == 1 && qLegEng->bakeStats().hash == legTables[0]->hash(),
                              "H1 end-to-end XML legacy (b) restores exact saved table");
                    }
                    qLeg.releaseResources();
                }

                // End-to-end XML (b) state restore with non-zero pattern=0.7 & fixedPattern=false (0.3.0 reload bug)
                {
                    std::unique_ptr<VerbProcessor> p7Ptr;
                    if (setupBakedProcessor(p7Ptr, 0.7, true, "H1 legacyB0.7")) {
                        auto p7Eng = E9bAccess::engine(*p7Ptr);
                        const auto scSnap7 = p7Eng ? p7Eng->sceneSnapshot() : nullptr;
                        const auto p7Tables = p7Eng ? p7Eng->bakedTablesSnapshot() : std::vector<std::shared_ptr<const verb::BakedTable>>{};
                        if (p7Eng && scSnap7 && !p7Tables.empty()) {
                            verb::Scene scLeg7 = *scSnap7;
                            scLeg7.clutter.count = 0;
                            scLeg7.roughness = 0.0;
                            if (!scLeg7.sources.empty()) scLeg7.sources[0].pattern = (double)(float)0.7;
                            verb::Engine leg7(scLeg7, ec);
                            if (p7Tables[0]->staticPose.size() >= 4)
                                leg7.setListener(p7Tables[0]->staticPose[0], p7Tables[0]->staticPose[1], p7Tables[0]->staticPose[2], p7Tables[0]->staticPose[3]);
                            leg7.setBakedPaths(p7Tables[0]->movingPaths);
                            leg7.setSourcePattern(0, (double)(float)0.7);
                            leg7.requestPathBake(0);
                            const double tl0 = juce::Time::getMillisecondCounterHiRes();
                            while (juce::Time::getMillisecondCounterHiRes() - tl0 < 30000.0 && leg7.bakeStats().state != verb::Engine::BakeState::Baked)
                                juce::Thread::sleep(10);
                            const auto leg7Tables = leg7.bakedTablesSnapshot();
                            check(leg7.bakeStats().state == verb::Engine::BakeState::Baked && leg7Tables.size() == 1
                                  && leg7Tables[0]->contentSig != p7Eng->bakeContentSignatures()[0],
                                  "H1 synthesized 0.3.0 direction-(b) fixedPattern=false table for pattern=0.7");
                            if (leg7Tables.size() == 1) {
                                std::vector<verb::BakedTable> leg7Vec{*leg7Tables[0]};
                                const juce::String xml7B = p4test::blobToBakeCacheXml(p4test::stateXml(*p7Ptr), verb::bakeSerializeTables(leg7Vec), (int)verb::kBakePersistVersion);
                                VerbProcessor qLeg7;
                                qLeg7.setTailSettings({4000, false});
                                qLeg7.setPlayConfigDetails(2, 2, 48000, 128);
                                qLeg7.prepareToPlay(48000, 128);
                                check(waitReady(qLeg7, 90) && p4test::loadStateXml(qLeg7, xml7B) && waitRestoredBaked(qLeg7),
                                      "H1 end-to-end XML state with fixedPattern=false BakeCache (pattern=0.7) restores Baked");
                                if (auto qLeg7Eng = E9bAccess::engine(qLeg7)) {
                                    check(qLeg7Eng->bakeStats().restored == 1 && qLeg7Eng->bakeStats().hash == leg7Tables[0]->hash(),
                                          "H1 end-to-end XML direction-(b) pattern=0.7 restores exact saved table");
                                }
                                qLeg7.releaseResources();
                            }
                        }
                    }
                }

                p.clearPathBake();
                pEng->clearPathBake();
                const double tc2 = juce::Time::getMillisecondCounterHiRes();
                while (juce::Time::getMillisecondCounterHiRes() - tc2 < 5000.0 && (pEng->bakeStats().state == verb::Engine::BakeState::Baked || p.pathBakeStatus()["state"].toString() != "notBaked")) {
                    pumpAudioAndDispatch(p, 4, 10.0);
                }
                installVec[0].contentSig ^= 1ULL;
                check(!pEng->installBakedTables(installVec) && pEng->bakeStats().state != verb::Engine::BakeState::Baked,
                      "H1 corrupted contentSig strictly rejected");
            }
        }
    }

    // 2. .verbroom preset round-trip with fixed pattern=0.7
    {
        std::unique_ptr<VerbProcessor> pPtr;
        if (setupBakedProcessor(pPtr, 0.7, true, "H1 verbroom")) {
            const auto presetXml = pPtr->getRoomPresetText();
            const auto pSig = E9bAccess::engine(*pPtr)->bakeContentSignatures();
            VerbProcessor r;
            r.setTailSettings({4000, false});
            r.setPlayConfigDetails(2, 2, 48000, 128);
            r.prepareToPlay(48000, 128);
            juce::String err;
            check(r.loadRoomPresetText(presetXml, err) && waitReady(r, 90), "H1 .verbroom pattern=0.7 loads");
            const auto rSrc = r.getSources();
            check(!rSrc.empty() && std::abs(rSrc[0].pattern - 0.7) < 1e-5, "H1 .verbroom preserves pattern=0.7");
            check(r.pathBakePercent() == 0, "H1 .verbroom resets bake cleanly per spec");
            check(r.startPathBake(0) && waitBakedStatus(r), "H1 .verbroom re-bakes cleanly");
            check(E9bAccess::engine(r)->bakeContentSignatures() == pSig,
                  "H1 .verbroom re-bake signature matches original pattern=0.7 signature");
        }
    }

    // 3. Stage2 legacy.state and legacy.verbroom compatibility
    if (const auto stFile = h1RepoFile("plugin/fixtures/stage2/legacy.state"); stFile.existsAsFile()) {
        VerbProcessor leg;
        leg.setTailSettings({4000, false});
        leg.setPlayConfigDetails(2, 2, 48000, 128);
        leg.prepareToPlay(48000, 128);
        juce::MemoryBlock mb;
        check(stFile.loadFileAsData(mb), "H1 read stage2/legacy.state");
        leg.setStateInformation(mb.getData(), (int)mb.getSize());
        check(waitReady(leg, 90), "H1 stage2/legacy.state becomes ready");
        const auto ir = renderImpulse(leg);
        check(ir.finite && ir.energy > 1e-8, "H1 stage2/legacy.state renders finite non-zero audio");
    } else {
        check(false, "H1 stage2/legacy.state fixture missing");
    }

    if (const auto vrFile = h1RepoFile("plugin/fixtures/stage2/legacy.verbroom"); vrFile.existsAsFile()) {
        VerbProcessor leg;
        leg.setTailSettings({4000, false});
        leg.setPlayConfigDetails(2, 2, 48000, 128);
        leg.prepareToPlay(48000, 128);
        juce::String err;
        check(leg.loadRoomPresetText(vrFile.loadFileAsString(), err) && waitReady(leg, 90),
              "H1 stage2/legacy.verbroom becomes ready");
        const auto ir = renderImpulse(leg);
        check(ir.finite && ir.energy > 1e-8, "H1 stage2/legacy.verbroom renders finite non-zero audio");
    } else {
        check(false, "H1 stage2/legacy.verbroom fixture missing");
    }

    if (const auto azFile = h1RepoFile("test/test azimut.verbroom"); azFile.existsAsFile()) {
        VerbProcessor azp;
        azp.setTailSettings({4000, false});
        azp.setPlayConfigDetails(2, 2, 48000, 128);
        azp.prepareToPlay(48000, 128);
        juce::String err;
        check(azp.loadRoomPresetText(azFile.loadFileAsString(), err) && waitReady(azp, 90),
              "H1 test azimut.verbroom loads cleanly");
        const auto ir = renderImpulse(azp);
        check(ir.finite && ir.energy > 1e-8, "H1 test azimut.verbroom renders finite non-zero audio");
    }
}

static void runDryMute32SourcesCheck() {
    VerbProcessor p;
    p.setTailSettings({4000, false});
    p.setPlayConfigDetails(64, 2, 48000, 128);
    p.prepareToPlay(48000, 128);
    check(waitReady(p, 90), "H1 T4 32-source processor ready");

    std::vector<verbp::SourceCfg> sources;
    sources.reserve(verbp::kMaxSources);
    for (int k = 0; k < verbp::kMaxSources; ++k) {
        verbp::SourceCfg s;
        s.name = "Grp " + std::to_string(k + 1);
        s.format = 1;  // Stereo
        s.channels = 2;
        s.firstIn = 2 * k;
        s.sum = true;
        sources.push_back(std::move(s));
    }
    p.setSources(sources);
    check(waitReady(p, 90), "H1 T4 32 stereo sources ready (64 in -> 2 out)");

    setP(p, "spatialDry", 0.0);
    setP(p, "mix", 0.0);

    juce::AudioBuffer<float> buf(64, 128);
    juce::MidiBuffer midi;
    auto runBlock = [&](bool signalAll64) -> std::pair<double, double> {
        buf.clear();
        const int activeCh = signalAll64 ? 64 : 2;
        for (int ch = 0; ch < activeCh; ++ch) {
            float* dst = buf.getWritePointer(ch);
            for (int i = 0; i < 128; ++i) dst[i] = 0.5f;
        }
        p.processBlock(buf, midi);
        double outPeak = 0.0, passCh2Peak = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 128; ++i)
                outPeak = std::max(outPeak, (double)std::abs(buf.getSample(ch, i)));
        for (int ch = 2; ch < 64; ++ch)
            for (int i = 0; i < 128; ++i)
                passCh2Peak = std::max(passCh2Peak, (double)std::abs(buf.getSample(ch, i)));
        return {outPeak, passCh2Peak};
    };

    // Settle parameter ramps, then measure baseline covered channels 1/2
    runBlock(false);
    const auto base = runBlock(false);
    check(std::abs(base.first - 0.5) < 1e-4, "H1 T4 32-source baseline dry passes channels 1/2 unattenuated");

    // Lower all 32 source Gains to -60 dB: unpositioned raw dry stays unaffected
    for (int k = 0; k < verbp::kMaxSources; ++k)
        setP(p, ("s" + std::to_string(k + 1) + "g").c_str(), -60.0);
    runBlock(false);
    const auto gainMin = runBlock(false);
    check(std::abs(gainMin.first - 0.5) < 1e-4, "H1 T4 32-source Gain -60 dB leaves raw dry on 1/2 unchanged");

    // Mute all 32 sources: output 1/2 must become exact silence when signal is on covered channels
    for (int k = 0; k < verbp::kMaxSources; ++k)
        setP(p, ("s" + std::to_string(k + 1) + "mute").c_str(), 1.0);
    runBlock(false);  // block-0 ramp
    const auto muted12 = runBlock(false);
    const auto mutedAll64 = runBlock(true);
    std::printf("  [H1 T4 32-src] spatialDry=%.0f mix=%.0f baseOut=%.4f gainMinOut=%.4f allMute12Out=%.3e allMute64Out=%.3e ch3_64Bypass=%.4f\n",
                p.apvts.getRawParameterValue("spatialDry")->load(),
                p.apvts.getRawParameterValue("mix")->load(),
                base.first, gainMin.first, muted12.first, mutedAll64.first, mutedAll64.second);
    check(muted12.first <= 1e-7, "H1 T4 32 sources all Mute -> plugin output 1/2 is silence at Positioned dry off / Mix 0");
    check(mutedAll64.first <= 1e-7 && std::abs(mutedAll64.second - 0.5) < 1e-4,
          "H1 T4 32 sources all Mute -> plugin out 1/2 silent even with 64ch signal while host ch 3..64 stay untouched");
}

}  // namespace h1test

static int testH1Fast() {
    const int before = fails;
    std::printf("H1 fast: fixedPattern bake contentSig persistence (0.7, 0.0)\n");
    h1test::runRoundTripPatternCase(0.7, true, true, "H1 pat=0.7");
    h1test::runRoundTripPatternCase(0.0, false, false, "H1 pat=0.0");
    return fails == before ? 0 : 1;
}

static int testH1Suite() {
    const int before = fails;
    std::printf("H1 full suite: fixedPattern round-trips, legacy compat, rejection, standalone Engine\n");
    testH1Fast();
    h1test::runRoundTripPatternCase(0.35, true, true, "H1 pat=0.35");
    h1test::runLegacyAndRejectionChecks();
    h1test::runDryMute32SourcesCheck();
    std::printf(fails == before ? "\nH1 suite PASSED\n" : "\nH1 suite FAILED (%d)\n", fails - before);
    return fails == before ? 0 : 1;
}
