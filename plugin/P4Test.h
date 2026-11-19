#pragma once
// P4 (owner decision 2026-11-18): the bake survives save/reopen and Undo. These tests replace the old E9
// "session-only" expectation with the NEW specification: a stored table whose content signature still matches
// must be restored (Baked, identical audio, no worker); anything else must load safely as notBaked.
// Run: VERB_ONLY_P4=1 VerbPluginTest.exe
#include "E9bTest.h"
#include <cstdio>

namespace p4test {
// Small closed corridor: one source, fast non-adaptive bakes (~10 s), deterministic seeded-free geometry.
static const char* kScene =
    "sr 48000\nlength 1.2\nheight 3\n"
    "material concrete 0.02 0.02 0.03 0.03 0.04 0.05 0.06 0.05 0.30\n"
    "floor concrete\nceiling concrete\n"
    "wall 0 4 0 0 concrete\nwall 4 0 4 8 concrete\nwall 4 8 0 8 concrete\nwall 0 8 0 4 concrete\n"
    "source S1 1 1 1.5 0 0 0 0\nreceiver R1 2 4 1.5 0 0 0 0\nlistener 2 4 1.5 0\n";
static const char* kScene2 =
    "sr 48000\nlength 1.2\nheight 3\n"
    "material concrete 0.02 0.02 0.03 0.03 0.04 0.05 0.06 0.05 0.30\n"
    "floor concrete\nceiling concrete\n"
    "wall 0 4 0 0 concrete\nwall 4 0 4 8 concrete\nwall 4 8 0 8 concrete\nwall 0 8 0 4 concrete\n"
    "source S1 1 1 1.5 0 0 0 0\nsource S2 3 1 1.5 0 0 0 0\nreceiver R1 2 4 1.5 0 0 0 0\nlistener 2 4 1.5 0\n";

static void pump(VerbProcessor& p, int blocks) {
    juce::AudioBuffer<float> buf(2, 128); juce::MidiBuffer midi;
    for (int i = 0; i < blocks; ++i) { buf.clear(); stage2Process(p, buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(2); }
}
static bool waitState(VerbProcessor& p, const char* a, const char* b, double seconds) {
    const auto until = juce::Time::getMillisecondCounterHiRes() + seconds * 1000;
    while (juce::Time::getMillisecondCounterHiRes() < until) {
        const auto s = p.pathBakeStatus()["state"].toString();
        if (s == a || (b && s == b)) return true;
        pump(p, 4); juce::Thread::sleep(2);
    }
    const auto s = p.pathBakeStatus()["state"].toString();
    return s == a || (b && s == b);
}
static bool waitBaked(VerbProcessor& p, double seconds) { return waitState(p, "baked", "approximate", seconds); }
// Settled unit-impulse response at a path phase; 200 blocks x 128 samples, interleaved L/R.
static std::vector<float> impulse(VerbProcessor& p, const char* pathParam, double u) {
    setP(p, pathParam, u);
    auto eng = E9bAccess::engine(p);
    juce::AudioBuffer<float> buf(2, 128); juce::MidiBuffer midi;
    const auto until = juce::Time::getMillisecondCounterHiRes() + 15000;
    while (juce::Time::getMillisecondCounterHiRes() < until) {
        buf.clear(); stage2Process(p, buf, midi); juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        if (eng->tailIsCurrent()) break;
    }
    for (int i = 0; i < (int)(0.3 * 48000 / 128); ++i) { buf.clear(); stage2Process(p, buf, midi); }
    std::vector<float> ir; ir.reserve(2 * 128 * 200);
    bool fired = false;
    for (int b = 0; b < 200; ++b) {
        buf.clear();
        if (!fired) { buf.setSample(0, 0, 1.f); buf.setSample(1, 0, 1.f); fired = true; }
        stage2Process(p, buf, midi);
        for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i) ir.push_back(buf.getSample(c, i));
    }
    return ir;
}
static double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 1e30;
    double d = 0; for (size_t i = 0; i < a.size(); ++i) d = std::max(d, (double)std::fabs(a[i] - b[i]));
    return d;
}
// Load a raw state XML string into a fresh prepared processor; returns ready.
static bool loadStateXml(VerbProcessor& q, const juce::String& xmlText) {
    juce::MemoryBlock data;
    auto xml = juce::parseXML(xmlText);
    if (!xml) return false;
    juce::AudioProcessor::copyXmlToBinary(*xml, data);
    q.setStateInformation(data.getData(), (int)data.getSize());
    return waitReady(q, 120);
}
static juce::String stateXml(VerbProcessor& p) {
    juce::MemoryBlock data; p.getStateInformation(data);
    auto xml = p.getXmlFromBinary(data.getData(), (int)data.getSize());
    return xml ? xml->toString() : juce::String();
}
// Patch the single-table blob and recompute its FNV-1a checksum (offsets: header 12, contentSig u64 @12,
// estimator i32 @20, checksum u64 at the last 8 bytes).
static std::vector<uint8_t> patchedBlob(const std::vector<uint8_t>& blob, size_t offset, const void* bytes, size_t n) {
    std::vector<uint8_t> b = blob;
    std::memcpy(b.data() + offset, bytes, n);
    uint64_t h = verb::bakeFnv1a(b.data() + 12, b.size() - 12 - 8);
    for (int i = 0; i < 8; ++i) b[b.size() - 8 + i] = (uint8_t)(h >> (8 * i));
    return b;
}
// getStateInformation XML has <VerbState> as its ROOT; a .verbroom preset wraps it in <VerbRoomPreset>.
static juce::XmlElement* verbStateOf(juce::XmlElement* xml) {
    if (!xml) return nullptr;
    return xml->hasTagName("VerbState") ? xml : xml->getChildByName("VerbState");
}
static juce::String blobToBakeCacheXml(const juce::String& baseStateXml, const std::vector<uint8_t>& blob, int version) {
    auto xml = juce::parseXML(baseStateXml);
    auto* st = verbStateOf(xml.get());
    if (!st) return {};
    while (auto* bc = st->getChildByName("BakeCache")) st->removeChildElement(bc, true);
    juce::MemoryBlock raw(blob.data(), blob.size());
    auto* bc = new juce::XmlElement("BakeCache");
    bc->setAttribute("v", version);
    bc->setAttribute("data", raw.toBase64Encoding());
    st->addChildElement(bc);
    return xml->toString();
}
}  // namespace p4test

