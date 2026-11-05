// Robustness / stress tests of the plugin processor (no host needed). Everything a real host or a real user can do
// in a bad moment: odd sample rates and block sizes, bus layouts, rapid re-preparation and destruction, corrupt
// or foreign project state, extreme parameter values, NaN input, and (the hard one) edits from the UI thread
// while the audio thread keeps running.
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <thread>

#include "PluginProcessor.h"

static int fails = 0;
static void check(bool ok, const juce::String& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (!ok) ++fails;
}

static bool waitReady(VerbProcessor& p, double seconds) {
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while ((juce::Time::getMillisecondCounterHiRes() - t0) < seconds * 1000) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        if (p.getLoadState() == 2) return true;
        if (p.getLoadState() == 3) return false;
    }
    return false;
}

struct Rig {
    VerbProcessor p;
    Rig(int in, int out, double sr, int block) {
        p.setPlayConfigDetails(in, out, sr, block);
        p.prepareToPlay(sr, block);
    }
};

static bool allFinite(const juce::AudioBuffer<float>& b, int channels) {
    for (int ch = 0; ch < channels && ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (!std::isfinite(b.getSample(ch, i))) return false;
    return true;
}

static std::string demoSceneText() {
    juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile("scenes").getChildFile("demo_furnished.txt");
    return f.existsAsFile() ? f.loadFileAsString().toStdString() : std::string();
}

// ---------------------------------------------------------------------------------------------------------------
static void testRatesAndBlocks() {
    std::printf("S1: sample rates and block sizes (also blocks larger than the host announced)\n");
    const double rates[] = {22050, 44100, 48000, 88200, 96000, 192000};
    const int blocks[] = {1, 2, 3, 7, 31, 64, 100, 127, 128, 129, 255, 256, 511, 512, 513, 1000, 2047, 4096};
    for (double sr : rates) {
        Rig r(4, 2, sr, 512);
        const bool ready = waitReady(r.p, 180);
        if (!ready) { check(false, juce::String(sr, 0) + " Hz: ready"); continue; }
        bool fin = true;
        long pos = 0, first = -1;
        size_t bi = 0;
        const long total = (long)(0.25 * sr);
        juce::MidiBuffer midi;
        while (pos < total) {
            const int n = blocks[bi++ % (sizeof blocks / sizeof blocks[0])];
            juce::AudioBuffer<float> b(4, n);
            b.clear();
            if (pos == 0) b.setSample(0, 0, 1.f);
            r.p.processBlock(b, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i) {
                    const float v = b.getSample(ch, i);
                    fin &= std::isfinite(v);
                    if (first < 0 && std::fabs(v) > 1e-3f) first = pos + i;
                }
            pos += n;
        }
        const double expect = 4.0009 / 343.2 * sr;
        std::printf("    %.0f Hz: first arrival %ld, expected %.0f\n", sr, first, expect);
        check(fin && first > 0 && std::fabs((double)first - expect) < 6.0 + sr / 24000.0, juce::String(sr, 0) + " Hz: finite, direct sound at the right time");
    }
}

static void testBlockIndependence() {
    std::printf("S2: the result must not depend on how the host chops the audio\n");
    Rig a(4, 2, 48000, 512), b(4, 2, 48000, 512);
    check(waitReady(a.p, 90) && waitReady(b.p, 90), "both instances ready");
    const int N = 48000;
    std::mt19937 rng(1);
    std::normal_distribution<float> nd(0, 0.1f);
    std::vector<float> x((size_t)N);
    for (auto& v : x) v = nd(rng);
    auto run = [&](VerbProcessor& p, const std::function<int()>& nextBlock) {
        std::vector<float> out((size_t)N * 2);
        juce::MidiBuffer midi;
        long pos = 0;
        while (pos < N) {
            const int n = (int)std::min<long>(nextBlock(), N - pos);
            juce::AudioBuffer<float> buf(4, n);
            buf.clear();
            for (int i = 0; i < n; ++i) buf.setSample(0, i, x[(size_t)(pos + i)]);
            p.processBlock(buf, midi);
            for (int i = 0; i < n; ++i) { out[(size_t)(pos + i)] = buf.getSample(0, i); out[(size_t)(N + pos + i)] = buf.getSample(1, i); }
            pos += n;
        }
        return out;
    };
    const auto oa = run(a.p, [] { return 64; });
    std::mt19937 r2(5);
    const int sizes[] = {1, 7, 100, 512, 3000, 37};
    const auto ob = run(b.p, [&] { return sizes[r2() % 6]; });
    double maxd = 0, peak = 0;
    size_t maxIdx = 0;
    for (size_t i = 0; i < oa.size(); ++i) {
        const double d = (double)std::fabs(oa[i] - ob[i]);
        if (d > maxd) { maxd = d; maxIdx = i; }
        peak = std::max(peak, (double)std::fabs(oa[i]));
    }
    std::printf("    max difference %.2e (peak %.3f) at sample %zu of channel %d\n", maxd, peak, maxIdx % (size_t)N, (int)(maxIdx / (size_t)N));
    check(peak > 0.01 && maxd < 1e-4, "block size 64 and random block sizes 1..3000 give the same output");
}

static void testBusLayouts() {
    std::printf("S3: bus layouts\n");
    VerbProcessor p;
    auto mk = [](int in, int out) {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add(in > 0 ? juce::AudioChannelSet::discreteChannels(in) : juce::AudioChannelSet::disabled());
        l.outputBuses.add(out > 0 ? juce::AudioChannelSet::discreteChannels(out) : juce::AudioChannelSet::disabled());
        return l;
    };
    check(p.isBusesLayoutSupported(mk(0, 1)) && p.isBusesLayoutSupported(mk(1, 1)) && p.isBusesLayoutSupported(mk(2, 2)) && p.isBusesLayoutSupported(mk(32, 16)),
          "mono, stereo, no inputs and 32 in / 16 out are accepted");
    check(p.isBusesLayoutSupported(mk(64, 2)) && !p.isBusesLayoutSupported(mk(65, 2)) && !p.isBusesLayoutSupported(mk(2, 17)) && !p.isBusesLayoutSupported(mk(2, 0)), "64 inputs are accepted; 65 inputs, 17 outputs and no output are refused");
    const int cfgs[][2] = {{0, 1}, {1, 1}, {2, 2}, {8, 12}, {32, 16}};
    for (auto& c : cfgs) {
        Rig r(c[0], c[1], 48000, 256);
        const bool ready = waitReady(r.p, 120);
        bool fin = true;
        juce::MidiBuffer midi;
        for (int s = 0; s < 40; ++s) {
            juce::AudioBuffer<float> b(std::max(c[0], c[1]), 256);
            b.clear();
            if (c[0] > 0 && s == 0) b.setSample(0, 0, 1.f);
            r.p.processBlock(b, midi);
            fin &= allFinite(b, c[1]);
        }
        check(ready && fin, juce::String(c[0]) + " in / " + juce::String(c[1]) + " out: loads and renders finite audio");
    }
    {   // zero-length blocks must be harmless
        Rig r(2, 2, 48000, 256);
        waitReady(r.p, 60);
        juce::AudioBuffer<float> b(2, 0);
        juce::MidiBuffer midi;
        r.p.processBlock(b, midi);
        check(true, "a zero-sample block is harmless");
    }
}

static void testLifecycle() {
    std::printf("S4: destruction while loading, re-preparation storms\n");
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) {
        VerbProcessor q;
        q.setPlayConfigDetails(2, 2, 48000, 256);
        q.prepareToPlay(48000, 256);
        juce::Thread::sleep(i * 120);
    }  // each one is destroyed while its loader is (probably) still building the engine
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("    5 create/destroy cycles took %.1f s\n", dt);
    check(dt < 120, "destroying a processor mid-load neither hangs nor crashes");

    Rig r(4, 2, 48000, 256);
    const struct { double sr; int block; } steps[] = {{44100, 64}, {96000, 1024}, {48000, 128}, {88200, 512}, {48000, 256}};
    juce::MidiBuffer midi;
    bool okAll = true;
    for (auto& s : steps) {
        r.p.setPlayConfigDetails(4, 2, s.sr, s.block);
        r.p.prepareToPlay(s.sr, s.block);
        for (int i = 0; i < 3; ++i) {  // processing right away, while the new engine is still being built
            juce::AudioBuffer<float> b(4, s.block);
            b.clear();
            r.p.processBlock(b, midi);
            okAll &= allFinite(b, 2);
        }
        const bool ready = waitReady(r.p, 120);
        long first = -1, pos = 0;
        for (int blk = 0; blk < 200 && first < 0; ++blk) {
            juce::AudioBuffer<float> b(4, s.block);
            b.clear();
            if (blk == 0) b.setSample(0, 0, 1.f);
            r.p.processBlock(b, midi);
            okAll &= allFinite(b, 2);
            for (int i = 0; i < s.block && first < 0; ++i) if (std::fabs(b.getSample(0, i)) > 1e-3f || std::fabs(b.getSample(1, i)) > 1e-3f) first = pos + i;
            pos += s.block;
        }
        const double expect = 4.0009 / 343.2 * s.sr;
        const bool right = ready && first > 0 && std::fabs((double)first - expect) < 6.0 + s.sr / 24000.0;
        std::printf("    -> %.0f Hz / %d: first arrival %ld (expected %.0f)\n", s.sr, s.block, first, expect);
        okAll &= right;
    }
    check(okAll, "five re-preparations with other rates and block sizes: finite, and the engine follows the new rate");
}

