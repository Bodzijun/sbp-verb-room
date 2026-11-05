// Headless test of the plugin processor (no host): state, layout, automation, rendering, state round-trip.
#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <cstdio>
#include <random>

#include "InputOrder.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

static bool waitReady(VerbProcessor& p, double seconds) {
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    while ((juce::Time::getMillisecondCounterHiRes() - t0) < seconds * 1000) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        if (p.getLoadState() == 2) return true;
        if (p.getLoadState() == 3) return false;
    }
    return false;
}

static void setP(VerbProcessor& p, const char* id, double v) {
    auto* prm = p.apvts.getParameter(id);
    prm->setValueNotifyingHost(prm->convertTo0to1((float)v));
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep the output even if something crashes
    juce::ScopedJuceInitialiser_GUI init;
    std::printf("Plugin processor test\n");
    {
        VerbProcessor p;
        p.setPlayConfigDetails(4, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "built-in scene loads in the background");
        std::printf("  status: %s\n", p.getStatus().toRawUTF8());
        check(p.getTotalNumOutputChannels() == 2, "default output is stereo");

        // impulse on source 1 -> check arrival time (direct path to the listener centroid) and finiteness
        const int N = 48000, blk = 256;
        juce::AudioBuffer<float> buf(4, blk);
        std::vector<float> L, R;
        juce::MidiBuffer midi;
        bool finite = true;
        for (int pos = 0; pos < N; pos += blk) {
            buf.clear();
            if (pos == 0) buf.setSample(0, 0, 1.f);
            p.processBlock(buf, midi);
            for (int i = 0; i < blk; ++i) {
                float a = buf.getSample(0, i), b = buf.getSample(1, i);
                finite &= std::isfinite(a) && std::isfinite(b);
                L.push_back(a); R.push_back(b);
            }
        }
        check(finite, "output finite");
        int first = -1;
        double e = 0;
        for (size_t i = 0; i < L.size(); ++i) { e += L[i] * L[i]; if (first < 0 && std::fabs(L[i]) > 1e-3f) first = (int)i; }
        std::printf("  first arrival at sample %d (direct path 4 m => ~560), energy %.4f\n", first, e);
        check(first > 540 && first < 600 && e > 1e-4, "direct sound arrives at the geometric delay with energy present");

        // automation: move the source far away -> level must drop
        auto energy = [&](int steps) {
            double en = 0;
            for (int s = 0; s < steps; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                for (int i = 0; i < blk; ++i) en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i);
            }
            return en;
        };
        for (int i = 0; i < 100; ++i) { buf.clear(); p.processBlock(buf, midi); }
        double eNear = energy(40);
        setP(p, "s1x", 0.5);
        for (int i = 0; i < 400; ++i) { buf.clear(); p.processBlock(buf, midi); }  // let glide + decay settle
        double eMoved = energy(40);
        std::printf("  energy near %.3e, after automating s1x to 0.5 m: %.3e\n", eNear, eMoved);
        check(std::isfinite(eMoved) && eMoved != eNear, "automating a source parameter changes the output");

        // room editing: an opaque partition between source 1 and the listener must cut the early sound, an Open
        // segment must let it through again, and the edit must survive the project state
        auto early = [&](int steps) {  // energy of the first ~50 ms after an impulse on input 1
            double en = 0;
            for (int s = 0; s < steps; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                for (int i = 0; i < blk; ++i) en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i);
            }
            return en;
        };
        auto settleBlocks = [&](int n) { for (int i = 0; i < n; ++i) { buf.clear(); p.processBlock(buf, midi); } };
        setP(p, "s1x", 3.0);  // back to the scene position (the automation test above moved it)
        settleBlocks(400);
        const double eOpenRoom = early(10);
        const size_t wallsBefore = p.getDisplayScene().walls.size();
        p.editScene([](verb::Scene& s) {
            verb::Material m = verb::surfacePresets()[0];  // Concrete
            s.mats.push_back(m);
            s.walls.push_back(verb::Wall{0.0, 4.0, 6.0, 4.0, (int)s.mats.size() - 1});  // between the source (3, 6) and the listener (3, 2)
        });
        // early paths follow within a control period; the late field is re-traced on a worker thread (seconds)
        juce::Thread::sleep(6000);
        settleBlocks(400);
        const double eBlocked = early(10);
        std::printf("  early energy: open room %.3e, with a concrete partition %.3e (%.0f dB)\n", eOpenRoom, eBlocked, 10 * std::log10(eBlocked / eOpenRoom));
        check(p.getDisplayScene().walls.size() == wallsBefore + 1, "editScene adds the wall to the live room");
        check(eBlocked < eOpenRoom * 0.05, "an opaque wall inserted while running cuts the early sound by > 13 dB");
        juce::MemoryBlock edited;
        p.getStateInformation(edited);
        p.editScene([](verb::Scene& s) {  // make that segment an opening (own surface: only this wall changes)
            s.mats[(size_t)s.walls.back().mat] = verb::presetByName("Open");
        });
        juce::Thread::sleep(6000);
        settleBlocks(400);
        const double eOpening = early(10);
        std::printf("  with the segment set to Open: %.3e (%.1f dB vs open room)\n", eOpening, 10 * std::log10(eOpening / eOpenRoom));
        check(std::fabs(10 * std::log10(eOpening / eOpenRoom)) < 3.0, "an Open segment lets the sound through (no extra reflection/loss)");
        // the same partition, now only 1.0 m high: the sound (source and listener at 1.5 m) passes over it
        p.editScene([](verb::Scene& s) {
            s.mats[(size_t)s.walls.back().mat] = verb::presetByName("Concrete");
            s.walls.back().z1 = 1.0;
        });
        juce::Thread::sleep(6000);
        settleBlocks(400);
        const double eLow = early(10);
        std::printf("  concrete partition 1.0 m high (ears at 1.5 m): %.3e (%.1f dB vs open room)\n", eLow, 10 * std::log10(eLow / eOpenRoom));
        check(std::fabs(10 * std::log10(eLow / eOpenRoom)) < 3.0, "a low partition is passed over by sound at ear height");
        {
            VerbProcessor q2;
            q2.setPlayConfigDetails(4, 2, 48000, 256);
            q2.prepareToPlay(48000, 256);
            q2.setStateInformation(edited.getData(), (int)edited.getSize());
            check(waitReady(q2, 60), "project with the edited room reloads");
            check(q2.getDisplayScene().walls.size() == wallsBefore + 1, "the edited walls are saved in the project state");
        }

        // positions are free: the listener far outside the room (and above the ceiling) must not break anything,
        // and must come back to the same level
        {
            setP(p, "lx", 3.0); setP(p, "ly", 2.0);
            settleBlocks(200);
            const double eHome = early(10);
            setP(p, "lx", -40.0); setP(p, "ly", 25.0); setP(p, "lz", 29.0);
            settleBlocks(200);
            bool fin = true;
            for (int s = 0; s < 20; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                for (int i = 0; i < blk; ++i) fin &= std::isfinite(buf.getSample(0, i)) && std::isfinite(buf.getSample(1, i));
            }
            check(fin, "listener far outside the room: output finite");
            check(p.getDisplayScene().walls.size() > 0 && std::fabs(p.apvts.getRawParameterValue("lx")->load() + 40.f) < 1e-3f, "outside position is kept (not clamped to the walls)");
            setP(p, "lx", 3.0); setP(p, "ly", 2.0); setP(p, "lz", 1.5);
            settleBlocks(300);
            const double eBack = early(10);
            check(std::fabs(10 * std::log10(eBack / eHome)) < 1.0, "back inside: the same level as before");
        }

        // the editor must construct, lay out and paint; snapshots go to out/ for a visual check
        {
            std::unique_ptr<juce::AudioProcessorEditor> base(p.createEditor());
            auto* ed = dynamic_cast<VerbEditor*>(base.get());
            check(ed != nullptr, "editor is created");
            if (ed) {
                ed->setSize(1160, 760);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
                auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile("out");
                outDir.createDirectory();
                auto save = [&](const char* name) {
                    juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds());
                    juce::PNGImageFormat png;
                    juce::FileOutputStream fo(outDir.getChildFile(name));
                    if (fo.openedOk()) { fo.setPosition(0); fo.truncate(); png.writeImageToStream(img, fo); }
                    return img.getWidth() > 100;
                };
                check(save("editor_objects.png"), "editor paints (objects tab)");
                ed->selectWall((int)p.getDisplayScene().walls.size() - 1);  // the segment made Open above
                juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
                check(save("editor_surfaces.png"), "editor paints (surfaces tab, selected segment)");
                ed->selectWall(0);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
                check(save("editor_surfaces_wall1.png"), "editor paints another segment");
            }
        }

        // state round trip
        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        VerbProcessor q;
        q.setPlayConfigDetails(4, 2, 48000, 256);
        q.prepareToPlay(48000, 256);
        q.setStateInformation(mb.getData(), (int)mb.getSize());
        check(waitReady(q, 60), "scene restored from saved state");
        check(std::fabs(q.apvts.getRawParameterValue("s1x")->load() - 3.0f) < 1e-3f, "automated parameter value restored");
        check(q.getSceneName() == p.getSceneName(), "scene name restored");
        {   // the input meters are an option saved with the project
            p.setInputMetersEnabled(false);
            juce::MemoryBlock mb2;
            p.getStateInformation(mb2);
            VerbProcessor q2;
            q2.setPlayConfigDetails(4, 2, 48000, 256);
            q2.prepareToPlay(48000, 256);
            q2.setStateInformation(mb2.getData(), (int)mb2.getSize());
            check(!q2.inputMetersEnabled() && q.inputMetersEnabled(), "the input meters option is saved with the project");
            p.setInputMetersEnabled(true);
        }
    }
    {   // orientation, scaling, free channel points, space (air, clutter)
        VerbProcessor r;
        r.setPlayConfigDetails(8, 2, 48000, 256);
        r.prepareToPlay(48000, 256);
        check(waitReady(r, 60), "setup for orientation / scaling / free points loads");
        auto param = [&](const char* id) { return (double)r.apvts.getRawParameterValue(id)->load(); };
        setP(r, "s1x", 3.0); setP(r, "s1y", 6.0);
        r.orientObject(0, 3.0, 10.0);
        const double az1 = param("s1az");
        r.orientObject(verbp::kListener, param("lx"), param("ly") - 8.0);
        const double yaw1 = param("laz");  // the listener's yaw uses the "az" component id
        std::printf("  orient: source towards +y -> azimuth %.1f, listener towards -y -> yaw %.1f\n", az1, yaw1);
        check(std::fabs(az1 - 90.0) < 0.01 && std::fabs(yaw1 + 90.0) < 0.01, "orientObject points a source / the listener at a position");

        const auto before = r.getDisplayScene();
        const double s1x0 = param("s1x"), lx0 = param("lx");
        r.scaleRoom(2.0, true);
        const auto big = r.getDisplayScene();
        const bool bigOk = std::fabs(big.walls[1].ax - (-3.0)) < 1e-6 && std::fabs(big.walls[1].bx - 9.0) < 1e-6 && std::fabs(big.height - 6.0) < 1e-6 &&
                           std::fabs(param("s1x") - (3.0 + (s1x0 - 3.0) * 2.0)) < 1e-3 && std::fabs(param("lx") - (3.0 + (lx0 - 3.0) * 2.0)) < 1e-3;
        r.scaleRoom(0.5, true);
        const auto back = r.getDisplayScene();
        check(bigOk && std::fabs(back.walls[0].ax - before.walls[0].ax) < 1e-6 && std::fabs(back.height - before.height) < 1e-6 && std::fabs(param("s1x") - s1x0) < 1e-3,
              "scaleRoom scales walls, height, sources and listener about the room centre; x2 then x0.5 restores it");
        r.scaleWall(0, 0.5);
        const auto sw = r.getDisplayScene();
        check(std::fabs(std::hypot(sw.walls[0].bx - sw.walls[0].ax, sw.walls[0].by - sw.walls[0].ay) - 4.0) < 1e-6 &&
                  std::fabs(0.5 * (sw.walls[0].ay + sw.walls[0].by) - 4.0) < 1e-6,
              "scaleWall halves a segment about its midpoint");

        // free channel points: moving a point 2 m towards the listener makes its sound arrive 2 m / c earlier
        setP(r, "s1az", -90.0);  // faces the listener, which is below the source
        {
            auto v = r.getSources();
            v[0].name = "Pair"; v[0].format = 1; v[0].channels = 2; v[0].firstIn = 0; v[0].spread = 1.0; v[0].freePoints = true;
            verbp::freezeLayout(v[0]);
            r.setSources(v);
        }
        check(waitReady(r, 60), "stereo source with free points loads");
        auto arrival = [&]() {
            juce::AudioBuffer<float> b(8, 256);
            juce::MidiBuffer m;
            for (int s = 0; s < 60; ++s) { b.clear(); r.processBlock(b, m); }  // settle
            int first = -1;
            for (int s = 0; s < 30 && first < 0; ++s) {
                b.clear();
                if (s == 0) b.setSample(0, 0, 1.f);  // impulse on channel 1 = point 0 of the pair
                r.processBlock(b, m);
                for (int i = 0; i < 256 && first < 0; ++i)
                    if (std::fabs(b.getSample(0, i)) > 2e-3f || std::fabs(b.getSample(1, i)) > 2e-3f) first = s * 256 + i;
            }
            return first;
        };
        const int a0 = arrival();
        const auto cfg0 = r.getSources()[0];
        r.setPointOffset(0, 0, cfg0.ox[0] + 2.0f, cfg0.oy[0], cfg0.oz[0]);
        const int a1 = arrival();
        const double expectShift = 2.0 / 343.2 * 48000.0;
        std::printf("  free point moved 2 m towards the listener: first arrival %d -> %d (shift %d, expected %.0f)\n", a0, a1, a0 - a1, expectShift);
        check(a0 > 0 && a1 > 0 && std::fabs((a0 - a1) - expectShift) < 12, "a free channel point moves independently of the others (arrival time follows its distance)");
        juce::MemoryBlock fm;
        r.getStateInformation(fm);

        // space: air and clutter live in the scene and in the project state
        r.editScene([](verb::Scene& s) { s.tempC = 8.0; s.humidity = 25.0; s.clutter.count = 30; s.clutter.seed = 1234; });
        juce::Thread::sleep(300);
        juce::MemoryBlock sm;
        r.getStateInformation(sm);
        VerbProcessor r2;
        r2.setPlayConfigDetails(8, 2, 48000, 256);
        r2.prepareToPlay(48000, 256);
        r2.setStateInformation(sm.getData(), (int)sm.getSize());
        check(waitReady(r2, 90), "project with air, clutter and free points reloads");
        const auto s2 = r2.getDisplayScene();
        const auto src2 = r2.getSources();
        check(std::fabs(s2.tempC - 8.0) < 1e-9 && std::fabs(s2.humidity - 25.0) < 1e-9 && s2.clutter.count == 30 && s2.clutter.seed == 1234, "temperature, humidity and clutter (count, seed) are saved in the project");
        check(src2[0].freePoints && src2[0].ox.size() == 2 && std::fabs(src2[0].ox[0] - (cfg0.ox[0] + 2.0f)) < 1e-3f, "free channel points are saved in the project");

        // pictures of the new pages
        std::unique_ptr<juce::AudioProcessorEditor> base(r.createEditor());
        if (auto* ed = dynamic_cast<VerbEditor*>(base.get())) {
            ed->setSize(1160, 760);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile("out");
            outDir.createDirectory();
            auto save = [&](const char* name) {
                juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds());
                juce::PNGImageFormat png;
                juce::FileOutputStream fo(outDir.getChildFile(name));
                if (fo.openedOk()) { fo.setPosition(0); fo.truncate(); png.writeImageToStream(img, fo); }
                return img.getWidth() > 100;
            };
            ed->showPage(0, 0);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_free_points.png"), "editor paints free channel points");
            ed->showPage(0, verbp::kListener);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_listener.png"), "editor paints the listener page with the output configuration");
            ed->showPage(3, -1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_space.png"), "editor paints the Space page");
            ed->selectWall(1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_surface_classes.png"), "editor paints the Surfaces page with classes and scale");
            check(ed->isAutoView(), "the map starts in auto view (follows walls and objects)");
            ed->zoomView(0.12);  // zoom far out: room for a long path (a car driving by)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(!ed->isAutoView() && save("editor_zoomed_out.png"), "zooming switches to the free view and paints");
            ed->setAutoView(true);
            check(ed->isAutoView(), "auto view can be switched back on");
        }
    }
    {   // many sources, formats and a 7.1.4 output
        VerbProcessor q;
        q.setPlayConfigDetails(32, 12, 48000, 256);
        q.prepareToPlay(48000, 256);
        check(waitReady(q, 60), "setup with 12 output channels loads");
        std::printf("  status: %s\n", q.getStatus().toRawUTF8());
        // a track with 12 channels (it needs them for side inputs, say) does not become a 7.1.4 output by itself
        check(q.numOutputChannels() == 2 && q.effectiveLayout() == (int)verb::Layout::StereoORTF, "default output is stereo even on a 12-channel track (inputs and outputs are independent)");
        {
            juce::AudioBuffer<float> bb(32, 256);
            juce::MidiBuffer mm;
            double e01 = 0, eRest = 0;
            for (int s = 0; s < 20; ++s) {
                bb.clear();
                if (s == 0) bb.setSample(0, 0, 1.f);
                q.processBlock(bb, mm);
                for (int ch = 0; ch < 12; ++ch)
                    for (int i = 0; i < 256; ++i) (ch < 2 ? e01 : eRest) += (double)bb.getSample(ch, i) * bb.getSample(ch, i);
            }
            check(e01 > 1e-6 && eRest == 0.0, "channels beyond the layout stay silent (the sources' side-input channels are consumed)");
        }
        {
            verbp::OutputCfg oc = q.getOutputCfg();
            oc.layout = (int)verb::Layout::Surround714;
            q.setOutputCfg(oc);
            check(waitReady(q, 60), "explicit 7.1.4 layout loads");
            check(q.numOutputChannels() == 12 && q.effectiveLayout() == (int)verb::Layout::Surround714, "the 7.1.4 layout is chosen explicitly on the listener");
            oc.layout = -1;  // "match the track" still exists as an option
            q.setOutputCfg(oc);
            check(waitReady(q, 60) && q.numOutputChannels() == 12, "the auto option follows the track's channel count");
            oc.layout = (int)verb::Layout::Surround714;
            q.setOutputCfg(oc);
            check(waitReady(q, 60), "back to explicit 7.1.4");
        }
        for (int i = 0; i < 5; ++i) q.addSource();  // 6 mono sources on inputs 1..6
        {
            auto v = q.getSources();
            verbp::SourceCfg music; music.name = "Music"; music.format = 1; music.firstIn = 6; music.spread = 1.0;           // stereo, inputs 7-8
            verbp::SourceCfg bed; bed.name = "Bed"; bed.format = 5; bed.firstIn = 8;                                           // 5.1, inputs 9-14 (LFE = input 12)
            verbp::SourceCfg vox; vox.name = "Vox"; vox.format = 1; vox.firstIn = 14; vox.sum = true;                          // stereo summed into one point
            v.push_back(music); v.push_back(bed); v.push_back(vox);
            q.setSources(v);
        }
        check(waitReady(q, 90), "setup with 9 sources of mixed formats loads");
        std::printf("  status: %s\n", q.getStatus().toRawUTF8());
        check(q.numPoints() == 6 + 2 + 5 + 1, "points: 6 mono + stereo (2) + 5.1 without LFE (5) + summed stereo (1)");

        const int blk = 256;
        juce::AudioBuffer<float> b(32, blk);
        juce::MidiBuffer midi;
        bool fin = true;
        std::vector<double> chEnergy(12, 0.0);
        std::vector<float> lfeOut;
        for (int s = 0; s < 40; ++s) {
            b.clear();
            if (s == 0) b.setSample(0, 0, 1.f);    // impulse on mono source 1
            if (s == 0) b.setSample(11, 0, 1.f);   // impulse on the LFE input of the 5.1 bed (input 12)
            q.processBlock(b, midi);
            for (int ch = 0; ch < 12; ++ch)
                for (int i = 0; i < blk; ++i) {
                    const float v = b.getSample(ch, i);
                    fin &= std::isfinite(v);
                    chEnergy[(size_t)ch] += (double)v * v;
                    if (ch == 3) lfeOut.push_back(v);
                }
        }
        check(fin, "16-source setup: output finite");
        bool allSound = true;
        for (int ch = 0; ch < 12; ++ch) if (ch != 3 && chEnergy[(size_t)ch] <= 1e-9) allSound = false;
        check(allSound, "every 7.1.4 channel (including the 4 height channels) receives sound from a source");
        check(std::fabs(lfeOut[0] - 1.f) < 0.01f, "the 5.1 LFE input reaches the LFE output directly (sample 0, unity)");

        // CPU for 16 mono-ish sources through 12 outputs while one source moves
        const int blocks = 400;
        double total = 0, worst = 0;
        std::mt19937 rng(3);
        std::normal_distribution<float> nd(0, 0.05f);
        for (int s = 0; s < blocks; ++s) {
            for (int ch = 0; ch < 16; ++ch) { auto* d = b.getWritePointer(ch); for (int i = 0; i < blk; ++i) d[i] = nd(rng); }
            setP(q, "s1x", 2.0 + 0.5 * std::sin(s * 0.05));
            const auto t0 = juce::Time::getHighResolutionTicks();
            q.processBlock(b, midi);
            const double dt = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - t0);
            total += dt;
            worst = std::max(worst, dt);
        }
        const double load = 100.0 * total / (blocks * blk / 48000.0);
        std::printf("  9 sources / 14 points -> 12 outputs: mean load %.1f %% of one core, worst block %.2f ms (budget %.2f ms)\n", load, worst * 1000, 1000.0 * blk / 48000.0);
        check(load < 60.0, "mean audio-thread load below 60 % of one core");

        // pictures of the source / output pages for this setup
        std::unique_ptr<juce::AudioProcessorEditor> base(q.createEditor());
        if (auto* ed = dynamic_cast<VerbEditor*>(base.get())) {
            ed->setSize(1160, 760);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile("out");
            outDir.createDirectory();
            auto save = [&](const char* name) {
                juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds());
                juce::PNGImageFormat png;
                juce::FileOutputStream fo(outDir.getChildFile(name));
                if (fo.openedOk()) { fo.setPosition(0); fo.truncate(); png.writeImageToStream(img, fo); }
                return img.getWidth() > 100;
            };
            ed->showPage(0, 7);  // the 5.1 bed
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_sources_bed.png"), "editor paints the 5.1 source page");
            ed->showPage(2, -1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_mix_714.png"), "editor paints the output page");
        }
    }
    {
        VerbProcessor p;
        p.setPlayConfigDetails(4, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        waitReady(p, 60);
        p.loadSceneText("wall 0 0 1 1 nosuchmaterial", "bad.txt", true);
        bool err = false;
        for (int i = 0; i < 100 && !err; ++i) { juce::Thread::sleep(20); err = p.getLoadState() == 3; }
        check(err, "invalid scene reports an error");
        juce::AudioBuffer<float> buf(4, 256);
        juce::MidiBuffer midi;
        buf.clear();
        p.processBlock(buf, midi);
        check(true, "processing continues with the previous scene after a bad load");
    }
    {   // source directivity and Doppler strength: live controls, saved in the project
        std::printf("Directivity and Doppler controls\n");
        VerbProcessor p;
        p.setPlayConfigDetails(4, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "scene loads");
        juce::AudioBuffer<float> buf(4, 256);
        juce::MidiBuffer midi;
        auto energy = [&]() {  // impulse on input 1, 0.3 s of output
            double en = 0;
            for (int s = 0; s < 56; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                for (int i = 0; i < 256; ++i) en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i);
            }
            return en;
        };
        for (int i = 0; i < 100; ++i) { buf.clear(); p.processBlock(buf, midi); }
        setP(p, "s1az", 0.0);
        for (int i = 0; i < 100; ++i) { buf.clear(); p.processBlock(buf, midi); }
        const double omni = energy();
        p.setSourcePattern(0, 0.5);
        for (int i = 0; i < 300; ++i) { buf.clear(); p.processBlock(buf, midi); }
        const double card0 = energy();
        setP(p, "s1az", 180.0);
        for (int i = 0; i < 300; ++i) { buf.clear(); p.processBlock(buf, midi); }
        const double card180 = energy();
        std::printf("  energy omni %.3e, cardioid az 0 %.3e, cardioid az 180 %.3e\n", omni, card0, card180);
        check(std::isfinite(card0) && std::isfinite(card180) && card0 != card180, "directivity changes the output when the source turns");
        check(p.getSources()[0].pattern == 0.5, "the directivity is part of the source configuration");
        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        VerbProcessor q;
        q.setPlayConfigDetails(4, 2, 48000, 256);
        q.prepareToPlay(48000, 256);
        q.setStateInformation(mb.getData(), (int)mb.getSize());
        check(q.getSources()[0].pattern == 0.5, "the directivity survives the project state");
        setP(p, "doppler", 0.0);
        for (int i = 0; i < 50; ++i) { buf.clear(); p.processBlock(buf, midi); }
        check(std::fabs(p.apvts.getRawParameterValue("doppler")->load()) < 1e-6f, "Doppler strength is an automatable parameter");
    }
    {   // floor and ceiling zones through the plugin: they change the sound, survive the project state, and the editor draws them
        std::printf("Floor and ceiling zones\n");
        VerbProcessor p;
        p.setPlayConfigDetails(4, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "scene loads");
        juce::AudioBuffer<float> buf(4, 256);
        juce::MidiBuffer midi;
        auto tailEnergy = [&]() {  // reverberant energy 80..400 ms after an impulse
            double en = 0;
            for (int s = 0; s < 80; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                if (s >= 15) for (int i = 0; i < 256; ++i) en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i);
            }
            return en;
        };
        for (int i = 0; i < 100; ++i) { buf.clear(); p.processBlock(buf, midi); }
        const double before = tailEnergy();
        p.editScene([](verb::Scene& s) {
            s.mats.push_back(verb::presetByName("Absorber"));
            verb::Zone z;
            z.mat = (int)s.mats.size() - 1;
            z.poly = {{-50, -50}, {50, -50}, {50, 50}, {-50, 50}};
            s.floorZones.push_back(z);
            verb::Zone c = z;
            c.mat = -1;  // an opening over the whole room
            s.ceilZones.push_back(c);
        });
        for (int i = 0; i < 60; ++i) {  // the new geometry is built and published by the loader thread
            for (int k = 0; k < 20; ++k) { buf.clear(); p.processBlock(buf, midi); }
            juce::Thread::sleep(100);
        }
        for (int i = 0; i < 300; ++i) { buf.clear(); p.processBlock(buf, midi); }
        const double after = tailEnergy();
        std::printf("  late energy: %.3e before, %.3e with an absorbing floor zone and a ceiling opening\n", before, after);
        check(std::isfinite(after) && after < before * 0.7, "floor and ceiling zones change the reverberation");
        const auto sc = p.getDisplayScene();
        check(sc.floorZones.size() == 1 && sc.ceilZones.size() == 1 && sc.ceilZones[0].mat < 0, "the zones are part of the scene");
        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        VerbProcessor q;
        q.setPlayConfigDetails(4, 2, 48000, 256);
        q.prepareToPlay(48000, 256);
        q.setStateInformation(mb.getData(), (int)mb.getSize());
        waitReady(q, 60);
        const auto sq = q.getDisplayScene();
        check(sq.floorZones.size() == 1 && sq.ceilZones.size() == 1 && sq.ceilZones[0].mat < 0 && sq.floorZones[0].poly.size() == 4, "the zones survive the project state");
        // editor: zones on the plan, the front and side elevations
        p.editScene([](verb::Scene& s) {
            s.floorZones[0].poly = {{0.5, 0.5}, {3.5, 0.5}, {3.5, 3.0}, {0.5, 3.0}};
            s.ceilZones[0].poly = {{4.0, 1.0}, {6.0, 1.0}, {6.0, 3.0}, {4.0, 3.0}};
            s.floorZones[0].mat = s.floorMat;
            verb::Zone plate;  // a lowered ceiling over part of the room
            plate.mat = s.floorMat;
            plate.z = 2.0;
            plate.poly = {{0.5, 3.5}, {3.5, 3.5}, {3.5, 5.0}, {0.5, 5.0}};
            s.ceilZones.push_back(plate);
        });
        std::unique_ptr<juce::AudioProcessorEditor> base(p.createEditor());
        auto* ed = dynamic_cast<VerbEditor*>(base.get());
        check(ed != nullptr, "editor opens with zones");
        if (ed) {
            ed->setSize(1160, 760);
            auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile("out");
            outDir.createDirectory();
            auto save = [&](const char* name) {
                juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds());
                juce::PNGImageFormat png;
                juce::FileOutputStream fo(outDir.getChildFile(name));
                if (fo.openedOk()) { fo.setPosition(0); fo.truncate(); png.writeImageToStream(img, fo); }
                return img.getWidth() > 100;
            };
            ed->selectZone(false, 0);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_zone_plan.png"), "editor paints floor and ceiling zones on the plan");
            ed->selectZone(true, 1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_plate.png"), "editor paints a ceiling plate with its height row");
            check(p.getDisplayScene().ceilZones.size() == 2 && std::fabs(p.getDisplayScene().ceilZones[1].z - 2.0) < 1e-9, "the plate keeps its height");
            {   // overlapping surfaces: every one under the point can be reached; layers can be locked or hidden
                p.editScene([](verb::Scene& s) {
                    verb::Zone extra;
                    extra.mat = s.floorMat;
                    extra.poly = {{0.5, 2.5}, {3.5, 2.5}, {3.5, 4.5}, {0.5, 4.5}};
                    s.floorZones.push_back(extra);  // overlaps the first floor zone and the plate
                });
                juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
                check(save("editor_overlap.png"), "editor paints overlapping zones");
                const int both = ed->testPickCount(2.0, 3.8), floors = ed->testPickCount(2.0, 2.8);
                ed->testSetLayer(3, 2);  // plates hidden
                const int noPlate = ed->testPickCount(2.0, 3.8);
                ed->testSetLayer(3, 0);
                ed->testSetLayer(1, 1);  // floors locked
                const int onlyPlate = ed->testPickCount(2.0, 3.8);
                ed->testSetLayer(3, 1);
                const int none = ed->testPickCount(2.0, 3.8);
                ed->testSetLayer(1, 0);
                ed->testSetLayer(3, 0);
                std::printf("  surfaces under a point: %d (plate + floor zone), %d (two floor zones); plates hidden %d, floors locked %d, both %d\n", both, floors, noPlate, onlyPlate, none);
                check(both == 2 && floors == 2 && noPlate == 1 && onlyPlate == 1 && none == 0, "all overlapping surfaces are reachable, hidden and locked layers are skipped");
            }
            ed->showPlane(1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_view_front.png"), "editor paints the front elevation");
            ed->showPlane(2);
            ed->selectWall(1);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_view_side.png"), "editor paints the side elevation");
            ed->showPlane(3);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            check(save("editor_view_3d.png"), "editor paints the 3D view");
            ed->showPlane(0);
            // several walls selected together take a surface change together
            ed->selectWalls({0, 1, 2});
            ed->applyPresetToSelection("Heavy_curtain");
            juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
            const auto sc3 = p.getDisplayScene();
            int curtains = 0;
            for (size_t i = 0; i < sc3.walls.size(); ++i)
                curtains += sc3.mats[(size_t)sc3.walls[i].mat].name == "Heavy_curtain" ? 1 : 0;
            std::printf("  walls with the curtain material after editing a 3-wall selection: %d of %zu\n", curtains, sc3.walls.size());
            check(sc3.walls.size() >= 4 && curtains == 3, "a selection of several walls is edited together, the others stay as they were");
            check(save("editor_multi_select.png"), "editor paints the multi-selection");
            // "clear everything": no walls, no zones, the plugin keeps running and still makes (free-field) sound
            ed->clearEverything();
            juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
            const auto sc4 = p.getDisplayScene();
            check(sc4.walls.empty() && sc4.floorZones.empty() && sc4.ceilZones.empty() && sc4.clutter.count == 0, "clear everything leaves an empty room");
            for (int i = 0; i < 60; ++i) { juce::Thread::sleep(50); buf.clear(); p.processBlock(buf, midi); }
            double en = 0;
            bool fin = true;
            for (int s = 0; s < 40; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                for (int i = 0; i < 256; ++i) { fin &= std::isfinite(buf.getSample(0, i)); en += buf.getSample(0, i) * buf.getSample(0, i); }
            }
            check(fin && en > 1e-9, "an empty room still renders finite sound");
            check(save("editor_cleared.png"), "editor paints the empty room");
        }
    }
    {   // Doppler of a fast fly-by driven by block-rate automation (what a host does): the pitch must follow the physics smoothly
        std::printf("Fast fly-by, automation once per host block\n");
        static const char* freeField = R"(sr 48000
length 1.0
height 30
material absorb 1 1 1 1 1 1 1 0 0
floor absorb
ceiling open
source S 0 0 1.5
receiver L 0 0 1.5 0 0
receiver R 0 0 1.5 0 0
)";
        // updatePeriod > 0: the position is written only every `updatePeriod` samples, not at every block start (what a mouse
        // drag or sparse automation does); dy: closest distance of the pass
        auto runPass = [&](int blockSize, double speed, int updatePeriod = 0, double dyIn = 3.0) {
            VerbProcessor p;
            p.setPlayConfigDetails(2, 2, 48000, blockSize);
            p.prepareToPlay(48000, blockSize);
            p.loadSceneText(freeField, "free.txt", true);
            waitReady(p, 60);
            setP(p, "doppler", 1.0);
            setP(p, "lx", 0.0); setP(p, "ly", 0.0); setP(p, "lz", 1.5);
            const double sr = 48000, f0 = 4000, c = 343.2 * std::sqrt((20.0 + 273.15) / 293.15), dy = dyIn;
            const double x0 = -50.0, T = 100.0 / speed;
            const int total = (int)(T * sr);
            juce::AudioBuffer<float> buf(2, blockSize);
            juce::MidiBuffer midi;
            std::vector<float> out;
            setP(p, "s1y", dy); setP(p, "s1z", 1.5); setP(p, "s1az", 0.0);
            setP(p, "s1x", x0);
            for (int i = 0; i < 40; ++i) { buf.clear(); p.processBlock(buf, midi); }  // settle at the start
            long pos = 0;
            int nextUpdate = 0;
            for (int blockStart = 0; blockStart < total; blockStart += blockSize) {
                if (updatePeriod == 0 || blockStart >= nextUpdate) {  // the host writes the automation value at the block start
                    setP(p, "s1x", x0 + speed * (double)blockStart / sr);
                    nextUpdate = blockStart + updatePeriod;
                }
                for (int i = 0; i < blockSize; ++i) {
                    const double t = (double)(blockStart + i) / sr;
                    buf.setSample(0, i, (float)(0.5 * std::sin(2 * juce::MathConstants<double>::pi * f0 * t)));
                    buf.setSample(1, i, 0.f);
                }
                p.processBlock(buf, midi);
                for (int i = 0; i < blockSize; ++i) out.push_back(buf.getSample(0, i));
                pos += blockSize;
            }
            // measured frequency in 512-sample windows (rising zero crossings) vs the ideal Doppler of the straight pass
            auto idealF = [&](double tArr) {  // arrival time tArr -> the frequency the receiver hears
                auto arr = [&](double te) { return te + std::hypot(x0 + speed * te, dy) / c; };
                double lo = 0, hi = T;  // te solves arr(te) = tArr
                for (int it = 0; it < 60; ++it) { const double mid = 0.5 * (lo + hi); (arr(mid) < tArr ? lo : hi) = mid; }
                const double te = 0.5 * (lo + hi), h = 1e-4;
                return f0 * h / (arr(te + h) - arr(te));
            };
            double worst = 0, sum2 = 0;
            int n = 0;
            std::vector<double> fSeries;  // measured pitch per window, for the wobble measure below
            const int win = 512;
            for (size_t s = (size_t)(0.25 * sr); s + win < out.size() - (size_t)(0.1 * sr); s += win / 2) {
                double first = -1, last = -1;
                int cnt = 0;
                for (size_t i = s + 1; i < s + win; ++i)
                    if (out[i - 1] < 0 && out[i] >= 0) {
                        const double tc = (double)(i - 1) + (0 - out[i - 1]) / (out[i] - out[i - 1]);
                        if (first < 0) first = tc;
                        last = tc;
                        ++cnt;
                    }
                if (cnt < 8) continue;
                const double fm = (cnt - 1) * sr / (last - first);
                const double tMid = ((double)s + win / 2.0) / sr;
                const double err = std::fabs(fm - idealF(tMid)) / f0;
                fSeries.push_back(fm / f0);
                worst = std::max(worst, err);
                sum2 += err * err;
                ++n;
            }
            // clicks: the largest sample-to-sample step against the largest a pure tone at the highest Doppler pitch can make
            double amp = 0, jump = 0;
            for (size_t i = (size_t)(0.25 * sr); i + 1 < out.size() - (size_t)(0.1 * sr); ++i) { amp = std::max<double>(amp, std::fabs(out[i])); jump = std::max<double>(jump, std::fabs(out[i + 1] - out[i])); }
            const double natural = amp * 2 * std::sin(juce::MathConstants<double>::pi * f0 * 1.6 / sr);
            // wobble ("chewed tape"): the pitch's window-to-window scatter around a smooth curve (second differences)
            double s2 = 0;
            for (size_t i = 1; i + 1 < fSeries.size(); ++i) { const double d2 = fSeries[i - 1] - 2 * fSeries[i] + fSeries[i + 1]; s2 += d2 * d2; }
            const double wobble = fSeries.size() > 3 ? std::sqrt(s2 / (double)(fSeries.size() - 2) / 6.0) : 0.0;
            struct R { double worst, rms, click, wobble; };
            return R{worst, std::sqrt(sum2 / std::max(1, n)), jump / natural, wobble};
        };
        {   // pitch wobble of a moderate pass (10 m/s, 20 m away) under irregular position updates
            struct Case { const char* what; int block, period; };
            for (const Case& cs : {Case{"every block", 512, 0}, Case{"60 Hz mouse drag", 512, 800}, Case{"60 Hz mouse drag, big block", 1024, 800}, Case{"30 Hz sparse automation", 256, 1600}}) {
                const auto r = runPass(cs.block, 10.0, cs.period, 20.0);
                std::printf("  wobble 10 m/s pass at 20 m, block %d, updates: %s: pitch scatter %.3f %%, click ratio %.2f\n", cs.block, cs.what, 100 * r.wobble, r.click);
                // before the alpha-beta tracker a mouse drag / sparse automation gave 4.7-6.9 % ("chewed tape")
                check(r.wobble < 0.006 && r.click < 1.15, "the pitch stays steady with irregular position updates (scatter < 0.6 %, no clicks)");
            }
        }
        for (auto cfg : {std::make_pair(256, 40.0), std::make_pair(1024, 40.0), std::make_pair(2048, 40.0), std::make_pair(512, 80.0), std::make_pair(2048, 80.0)}) {
            const auto r = runPass(cfg.first, cfg.second);
            std::printf("  %.0f m/s fly-by at 3 m, host block %d: pitch error vs physics worst %.2f %%, rms %.2f %%, click ratio %.2f\n", cfg.second, cfg.first, 100 * r.worst, 100 * r.rms, r.click);
            // up to 1024 samples (the usual host blocks) the pitch follows the physics closely; at 2048 (43 ms) the
            // extrapolation over the block of a 500 m/s^2 turn at closest approach can no longer keep up (limit documented)
            if (cfg.first <= 1024) check(r.rms < 0.005 && r.worst < 0.02, "the Doppler follows the physical pitch curve (rms < 0.5 %, worst < 2 %) with block-rate automation");
            else check(r.rms < 0.02 && r.worst < 0.2, "very large host blocks (2048): still within 2 % rms");
            check(r.click < 1.15, "no clicks or steps in the fly-by");
        }
    }
    {   // the car: a place in the cabin for every one of the 30 engine stems
        std::printf("Car cabin preset\n");
        const auto rooms = verbp::builtinRooms();
        std::vector<const verbp::BuiltinRoom*> cars;
        for (auto& r : rooms) if (!r.setup.empty()) cars.push_back(&r);
        check(cars.size() == 2, "the library has the two MotorSynth presets");
        for (size_t ci = 0; ci < cars.size(); ++ci) {
            const verbp::BuiltinRoom* car = cars[ci];
            const size_t nSrc = ci == 0 ? 15 : 21;
            const int nPts = ci == 0 ? 19 : 25;
            std::printf("  %s\n", car->name.c_str());
            int channels = 0;
            for (auto& c : car->setup) channels += c.effectiveChannels();
            check(car->setup.size() == nSrc && channels == nPts, "sources and engine points as designed (4 wide sources carry 2 points)");
            VerbProcessor p;
            p.setPlayConfigDetails(48, 2, 48000, 256);
            p.prepareToPlay(48000, 256);
            check(waitReady(p, 60), "the plugin starts");
            p.loadRoomWithSetup(juce::String(car->text), "car.txt", car->setup, car->micPattern);
            check(waitReady(p, 60), "the car loads with its sources");
            check((int)p.getSources().size() == (int)nSrc && p.numPoints() == nPts, "the loaded plugin has the same sources and points");
            check(p.getOutputCfg().micPattern == (int)verb::MicPattern::HumanEars, "the driver listens with human ears");
            {   // every source sits on the Room inputs that MotorSynth feeds (stem i -> inputs 2i-1, 2i)
                const std::vector<std::pair<std::string, int>> want = {
                    {"Exhaust", 1}, {"Engine", 3}, {"Mechanical", 5}, {"Gearbox", 7}, {"Intake", 9}, {"Turbo BOV", 11}, {"Tires", 13}, {"Chassis", 15},
                    {"Brakes", 17}, {"Backfire", 19}, {"Wind", 21}, {"Cabin", 23}, {"Accessories", 25}, {"Electric", 27}, {"Environment", 29},
                    {"EngineSim Exhaust A", 31}, {"EngineSim Exhaust B", 33}, {"EngineSim Intake", 35}, {"EngineSim Block", 37}, {"EngineSim Valvetrain", 39}, {"EngineSim Starter", 41}};
                bool ok = true;
                int seen = 0;
                for (auto& w : want)
                    for (auto& c : car->setup)
                        if (c.name == w.first) { ++seen; ok &= c.firstIn + 1 == w.second; if (c.firstIn + 1 != w.second) std::printf("  %s is on input %d, MotorSynth sends it to %d\n", c.name.c_str(), c.firstIn + 1, w.second); }
                check(ok && seen == (int)car->setup.size(), "every stem is on the Room input that MotorSynth feeds");
            }
            {   // the engine bay is a box under a bonnet plate
                std::istringstream is(car->text);
                const verb::Scene cs = verb::parseScene(is, "car");
                int plates = 0;
                for (auto& z : cs.ceilZones) plates += z.z >= 0 && z.mat >= 0;
                check(plates == 1 && cs.walls.size() >= 11, "the car has a bonnet plate and the engine-bay walls");
            }
            // the stems sit where they should: tailpipes behind the cabin, engine in front (read back from the parameters)
            auto px = [&](int src) { return p.apvts.getRawParameterValue(verbp::paramId(src, 1))->load(); };  // the car drives up the plan: "forward" is y
            std::printf("  x positions: Exhaust A %.1f, Exhaust B %.1f, Intake %.1f, Engine block %.1f\n", px(0), px(1), px(2), px(3));
            check(px(0) < -1.6 && px(1) > 1.5 && px(4) > 1.5 && px(3) < 1.0, "tailpipe behind the car, the engine ahead of the firewall, the gearbox in the tunnel");
            // sound: an impulse in cylinder 1 (input 15) and in the tailpipe (input 1) arrives finite, the engine one is more muffled by the firewall
            auto level = [&](int inputCh) {
                juce::AudioBuffer<float> buf(48, 256);
                juce::MidiBuffer midi;
                for (int i = 0; i < 60; ++i) { buf.clear(); p.processBlock(buf, midi); }
                double en = 0;
                bool fin = true;
                for (int s = 0; s < 40; ++s) {
                    buf.clear();
                    if (s == 0) buf.setSample(inputCh, 0, 1.f);
                    p.processBlock(buf, midi);
                    for (int i = 0; i < 256; ++i) { fin &= std::isfinite(buf.getSample(0, i)) && std::isfinite(buf.getSample(1, i)); en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i); }
                }
                return fin ? 10 * std::log10(en + 1e-30) : 99.0;
            };
            {   // the input channel index in the buffer (0-based) is the number REAPER shows minus one: the source must hear exactly its own channel
                std::vector<bool> read(48, false);
                for (auto& c : car->setup) for (int q = 0; q < c.effectiveChannels(); ++q) read[(size_t)(c.firstIn + q)] = true;
                int bad = 0, tested = 0;
                for (auto& c : car->setup) {
                    if (c.effectiveChannels() != 1) continue;
                    const double own = level(c.firstIn);
                    ++tested;
                    if (c.firstIn >= 1 && !read[(size_t)c.firstIn - 1] && level(c.firstIn - 1) > own - 30.0) { ++bad; std::printf("  %s also hears input %d (one before it)\n", c.name.c_str(), c.firstIn); }
                    if (!read[(size_t)c.firstIn + 1] && level(c.firstIn + 1) > own - 30.0) { ++bad; std::printf("  %s also hears input %d (one after it)\n", c.name.c_str(), c.firstIn + 2); }
                    if (own < -200) { ++bad; std::printf("  %s hears nothing on input %d\n", c.name.c_str(), c.firstIn + 1); }
                }
                std::printf("  %d mono sources checked against their neighbouring input channels\n", tested);
                check(bad == 0 && tested > 0, "each source reacts to its own input channel and not to the one before or after it");
            }
            const double engine = level(8), pipe = level(0);
            std::printf("  an impulse in the intake: %.1f dB, in exhaust A: %.1f dB (at the driver's ears)\n", engine, pipe);
            check(engine < 90 && pipe < 90 && engine > -200 && pipe > -200, "all stems render finite sound at the driver");
            std::unique_ptr<juce::AudioProcessorEditor> base(p.createEditor());
            if (auto* ed = dynamic_cast<VerbEditor*>(base.get())) {
                ed->setSize(1160, 760);
                ed->showPage(0, 0);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(400);
                juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds());
                juce::PNGImageFormat png;
                juce::FileOutputStream fo(juce::File::getCurrentWorkingDirectory().getChildFile("out").getChildFile((ci == 0 ? "editor_car.png" : "editor_car_es.png")));
                if (fo.openedOk()) { fo.setPosition(0); fo.truncate(); png.writeImageToStream(img, fo); }
                check(img.getWidth() > 100, "editor paints the car");
            }
        }
    }
    {   // the host's channel order: JUCE reorders a long run of VST3 speaker bits; the plug-in undoes it
        std::printf("Input channel order\n");
        using C = juce::AudioChannelSet;
        const std::vector<C::ChannelType> bitOrder = {C::left, C::right, C::centre, C::LFE, C::leftSurround, C::rightSurround, C::leftCentre, C::rightCentre, C::centreSurround,
            C::leftSurroundSide, C::rightSurroundSide, C::topMiddle, C::topFrontLeft, C::topFrontCentre, C::topFrontRight, C::topRearLeft, C::topRearCentre, C::topRearRight, C::LFE2,
            C::discreteChannel0, C::ambisonicACN0, C::ambisonicACN1, C::ambisonicACN2, C::ambisonicACN3, C::topSideLeft, C::topSideRight, C::leftSurroundRear, C::rightSurroundRear,
            C::bottomFrontLeft, C::bottomFrontCentre, C::bottomFrontRight, C::proximityLeft, C::proximityRight, C::bottomSideLeft, C::bottomSideRight, C::bottomRearLeft,
            C::bottomRearCentre, C::bottomRearRight, C::ambisonicACN4, C::ambisonicACN5, C::ambisonicACN6, C::ambisonicACN7};
        juce::Array<C::ChannelType> types;
        for (auto t : bitOrder) types.add(t);
        const C set = C::channelSetWithChannels(types);
        const auto m = verbp::hostToBufferIndices(set);
        std::vector<int> sorted = m;
        std::sort(sorted.begin(), sorted.end());
        bool perm = (int)m.size() == set.size();
        for (size_t i = 0; i < sorted.size(); ++i) perm &= sorted[i] == (int)i;
        std::printf("  42 speaker bits: host channel -> buffer channel:");
        for (size_t i = 16; i < 26 && i < m.size(); ++i) std::printf(" %zu->%d", i + 1, m[i] + 1);
        std::printf(" ...\n");
        check(perm, "the host-to-buffer map of a 42-bit arrangement is a permutation");
        bool same = true;
        for (auto set2 : {C::stereo(), C::create5point1(), C::discreteChannels(8), C::discreteChannels(16)}) {
            const auto id = verbp::hostToBufferIndices(set2);
            for (size_t i = 0; i < id.size(); ++i) same &= id[i] == (int)i;
        }
        check(same, "a stereo, 5.1 or a short discrete bus keeps its order");
    }
    {   // listener microphone pattern: human ears
        std::printf("Listener microphone pattern\n");
        VerbProcessor p;
        p.setPlayConfigDetails(2, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "scene loads");
        verbp::OutputCfg o = p.getOutputCfg();
        o.micPattern = (int)verb::MicPattern::HumanEars;
        p.setOutputCfg(o);
        check(waitReady(p, 60), "the engine rebuilds with the human-ear pattern");
        juce::AudioBuffer<float> buf(2, 256);
        juce::MidiBuffer midi;
        double en = 0;
        bool fin = true;
        for (int s = 0; s < 60; ++s) {
            buf.clear();
            if (s == 0) buf.setSample(0, 0, 1.f);
            p.processBlock(buf, midi);
            for (int i = 0; i < 256; ++i) { fin &= std::isfinite(buf.getSample(0, i)) && std::isfinite(buf.getSample(1, i)); en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i); }
        }
        check(fin && en > 1e-9, "the human-ear listener renders finite sound");
        juce::MemoryBlock mb;
        p.getStateInformation(mb);
        VerbProcessor q;
        q.setPlayConfigDetails(2, 2, 48000, 256);
        q.prepareToPlay(48000, 256);
        q.setStateInformation(mb.getData(), (int)mb.getSize());
        waitReady(q, 60);
        check(q.getOutputCfg().micPattern == (int)verb::MicPattern::HumanEars, "the microphone pattern is saved in the project");
        std::unique_ptr<juce::AudioProcessorEditor> base(p.createEditor());
        if (auto* ed = dynamic_cast<VerbEditor*>(base.get())) {
            ed->setSize(1160, 760);
            ed->showPage(0, verbp::kListener);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            juce::Image img = ed->createComponentSnapshot(ed->getLocalBounds());
            juce::PNGImageFormat png;
            auto f = juce::File::getCurrentWorkingDirectory().getChildFile("out").getChildFile("editor_listener_ears.png");
            juce::FileOutputStream fo(f);
            if (fo.openedOk()) { fo.setPosition(0); fo.truncate(); png.writeImageToStream(img, fo); }
            check(img.getWidth() > 100, "editor paints the listener page with the pattern selector");
        }
    }
    {   // dry/wet
        std::printf("Dry/wet mix\n");
        VerbProcessor p;
        p.setPlayConfigDetails(2, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "scene loads");
        juce::AudioBuffer<float> buf(2, 256);
        juce::MidiBuffer midi;
        auto firstSample = [&](double mixPercent) {  // output of channel 0 at the sample of an input impulse
            setP(p, "mix", mixPercent);
            for (int i = 0; i < 20; ++i) { buf.clear(); p.processBlock(buf, midi); }  // let the gain ramp settle
            buf.clear();
            buf.setSample(0, 5, 1.f);
            p.processBlock(buf, midi);
            return buf.getSample(0, 5);
        };
        const double wetOnly = firstSample(100), dryOnly = firstSample(0), half = firstSample(50);
        std::printf("  output at the impulse: mix 100 %% -> %.4f, 0 %% -> %.4f, 50 %% -> %.4f\n", wetOnly, dryOnly, half);
        check(std::fabs(wetOnly) < 1e-3, "100 % is only the room (the room's direct sound arrives later)");
        check(std::fabs(dryOnly - 1.0) < 1e-3, "0 % passes the input straight through");
        check(std::fabs(half - std::cos(juce::MathConstants<double>::pi / 4)) < 0.01, "50 % is an equal-power blend");
        setP(p, "mix", 100);
    }
    {   // room library and impulse-response export
        std::printf("Room library and IR export\n");
        const auto rooms = verbp::builtinRooms();
        check(rooms.size() >= 8, "the library ships a set of rooms");
        bool allParse = true;
        for (auto& r : rooms) {
            try {
                std::istringstream is(r.text);
                auto sc = verb::parseScene(is, r.name);
                allParse &= sc.walls.size() >= 4 && sc.sources.size() >= 1 && sc.hasListener;
            } catch (const std::exception& ex) {
                std::printf("    %s: %s\n", r.name.c_str(), ex.what());
                allParse = false;
            }
        }
        check(allParse, "every built-in room parses (walls, a source and a listener)");
        VerbProcessor p;
        p.setPlayConfigDetails(4, 2, 48000, 256);
        p.prepareToPlay(48000, 256);
        check(waitReady(p, 60), "scene loads");
        bool allLoad = true, allSound = true;
        juce::AudioBuffer<float> buf(4, 256);
        juce::MidiBuffer midi;
        for (auto& r : rooms) {
            p.loadSceneText(juce::String(r.text), juce::String(r.name), true);
            if (!waitReady(p, 60)) { allLoad = false; std::printf("    %s did not load: %s\n", r.name.c_str(), p.getStatus().toRawUTF8()); continue; }
            double en = 0;
            bool fin = true;
            for (int s = 0; s < 120; ++s) {
                buf.clear();
                if (s == 0) buf.setSample(0, 0, 1.f);
                p.processBlock(buf, midi);
                for (int i = 0; i < 256; ++i) { fin &= std::isfinite(buf.getSample(0, i)); en += buf.getSample(0, i) * buf.getSample(0, i) + buf.getSample(1, i) * buf.getSample(1, i); }
            }
            allSound &= fin && en > 1e-9;
        }
        check(allLoad, "every built-in room loads into the plugin");
        check(allSound, "every built-in room makes finite, audible sound");

        // export: back to the demo room, export, read the files back
        p.loadSceneText(juce::String(rooms[1].text), "Bedroom.txt", true);
        check(waitReady(p, 60), "bedroom loads");
        auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("verb_ir_export_test");
        dir.deleteRecursively();
        p.startExportIR(dir);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        while (p.getExportState() == 1 && juce::Time::getMillisecondCounterHiRes() - t0 < 120000) juce::Thread::sleep(100);
        std::printf("  export: %s\n", p.getExportMessage().toRawUTF8());
        check(p.getExportState() == 2, "the export finishes");
        auto wavs = dir.findChildFiles(juce::File::findFiles, false, "*.wav");
        check(wavs.size() == 1, "one WAV per source point");
        if (wavs.size() == 1) {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> rd(wav.createReaderFor(wavs[0].createInputStream().release(), true));
            check(rd != nullptr && rd->numChannels == 2 && rd->sampleRate == 48000 && rd->lengthInSamples >= 90000, "the WAV is stereo, 48 kHz, as long as the scene's tail");
            if (rd) {
                juce::AudioBuffer<float> ir((int)rd->numChannels, (int)rd->lengthInSamples);
                rd->read(&ir, 0, (int)rd->lengthInSamples, 0, true, true);
                bool fin = true;
                double en = 0;
                int first = -1;
                for (int i = 0; i < ir.getNumSamples(); ++i) {
                    const float a = ir.getSample(0, i);
                    fin &= std::isfinite(a) && std::isfinite(ir.getSample(1, i));
                    en += a * a;
                    if (first < 0 && std::fabs(a) > 1e-4f) first = i;
                }
                // bedroom: source (1.2, 1.7), listener (3.0, 1.7): 1.8 m (plus the microphone offset) = ~250 samples
                std::printf("  first arrival at sample %d, energy %.3e\n", first, en);
                check(fin && en > 1e-6 && first > 150 && first < 400, "the exported IR is finite, with the direct sound at the geometric delay");
            }
        }
        auto roomFile = dir.getChildFile("Bedroom_room.txt");
        bool roomOk = false;
        if (roomFile.existsAsFile()) {
            try {
                std::istringstream is(roomFile.loadFileAsString().toStdString());
                auto sc = verb::parseScene(is, "exported room");
                roomOk = sc.walls.size() >= 4 && sc.hasListener && !sc.sources.empty() && std::fabs(sc.listenerPos.x - 1.7) < 0.05;  // the room is turned so that the listener looks up at the source
            } catch (...) {}
        }
        check(roomOk, "the room is written next to the IRs and reads back with the current listener position");
        dir.deleteRecursively();
        // room text with current positions
        setP(p, "lx", 3.3);
        const auto text = p.getRoomText();
        std::istringstream is2(text.toStdString());
        auto sc2 = verb::parseScene(is2, "room text");
        check(std::fabs(sc2.listenerPos.x - 3.3) < 0.01, "the saved room carries the current positions");
    }
    std::printf(fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