// (a) bake -> save state -> new processor -> load -> Baked, signature equal, audio equal; preset path too.
// (c) old state without the child; (d) corrupted/truncated/foreign-version child; (e) estimator 0 / foreign
// signature never installed; (f) size cap. All on one baked small scene to keep the suite fast.
static int testP4Persist() {
    using namespace p4test;
    const int before = fails;
    VerbProcessor p; p.setTailSettings({8000, false}); p.setPlayConfigDetails(2, 2, 48000, 128); p.prepareToPlay(48000, 128);
    check(waitReady(p, 90), "P4 processor ready");
    p.loadSceneText(kScene, "P4 corridor", true);
    check(waitReady(p, 90), "P4 corridor loads");
    verbp::MovementPath lp; lp.points = {{{1, 2, 1.5}}, {{3, 6, 1.5}}};
    check(p.setMovementPath(verbp::kListener, lp), "P4 listener path accepted");
    check(p.startPathBake(verbp::kListener), "P4 bake starts");
    check(waitBaked(p, 180), "P4 bake completes");
    auto eng = E9bAccess::engine(p);
    const auto st0 = eng->bakeStats();
    const auto sig0 = eng->bakeContentSignatures();
    auto t0 = eng->bakedTableSnapshot();
    check(st0.hash != 0 && t0 && t0->contentSig != 0 && sig0.size() == 1 && sig0[0] == t0->contentSig,
          "P4 baked table carries the current content signature");
    std::printf("P4 sig=%016llx hash=%016llx nodes=%zu rays=%d\n", (unsigned long long)sig0[0],
                (unsigned long long)st0.hash, t0 ? t0->u.size() : 0, t0 ? t0->selectedRays : 0);
    std::vector<verb::BakedTable> tables; for (const auto& t : eng->bakedTablesSnapshot()) if (t) tables.push_back(*t);
    const auto bin = verb::bakeSerializeTables(tables);
    std::printf("P4 serialized bytes=%zu cap=%zu\n", bin.size(), verb::kBakePersistMaxBytes);
    check(!bin.empty() && bin.size() <= verb::kBakePersistMaxBytes, "P4 serialized table fits the size cap");
    std::vector<verb::BakedTable> back;
    check(verb::bakeDeserializeTables(bin.data(), bin.size(), back) && back.size() == 1, "P4 binary round trip parses");
    check(back.size() == 1 && back[0].hash() == t0->hash() && back[0].contentSig == t0->contentSig,
          "P4 round trip preserves the payload hash and signature");
    check(back.size() == 1 && back[0].amplitude.size() == back[0].energy.size(), "P4 round trip rebuilds amplitude");
    check(back.size() == 1 && back[0].winEnergy == t0->winEnergy && back[0].winEss == t0->winEss &&
              back[0].winAmplitude.size() == back[0].winEnergy.size(),
          "P4 round trip preserves 4-window energy/ESS and rebuilds winAmplitude");
    const auto irBefore = impulse(p, "lpath", 0.5);
    const juce::String xml0 = stateXml(p);
    check(xml0.contains("BakeCache"), "P4 project state carries the BakeCache child");

    // (a) full project-state restore in a fresh processor
    VerbProcessor q; q.setPlayConfigDetails(2, 2, 48000, 128); q.prepareToPlay(48000, 128);
    check(waitReady(q, 90), "P4 reload processor ready");
    check(loadStateXml(q, xml0), "P4 project state reloads");
    check(waitBaked(q, 15), "P4 reloaded project is Baked without a worker");
    auto qe = E9bAccess::engine(q);
    const auto stq = qe->bakeStats();
    check(stq.hash == st0.hash && stq.hash != 0, "P4 restored table hash equals the saved one");
    check(stq.restored == 1, "P4 restored flag marks the saved-project source");
    const auto sigq = qe->bakeContentSignatures();
    check(sigq.size() == 1 && sigq[0] == sig0[0], "P4 same content gives the same signature in another processor");
    check(qe->bakedCacheEntries() >= 1, "P4 restored table entered the undo cache");
    check(q.pathBakeStatus()["reason"].toString().contains("restored from the saved project"), "P4 status hints at the saved-project restore");
    const auto irAfter = impulse(q, "lpath", 0.5);
    const double d = maxDiff(irBefore, irAfter);
    std::printf("P4 impulse maxdiff=%.9g\n", d);
    check(d <= 1e-5, "P4 audio after reload matches before save (float rounding)");
    q.releaseResources();

    // (a2) the exported .verbroom preset goes through the same path
    const juce::String preset = p.getRoomPresetText();
    check(preset.contains("BakeCache"), "P4 exported preset carries the BakeCache child");
    VerbProcessor r; r.setPlayConfigDetails(2, 2, 48000, 128); r.prepareToPlay(48000, 128);
    check(waitReady(r, 90), "P4 preset processor ready");
    juce::String why;
    check(r.loadRoomPresetText(preset, why) && waitReady(r, 120), "P4 preset with a bake reloads");
    check(waitBaked(r, 15), "P4 reloaded preset is Baked");
    check(E9bAccess::engine(r)->bakeStats().hash == st0.hash, "P4 preset restores the same table");
    r.releaseResources();

    // (c) old state without the child loads exactly as before
    {
        auto xml = juce::parseXML(xml0);
        auto* st = verbStateOf(xml.get());
        if (st) while (auto* bc = st->getChildByName("BakeCache")) st->removeChildElement(bc, true);
        VerbProcessor o; o.setPlayConfigDetails(2, 2, 48000, 128); o.prepareToPlay(48000, 128);
        check(waitReady(o, 90), "P4 old-state processor ready");
        check(xml && loadStateXml(o, xml->toString()), "P4 old state (no BakeCache) loads");
        check(waitState(o, "notBaked", nullptr, 5) && o.pathBakePercent() == 0, "P4 old state stays notBaked");
        check(o.getMovementPath(verbp::kListener).points.size() == 2, "P4 old state keeps the path");
        o.releaseResources();
    }
    // (d) truncated / corrupted / foreign-version children are ignored safely
    {
        auto xml = juce::parseXML(xml0);
        auto* st = verbStateOf(xml.get());
        auto* bc = st ? st->getChildByName("BakeCache") : nullptr;
        const juce::String data = bc ? bc->getStringAttribute("data") : juce::String();
        check(data.isNotEmpty(), "P4 fixture blob present");
        auto truncated = juce::parseXML(xml0);
        verbStateOf(truncated.get())->getChildByName("BakeCache")->setAttribute("data", data.substring(0, data.length() / 2));
        VerbProcessor o1; o1.setPlayConfigDetails(2, 2, 48000, 128); o1.prepareToPlay(48000, 128); waitReady(o1, 90);
        check(loadStateXml(o1, truncated->toString()) && waitState(o1, "notBaked", nullptr, 5), "P4 truncated BakeCache ignored (notBaked, no crash)");
        o1.releaseResources();
        auto corrupt = juce::parseXML(xml0);
        juce::String bad = data; const int mid = bad.length() / 2;
        bad = bad.substring(0, mid) + (bad[mid] == 'A' ? 'B' : 'A') + bad.substring(mid + 1);
        verbStateOf(corrupt.get())->getChildByName("BakeCache")->setAttribute("data", bad);
        VerbProcessor o2; o2.setPlayConfigDetails(2, 2, 48000, 128); o2.prepareToPlay(48000, 128); waitReady(o2, 90);
        check(loadStateXml(o2, corrupt->toString()) && waitState(o2, "notBaked", nullptr, 5), "P4 corrupted BakeCache ignored (checksum)");
        o2.releaseResources();
        auto foreign = juce::parseXML(xml0);
        verbStateOf(foreign.get())->getChildByName("BakeCache")->setAttribute("v", 99);
        VerbProcessor o3; o3.setPlayConfigDetails(2, 2, 48000, 128); o3.prepareToPlay(48000, 128); waitReady(o3, 90);
        check(loadStateXml(o3, foreign->toString()) && waitState(o3, "notBaked", nullptr, 5), "P4 foreign-version BakeCache ignored");
        o3.releaseResources();
    }
    // (e) a pre-P3/old estimator stamp (0 or 2) or a foreign content signature is never installed
    {
        const int curV = (int)verb::kBakePersistVersion;
        const int32_t zero = 0;
        const auto estBlob = patchedBlob(bin, 20, &zero, 4);
        VerbProcessor o; o.setPlayConfigDetails(2, 2, 48000, 128); o.prepareToPlay(48000, 128); waitReady(o, 90);
        check(loadStateXml(o, blobToBakeCacheXml(xml0, estBlob, curV)) && waitState(o, "notBaked", nullptr, 5),
              "P4 estimator-0 table is never installed");
        o.releaseResources();
        const int32_t prevEst = verb::kBakeEstimatorVersion - 1;
        const auto prevEstBlob = patchedBlob(bin, 20, &prevEst, 4);
        VerbProcessor oPrevEst; oPrevEst.setPlayConfigDetails(2, 2, 48000, 128); oPrevEst.prepareToPlay(48000, 128); waitReady(oPrevEst, 90);
        check(loadStateXml(oPrevEst, blobToBakeCacheXml(xml0, prevEstBlob, curV)) && waitState(oPrevEst, "notBaked", nullptr, 5),
              "P4 previous-estimator table is rejected cleanly (notBaked)");
        oPrevEst.releaseResources();
        if (curV > 1) {
            VerbProcessor oOldV; oOldV.setPlayConfigDetails(2, 2, 48000, 128); oOldV.prepareToPlay(48000, 128); waitReady(oOldV, 90);
            check(loadStateXml(oOldV, blobToBakeCacheXml(xml0, bin, 1)) && waitState(oOldV, "notBaked", nullptr, 5),
                  "P4 previous-persist-version (v1) table is rejected cleanly (notBaked)");
            oOldV.releaseResources();
        }
        const uint64_t foreignSig = ~sig0[0];
        const auto sigBlob = patchedBlob(bin, 12, &foreignSig, 8);
        VerbProcessor o2; o2.setPlayConfigDetails(2, 2, 48000, 128); o2.prepareToPlay(48000, 128); waitReady(o2, 90);
        check(loadStateXml(o2, blobToBakeCacheXml(xml0, sigBlob, curV)) && waitState(o2, "notBaked", nullptr, 5),
              "P4 table with a foreign content signature is never installed");
        o2.releaseResources();
    }
    // (f) size cap: an oversize child is ignored; the serializer reports oversize payloads honestly
    {
        const int curV = (int)verb::kBakePersistVersion;
        std::vector<uint8_t> huge(verb::kBakePersistMaxBytes + 1, 0);
        VerbProcessor o; o.setPlayConfigDetails(2, 2, 48000, 128); o.prepareToPlay(48000, 128); waitReady(o, 90);
        check(loadStateXml(o, blobToBakeCacheXml(xml0, huge, curV)) && waitState(o, "notBaked", nullptr, 5),
              "P4 oversize BakeCache ignored (size guard)");
        o.releaseResources();
        verb::BakedTable fat = *t0;
        fat.energy.assign(verb::kBakePersistMaxBytes / 8 + 16, 1.0);
        fat.u.assign(2, 0.0); fat.u[1] = 1.0;
        const auto fatBin = verb::bakeSerializeTables({fat});
        check(fatBin.size() > verb::kBakePersistMaxBytes, "P4 serializer reports an oversize payload (processor would skip it)");
    }
    p.releaseResources();
    return fails == before ? 0 : 1;
}