static void testStateRobustness() {
    std::printf("S5: corrupt, foreign and extreme project states\n");
    auto tryState = [&](const void* d, int n, const char* what) {
        VerbProcessor q;
        q.setPlayConfigDetails(2, 2, 48000, 256);
        q.prepareToPlay(48000, 256);
        q.setStateInformation(d, n);
        waitReady(q, 90);  // an error state is fine, a crash or a hang is not
        juce::MidiBuffer midi;
        bool fin = true;
        for (int s = 0; s < 20; ++s) {
            juce::AudioBuffer<float> b(2, 256);
            b.clear();
            b.setSample(0, 0, 1.f);
            q.processBlock(b, midi);
            fin &= allFinite(b, 2);
        }
        check(fin, what);
    };
    tryState(nullptr, 0, "empty state");
    {
        std::mt19937 rng(9);
        std::vector<char> junk(1000);
        for (auto& c : junk) c = (char)(rng() & 0xff);
        tryState(junk.data(), (int)junk.size(), "random bytes");
    }
    {
        juce::XmlElement x("Other");
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary(x, mb);
        tryState(mb.getData(), (int)mb.getSize(), "valid XML of a foreign type");
    }
    {   // a valid state, cut in half
        VerbProcessor q;
        juce::MemoryBlock mb;
        q.getStateInformation(mb);
        tryState(mb.getData(), (int)mb.getSize() / 2, "a valid state truncated to half");
    }
    {   // old/minimal state: only the scene text, no sources, no output, an unknown parameter
        juce::ValueTree t("VerbState");
        t.setProperty("sceneText", juce::String(demoSceneText()), nullptr);
        t.setProperty("sceneName", "old project", nullptr);
        juce::ValueTree prm("PARAM");
        prm.setProperty("id", "inputmode", nullptr);
        prm.setProperty("value", 1.0, nullptr);
        t.appendChild(prm, nullptr);
        auto xml = t.createXml();
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary(*xml, mb);
        tryState(mb.getData(), (int)mb.getSize(), "an old state without source/output sections and with an unknown parameter");
    }
    {   // absurd values everywhere
        juce::ValueTree t("VerbState");
        t.setProperty("sceneText", juce::String("sr 1\nlength 1e9\nheight -5\ntemp 1e9\nhumidity -50\ndetect_radius 0\nmaterial m 2 -1 5 9 9 9 9 7\nfloor m\nwall 0 0 0 0 m\nwall nan inf 1 1 m\nclutter 1000000000 -1 -2 5 1 1\nsource s 1e30 nan 1\nreceiver r 0 0 0\n"), nullptr);
        juce::ValueTree srcs("Sources");
        for (int i = 0; i < 100; ++i) {
            juce::ValueTree v("Src");
            v.setProperty("name", "x", nullptr);
            v.setProperty("format", 99, nullptr);
            v.setProperty("channels", 1000, nullptr);
            v.setProperty("firstIn", 999, nullptr);
            v.setProperty("spread", 1e9, nullptr);
            v.setProperty("free", true, nullptr);
            v.setProperty("ox", "1e30,nan,inf,-inf", nullptr);
            srcs.appendChild(v, nullptr);
        }
        t.appendChild(srcs, nullptr);
        juce::ValueTree out("Output");
        out.setProperty("layout", 99, nullptr);
        out.setProperty("radius", 1e9, nullptr);
        t.appendChild(out, nullptr);
        auto xml = t.createXml();
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary(*xml, mb);
        tryState(mb.getData(), (int)mb.getSize(), "absurd values (sizes, NaN, huge counts, bad layout) are clamped or rejected, not trusted");
    }
}

static void testExtremesAndNaN() {
    std::printf("S6: extreme parameters, NaN/Inf input\n");
    Rig r(8, 2, 48000, 256);
    check(waitReady(r.p, 90), "ready");
    std::mt19937 rng(77);
    juce::MidiBuffer midi;
    bool fin = true;
    double peak = 0;
    for (int it = 0; it < 30; ++it) {
        for (auto* prm : r.p.getParameters()) prm->setValueNotifyingHost((rng() & 1) ? 1.f : 0.f);  // every parameter at an extreme
        for (int s = 0; s < 25; ++s) {
            juce::AudioBuffer<float> b(8, 256);
            b.clear();
            if (s == 0) b.setSample(0, 0, 1.f);
            std::normal_distribution<float> nd(0, 0.3f);
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 256; ++i) b.setSample(ch, i, b.getSample(ch, i) + nd(rng));
            r.p.processBlock(b, midi);
            fin &= allFinite(b, 2);
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 256; ++i) peak = std::max(peak, (double)std::fabs(b.getSample(ch, i)));
        }
    }
    std::printf("    peak over 30 sets of extreme parameter values: %.2f\n", peak);
    check(fin && peak < 1e4, "all parameters at their limits: finite and bounded");
    for (auto* prm : r.p.getParameters()) prm->setValueNotifyingHost(prm->getDefaultValue());

    // NaN / Inf / huge samples from upstream must not poison the plugin
    for (int s = 0; s < 40; ++s) {  // let the state settle first
        juce::AudioBuffer<float> b(8, 256);
        b.clear();
        r.p.processBlock(b, midi);
    }
    {
        juce::AudioBuffer<float> b(8, 256);
        b.clear();
        for (int ch = 0; ch < 8; ++ch) {
            b.setSample(ch, 3, std::numeric_limits<float>::quiet_NaN());
            b.setSample(ch, 10, std::numeric_limits<float>::infinity());
            b.setSample(ch, 20, -std::numeric_limits<float>::infinity());
            b.setSample(ch, 30, 1e30f);
        }
        r.p.processBlock(b, midi);
        check(allFinite(b, 2), "a block full of NaN/Inf/1e30 produces finite output");
    }
    bool recovered = true;
    double energy = 0;
    for (int s = 0; s < 60; ++s) {
        juce::AudioBuffer<float> b(8, 256);
        b.clear();
        if (s == 0) b.setSample(0, 0, 1.f);
        r.p.processBlock(b, midi);
        recovered &= allFinite(b, 2);
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 256; ++i) energy += (double)b.getSample(ch, i) * b.getSample(ch, i);
    }
    check(recovered && energy > 1e-6, "afterwards the plugin is not poisoned: normal input gives normal output again");
}