// (b) bake -> relevant edit -> Outdated -> Undo (the real command/undo path) -> Baked again, no worker.
// Also: irrelevant edits keep the signature; geometry undo restores although session counters never return;
// an explicit clear empties the cache and stays notBaked; the audio thread never allocates for a restore.
static int testP4Undo() {
    using namespace p4test;
    const int before = fails;
    VerbProcessor p; p.setTailSettings({8000, false}); p.setPlayConfigDetails(2, 2, 48000, 128); p.prepareToPlay(48000, 128);
    check(waitReady(p, 90), "P4 undo processor ready");
    p.loadSceneText(kScene, "P4 corridor", true);
    check(waitReady(p, 90), "P4 undo corridor loads");
    verbp::MovementPath sp; sp.points = {{{1, 1, 1.5}}, {{1, 5, 1.5}}};
    check(p.setMovementPath(0, sp) && p.startPathBake(0), "P4 source-path bake starts");
    check(waitBaked(p, 180), "P4 source-path bake completes");
    auto eng = E9bAccess::engine(p);
    const auto st0 = eng->bakeStats();
    const auto sig0 = eng->bakeContentSignatures();
    const auto resets0 = st0.resets, stations0 = st0.stations; const double ms0 = st0.milliseconds;
    check(sig0.size() == 1 && sig0[0] != 0, "P4 undo fixture has one signed group");
    auto& session = p.getEditorSession();
    check(session.run(juce::JSON::parse(R"({"op":"listener.set","xM":3.5,"yM":7,"zM":1.5})")).ok, "P4 listener move accepted (command path)");
    check(waitState(p, "outdated", nullptr, 15), "P4 static-listener move invalidates the bake");
    const auto sigMoved = eng->bakeContentSignatures();
    check(sigMoved.size() == 1 && sigMoved[0] != sig0[0], "P4 relevant edit changes the signature");
    setP(p, "makeup", 3); setP(p, "mix", 0.7);
    const auto sigUi = eng->bakeContentSignatures();
    check(sigUi.size() == 1 && sigUi[0] == sigMoved[0], "P4 output gain/mix do not change the signature");
    setP(p, "makeup", 0); setP(p, "mix", 1);
    const auto alloc = stage2AudioAllocations;
    check(session.undo().ok, "P4 history.undo accepted");
    check(waitBaked(p, 15), "P4 undo restores Baked without a re-bake");
    const auto st1 = eng->bakeStats();
    check(st1.hash == st0.hash, "P4 undo restores the same table");
    check(st1.restored == 2, "P4 restored flag marks the undo cache");
    check(st1.resets == resets0 && st1.stations == stations0 && st1.milliseconds == ms0, "P4 no worker started for the undo restore");
    check(stage2AudioAllocations == alloc, "P4 undo restore allocates nothing in processBlock");
    check(eng->bakeContentSignatures()[0] == sig0[0], "P4 undo returns to the original signature");
    check(p.pathBakeStatus()["reason"].toString().contains("restored after undo"), "P4 status hints at the undo restore");
    check(session.redo().ok, "P4 redo accepted");
    check(waitState(p, "outdated", nullptr, 15), "P4 redo invalidates again");
    check(session.undo().ok, "P4 second undo accepted");
    check(waitBaked(p, 15), "P4 second undo restores Baked again");
    // Geometry edit + undo: geomSerial never returns, the CONTENT does -> the cache must still restore.
    check(session.run(juce::JSON::parse(R"({"op":"surface.moveVertex","ref":{"kind":"wall","index":0},"vertex":0,"positionM":[0.2,4]})")).ok,
          "P4 wall vertex move accepted");
    check(waitState(p, "outdated", nullptr, 20), "P4 geometry edit invalidates the bake");
    check(session.undo().ok, "P4 geometry undo accepted");
    check(waitBaked(p, 20), "P4 geometry undo restores Baked from the cache (content, not counters)");
    check(eng->bakeStats().hash == st0.hash, "P4 geometry undo restores the identical table");
    check(eng->bakeStats().restored == 2, "P4 geometry restore is marked as undo-cache source");
    // Explicit clear is never silently undone.
    p.clearPathBake();
    check(waitState(p, "notBaked", nullptr, 15), "P4 clear returns to notBaked");
    pump(p, 300);
    check(p.pathBakeStatus()["state"].toString() == "notBaked", "P4 cleared bake stays notBaked (cache emptied)");
    check(eng->bakedCacheEntries() == 0, "P4 clear empties the undo cache");
    p.releaseResources();
    return fails == before ? 0 : 1;
}