static void testThreadedFuzz() {
    std::printf("S7: edits from the UI thread while the audio thread runs (races, deadlocks, crashes)\n");
    VerbProcessor p;
    p.setPlayConfigDetails(32, 16, 48000, 256);
    p.prepareToPlay(48000, 256);
    check(waitReady(p, 90), "ready before the fuzz");
    std::atomic<bool> stop{false}, bad{false};
    std::atomic<long> blocks{0};
    std::thread audio([&] {
        juce::AudioBuffer<float> b(32, 256);
        juce::MidiBuffer midi;
        std::mt19937 rng(3);
        std::normal_distribution<float> nd(0, 0.05f);
        while (!stop) {
            for (int ch = 0; ch < 32; ++ch) { auto* d = b.getWritePointer(ch); for (int i = 0; i < 256; ++i) d[i] = nd(rng); }
            p.processBlock(b, midi);
            if (!allFinite(b, 16)) bad = true;
            ++blocks;
            std::this_thread::sleep_for(std::chrono::microseconds(800));
        }
    });
    std::mt19937 rng(42);
    auto rnd = [&](int n) { return (int)(rng() % (unsigned)n); };
    auto rf = [&](double a, double b2) { return a + (b2 - a) * ((rng() & 0xFFFFFF) / 16777215.0); };
    const std::string demo = demoSceneText();
    const auto t0 = std::chrono::steady_clock::now();
    int ops = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 25.0) {
        ++ops;
        const int ns = (int)p.getSources().size();
        switch (rnd(13)) {
            case 0: {  // a random parameter
                const int o = rnd(ns + 1);
                const int obj = o == ns ? verbp::kListener : o;
                const int k = rnd(obj == verbp::kListener ? 4 : 5);
                if (auto* prm = p.apvts.getParameter(verbp::paramId(obj, k))) prm->setValueNotifyingHost((float)rf(0, 1));
                break;
            }
            case 1:
                p.editScene([&](verb::Scene& s) {
                    const int m = rnd((int)verb::surfacePresets().size());
                    s.mats.push_back(verb::surfacePresets()[(size_t)m]);
                    s.walls.push_back(verb::Wall{rf(-2, 10), rf(-2, 8), rf(-2, 10), rf(-2, 8), (int)s.mats.size() - 1, rnd(3) ? 0.0 : rf(0, 2), rnd(3) ? 1e9 : rf(0.2, 3)});
                });
                break;
            case 2:
                p.editScene([&](verb::Scene& s) { if (!s.walls.empty()) s.walls.erase(s.walls.begin() + rnd((int)s.walls.size())); });
                break;
            case 3:
                p.editScene([&](verb::Scene& s) {
                    s.height = rf(1.5, 8); s.tempC = rf(-10, 40); s.humidity = rf(10, 95);
                    s.clutter.count = rnd(50); s.clutter.seed = rnd(9999); s.clutter.hMin = rf(0.1, 1); s.clutter.hMax = rf(1, 3);
                });
                break;
            case 4: p.addSource(); break;
            case 5: p.removeSource(rnd(std::max(1, ns))); break;
            case 6: {
                auto v = p.getSources();
                auto& c = v[(size_t)rnd((int)v.size())];
                c.format = rnd((int)verbp::sourceFormats().size());
                c.channels = 2 + rnd(7);
                c.firstIn = rnd(32);
                c.sum = rnd(2) == 0;
                c.spread = rf(0, 3);
                c.freePoints = rnd(2) == 0;
                if (c.freePoints) verbp::freezeLayout(c);
                p.setSources(v);
                break;
            }
            case 7: {
                verbp::OutputCfg o;
                o.layout = rnd(10) - 1;
                if (o.layout > (int)verb::Layout::Ring) o.layout = (int)verb::Layout::Ring;
                o.radius = rf(0, 0.6);
                p.setOutputCfg(o);
                break;
            }
            case 8: p.scaleRoom(rf(0.8, 1.25), rnd(2) == 0); break;
            case 9: p.orientObject(rnd(2) ? verbp::kListener : rnd(std::max(1, ns)), rf(-5, 12), rf(-5, 10)); break;
            case 10: p.setPointOffset(rnd(std::max(1, ns)), rnd(8), (float)rf(-3, 3), (float)rf(-3, 3), (float)rf(-1, 1)); break;
            case 11:
                if (rnd(6) == 0) {  // project save / load round trip while playing
                    juce::MemoryBlock mb;
                    p.getStateInformation(mb);
                    p.setStateInformation(mb.getData(), (int)mb.getSize());
                }
                break;
            case 12:
                if (rnd(12) == 0 && !demo.empty()) p.loadSceneText(juce::String(demo), "fuzz", rnd(2) == 0);
                break;
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil(rnd(30));
    }
    stop = true;
    audio.join();
    std::printf("    %d operations, %ld audio blocks during the fuzz\n", ops, blocks.load());
    check(!bad && blocks.load() > 1000, "no non-finite sample and the audio thread never stalled");

    // recovery: whatever state the fuzz left (even an error state) must be repairable
    p.setSources({verbp::SourceCfg{}});
    p.setOutputCfg(verbp::OutputCfg{});
    if (!demo.empty()) p.loadSceneText(juce::String(demo), "recovered", true);
    const bool ready = waitReady(p, 180);
    juce::MemoryBlock mb;
    p.getStateInformation(mb);
    VerbProcessor q;
    q.setPlayConfigDetails(32, 16, 48000, 256);
    q.prepareToPlay(48000, 256);
    q.setStateInformation(mb.getData(), (int)mb.getSize());
    check(ready && waitReady(q, 180), "after the fuzz: a clean scene loads, its state round-trips");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    std::printf("Plugin stress test\n");
    testRatesAndBlocks();
    testBlockIndependence();
    testBusLayouts();
    testLifecycle();
    testStateRobustness();
    testExtremesAndNaN();
    testThreadedFuzz();
    std::printf(fails ? "\n%d check(s) FAILED\n" : "\nall stress checks passed\n", fails);
    return fails ? 1 : 0;
}