// Multi-group scene: two independent source paths -> two tables; measures the combined serialized size and
// verifies that save/load restores BOTH groups (the brief's bigger multi-group measurement).
static int testP4MultiGroup() {
    using namespace p4test;
    const int before = fails;
    VerbProcessor p; p.setTailSettings({8000, false}); p.setPlayConfigDetails(2, 2, 48000, 128); p.prepareToPlay(48000, 128);
    check(waitReady(p, 90), "P4 multi processor ready");
    p.loadSceneText(kScene2, "P4 corridor x2", true);
    check(waitReady(p, 90), "P4 two-source corridor loads");
    verbp::MovementPath a; a.points = {{{1, 1, 1.5}}, {{1, 5, 1.5}}};
    verbp::MovementPath b; b.points = {{{3, 1, 1.5}}, {{3, 5, 1.5}}};
    check(p.setMovementPath(0, a) && p.setMovementPath(1, b), "P4 two source paths accepted");
    check(p.startPathBake(-1), "P4 multi-group bake starts");
    check(waitBaked(p, 240), "P4 multi-group bake completes");
    auto eng = E9bAccess::engine(p);
    const auto st0 = eng->bakeStats();
    check(st0.groups.size() == 2, "P4 two bake groups published");
    std::vector<verb::BakedTable> tables; for (const auto& t : eng->bakedTablesSnapshot()) if (t) tables.push_back(*t);
    const auto bin = verb::bakeSerializeTables(tables);
    std::printf("P4 multi-group tables=%zu serialized bytes=%zu cap=%zu\n", tables.size(), bin.size(), verb::kBakePersistMaxBytes);
    check(tables.size() == 2 && bin.size() <= verb::kBakePersistMaxBytes, "P4 multi-group payload fits the cap");
    const juce::String xml0 = stateXml(p);
    VerbProcessor q; q.setPlayConfigDetails(2, 2, 48000, 128); q.prepareToPlay(48000, 128);
    check(waitReady(q, 90), "P4 multi reload processor ready");
    check(loadStateXml(q, xml0), "P4 multi-group state reloads");
    check(waitBaked(q, 15), "P4 multi-group state restores Baked");
    auto qe = E9bAccess::engine(q);
    check(qe->bakeStats().hash == st0.hash, "P4 multi-group restore keeps the combined hash");
    check(qe->bakeStats().groups.size() == 2, "P4 both groups restored");
    check(qe->bakeStats().restored == 1, "P4 multi-group restore marked as saved-project source");
    q.releaseResources(); p.releaseResources();
    return fails == before ? 0 : 1;
}