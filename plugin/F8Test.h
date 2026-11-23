#pragma once
#include "AmbisonicsOutput.h"
#include "../src/geometry.hpp"
#include "../src/layouts.hpp"

static constexpr int f8Block = 512;
static constexpr int f8Blocks = 128;

static std::vector<float> f8Render(VerbProcessor& p, int ch) {
    std::vector<float> output((size_t)ch * f8Block * f8Blocks);
    juce::AudioBuffer<float> buf(ch, f8Block);
    juce::MidiBuffer midi;
    for (int b = -32; b < f8Blocks; ++b) {
        buf.clear();
        if (b == 0) buf.setSample(0, 0, 1.f);
        stage2Process(p, buf, midi);
        if (b >= 0) for (int c = 0; c < ch; ++c)
            std::copy_n(buf.getReadPointer(c), f8Block, output.data() + ((size_t)c * f8Blocks + b) * f8Block);
        juce::Thread::sleep(1);
    }
    return output;
}

static std::unique_ptr<VerbProcessor> f8Room(verb::Layout layout, int ch) {
    auto p = std::make_unique<VerbProcessor>();
    p->setTailSettings({8000, false});
    p->setPlayConfigDetails(1, ch, 48000, f8Block);
    p->prepareToPlay(48000, f8Block);
    auto cfg = p->getOutputCfg();
    cfg.layout = (int)layout;
    p->setOutputCfg(cfg);
    p->loadSceneText(
        "sr 48000\nlength 1\nheight 3\nseed 58\n"
        "material test 0.24 0.18 0.16 0.15 0.17 0.23 0.30 0.20 0.35\n"
        "floor test\nceiling test\n"
        "wall 0 0 8 0 test\nwall 8 0 8 6 test\nwall 8 6 0 6 test\nwall 0 6 0 0 test\n"
        "source mono 5.22 4.17 2.20 0 0 0\nlistener 2.62 2.19 1.32 18\nreceiver ref 2.62 2.19 1.32\n",
        "F8 room.txt", true);
    if (!waitReady(*p, 90)) throw std::runtime_error(p->getStatus().toStdString());
    return p;
}

static bool f8Settle(VerbProcessor& p, int ch) {
    juce::AudioBuffer<float> block(ch, f8Block);
    juce::MidiBuffer midi;
    const double stableAfter = juce::Time::getMillisecondCounterHiRes() + 750.;
    const double timeout = stableAfter + 30000.;
    do {
        block.clear();
        stage2Process(p, block, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(12);
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (now >= stableAfter && p.isTailCurrent()) return true;
        if (now >= timeout) return false;
    } while (true);
}

static int f8Order(int acn) { return (int)std::sqrt((double)acn); }

static int f8Acn(int normalization, int channel) {
    if (normalization != 2) return channel;
    // FuMa's WXYZ, then m=0,+1,-1,+2,-2,... within each higher order.
    if (channel < 4) return std::array<int, 4>{0, 3, 1, 2}[(size_t)channel];
    const int n = f8Order(channel);
    const int offset = channel - n*n;
    const int m = offset % 2 ? (offset + 1) / 2 : -offset / 2;
    return n*n + n + m;
}

static double f8Gain(int normalization, int levelDb, int acn) {
    const int n = f8Order(acn);
    double factor = 1.;
    if (normalization == 1) factor = std::sqrt(2.*n + 1.);
    if (normalization == 2) {
        if (n == 0) factor = std::sqrt(.5);
        else {
            // Use the harmonic's actual maximum. The nonzonal peaks follow from
            // differentiating the associated Legendre polynomial times (1-z^2)^(|m|/2).
            const int m = acn - n*n - n;
            double zSquared = m == 0 ? 1. : 0.;
            if (n == 2 && std::abs(m) == 1) zSquared = .5;
            if (n == 3 && std::abs(m) == 1) zSquared = 11./15.;
            if (n == 3 && std::abs(m) == 2) zSquared = 1./3.;
            const double az = m < 0 ? verb::kPi / (2. * std::abs(m)) : 0.;
            const double xy = std::sqrt(1. - zSquared);
            factor = 1. / std::abs(verb::shSN3D(acn, xy * std::cos(az), xy * std::sin(az), std::sqrt(zSquared)));
        }
    }
    return factor * std::pow(10., levelDb / 20.);
}

static void f8Format(VerbProcessor& p, int normalization, int levelDb) {
    auto output = p.getOutputCfg();
    output.ambisonicsNormalization = normalization;
    output.ambisonicsLevelDb = levelDb;
    p.setOutputCfg(output);
}

static double f8Energy(const std::vector<float>& audio, int ch, int from) {
    const auto start = audio.data() + ch * f8Block * f8Blocks;
    double energy = 0.;
    for (auto* p = start + from; p != start + f8Block * f8Blocks; ++p) energy += (double)*p * *p;
    return energy;
}

static void f8Baseline(const std::vector<float>& audio, verb::Layout layout, int channels) {
    const auto name = "baseline-" + std::to_string((int)layout) + "-" + std::to_string(channels) + ".f32";
    if (auto* file = std::fopen(name.c_str(), "rb")) {
        std::vector<float> old(audio.size());
        const auto count = std::fread(old.data(), sizeof(float), old.size(), file);
        std::fclose(file);
        check(count == old.size() && std::memcmp(old.data(), audio.data(), audio.size() * sizeof(float)) == 0, name.c_str());
    }
}

static void f8RoundTrips(VerbProcessor& source) {
    const auto layout = verb::Layout::AmbiX1;
    const auto legacy = source.getRoomPresetText();
    auto legacyXml = juce::parseXML(legacy);
    auto* legacyOut = legacyXml->getChildByName("VerbState")->getChildByName("Output");
    check(!legacyOut->hasAttribute("ambisonicsNormalization") && !legacyOut->hasAttribute("ambisonicsLevelDb"),
          "F8 legacy format fixture lacks both attributes");
    f8Format(source, 2, -6);
    const auto modern = source.getRoomPresetText();
    const auto dir = juce::File::getCurrentWorkingDirectory();
    dir.getChildFile("F8-FuMa.verbroom").replaceWithText(modern);
    dir.getChildFile("F8-legacy.verbroom").replaceWithText(legacy);
    juce::MemoryBlock project;
    source.getStateInformation(project);
    auto target = f8Room(layout, 4);
    target->setStateInformation(project.getData(), (int)project.getSize());
    check(waitReady(*target, 90) && target->getOutputCfg() == source.getOutputCfg(), "F8 modern project state round trip");
    auto legacyProject = juce::AudioProcessor::getXmlFromBinary(project.getData(), (int)project.getSize());
    auto* projectOut = legacyProject->getChildByName("Output");
    projectOut->removeAttribute("ambisonicsNormalization");
    projectOut->removeAttribute("ambisonicsLevelDb");
    juce::AudioProcessor::copyXmlToBinary(*legacyProject, project);
    target->setStateInformation(project.getData(), (int)project.getSize());
    check(waitReady(*target, 90) && target->getOutputCfg().ambisonicsNormalization == 0 && target->getOutputCfg().ambisonicsLevelDb == 0,
          "F8 old project restores SN3D / 0 dB");
    check(f8Settle(*target, 4), "F8 old project's tail settles");
    const auto oldProjectSound = f8Render(*target, 4);
    projectOut->setAttribute("ambisonicsNormalization", 0);
    projectOut->setAttribute("ambisonicsLevelDb", 0);
    juce::AudioProcessor::copyXmlToBinary(*legacyProject, project);
    target->setStateInformation(project.getData(), (int)project.getSize());
    check(waitReady(*target, 90) && f8Settle(*target, 4), "F8 comparison project explicitly stores defaults");
    const auto explicitProject = f8Render(*target, 4);
    check(f8Energy(oldProjectSound, 0, 4800) > 1.e-10
          && std::memcmp(oldProjectSound.data(), explicitProject.data(), explicitProject.size()*sizeof(float)) == 0,
          "F8 absent and explicit default settings give bit-exact, audible project output");

    juce::String error;
    target->setKeepRoutingEnabled(false);
    check(target->loadRoomPresetText(modern, error) && waitReady(*target, 90) && target->getOutputCfg() == source.getOutputCfg(),
          "F8 modern preset restores FuMa / -6 dB");
    f8Format(*target, 1, -3);
    target->setKeepRoutingEnabled(true);
    check(target->loadRoomPresetText(modern, error) && waitReady(*target, 90)
          && target->getOutputCfg().ambisonicsNormalization == 1 && target->getOutputCfg().ambisonicsLevelDb == -3,
          "F8 Keep I/O preserves the local normalization and level");
    target->setKeepRoutingEnabled(false);
    check(target->loadRoomPresetText(legacy, error) && waitReady(*target, 90)
          && target->getOutputCfg().ambisonicsNormalization == 0 && target->getOutputCfg().ambisonicsLevelDb == 0,
          "F8 old preset restores SN3D / 0 dB");
    check(f8Settle(*target, 4), "F8 old preset's tail settles");
    const auto oldPresetSound = f8Render(*target, 4);
    check(std::memcmp(oldPresetSound.data(), oldProjectSound.data(), oldPresetSound.size()*sizeof(float)) == 0,
          "F8 legacy preset reproduces the project impulse bit for bit");
    legacyOut->setAttribute("ambisonicsNormalization", 0);
    legacyOut->setAttribute("ambisonicsLevelDb", 0);
    check(target->loadRoomPresetText(legacyXml->toString(), error) && waitReady(*target, 90) && f8Settle(*target, 4),
          "F8 comparison preset explicitly stores defaults");
    const auto explicitDefaults = f8Render(*target, 4);
    check(std::memcmp(oldPresetSound.data(), explicitDefaults.data(), explicitDefaults.size()*sizeof(float)) == 0,
          "F8 absent and explicit default settings are bit-exact after preset reload");
}

static void f8Export(VerbProcessor& p, int count, const std::vector<float>& live) {
    auto dir = juce::File::getCurrentWorkingDirectory().getChildFile("f8-fuma-ir");
    p.startExportIR(dir);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(100);
    while (p.getExportState() == 1 && std::chrono::steady_clock::now() < until)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(15);
    check(p.getExportState() == 2, p.getExportMessage().toRawUTF8());
    const auto files = dir.findChildFiles(juce::File::findFiles, false, "*.wav");
    check(files.size() == 1, "F8 IR export writes one WAV for the mono point");
    if (files.size() != 1) return;
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(files[0].createInputStream().release(), true));
    check(reader && (int)reader->numChannels == count && reader->sampleRate == 48000., "F8 IR WAV has the Ambisonic channel count");
    if (!reader || (int)reader->numChannels != count) return;
    const int n = (int)std::min<juce::int64>(reader->lengthInSamples, f8Block*f8Blocks);
    juce::AudioBuffer<float> wav(count, n);
    const bool read = reader->read(wav.getArrayOfWritePointers(), count, 0, n);
    double worst = 0.;
    for (int ch = 0; ch < count; ++ch)
        for (int i = 0; i < n; ++i)
            worst = std::max(worst, std::abs((double)wav.getSample(ch, i) - live[(size_t)ch*f8Block*f8Blocks+i]));
    std::printf("  F8 IR %d samples per channel, max error %.9g\n", n, worst);
    check(read && n > 48000 && worst < 2.e-6, "F8 FuMa and level in exported WAV agree with live samples");
    const auto description = dir.findChildFiles(juce::File::findFiles, false, "*_room.txt");
    const auto room = description.size() == 1 ? description[0].loadFileAsString() : juce::String();
    check(room.contains("Ambisonics normalization: FuMa") && room.contains("Ambisonics level: -6 dB"),
          "F8 exported room describes the format and level");
}

static void f8ClassicEditor(VerbProcessor& p, bool ambisonic) {
    auto editor = std::make_unique<VerbEditor>(p);
    editor->setSize(900, 600);
    editor->showPage(0, verbp::kListener);
    juce::ComboBox* normalization = nullptr;
    juce::ComboBox* level = nullptr;
    for (int i = 0; i < editor->getNumChildComponents(); ++i) {
        if (auto* box = dynamic_cast<juce::ComboBox*>(editor->getChildComponent(i))) {
            if (box->getItemText(0) == "SN3D") normalization = box;
            if (box->getItemText(0) == "0 dB") level = box;
        }
    }
    const bool visible = normalization && level && normalization->isVisible() && level->isVisible();
    check(normalization && level && visible == ambisonic, "F8 classic editor shows Ambisonics controls only on Ambisonic layouts");
    if (visible) {
        auto usable = [&](juce::ComboBox* box) {
            return !box->getBounds().isEmpty() && editor->getLocalBounds().contains(box->getBounds());
        };
        check(usable(normalization) && usable(level), "F8 classic controls fit at the 900 x 600 minimum");
        normalization->setSelectedId(2, juce::sendNotificationSync);
        level->setSelectedId(2, juce::sendNotificationSync);
        const auto cfg = p.getOutputCfg();
        check(cfg.ambisonicsNormalization == 1 && cfg.ambisonicsLevelDb == -3, "F8 classic controls change the processor format");
    }
}

static int testF8() {
    for (const auto& spec : {std::pair{verb::Layout::StereoORTF, 2}, {verb::Layout::AmbiX1, 4},
                             {verb::Layout::AmbiX2, 9}, {verb::Layout::AmbiX3, 16},
                             {verb::Layout::AmbiX3, 2}, {verb::Layout::AmbiX1, 2}}) {
        const auto [layout, count] = spec;
        const bool ambisonic = verb::isAmbisonicLayout(layout);
        auto p = f8Room(layout, count);
        f8Baseline(f8Render(*p, count), layout, count);
        check(f8Settle(*p, count), "F8 reference engine and tail settle");
        const auto reference = f8Render(*p, count);
        double total = 0.;
        for (int ch = 0; ch < count; ++ch) total += f8Energy(reference, ch, 4800);
        std::printf("  F8 layout %d, %d channels, 0.1-1.365333 s: first %.12g, sum %.12g\n",
                    (int)layout, count, f8Energy(reference, 0, 4800), total);
        check(total > 0., "F8 reference tail is audible");
        const uint64_t serial = p->getLoadSerial();
        for (const auto& [normalization, db] : {std::pair{1, 0}, {2, 0}, {0, -3}, {0, -6}, {2, -6}}) {
            f8Format(*p, normalization, db);
            const auto allocated = stage2AudioAllocations;
            const auto rendered = f8Render(*p, count);
            check(stage2AudioAllocations == allocated, "F8 output conversion allocates no audio-thread memory");
            double worst = 0.;
            bool finite = true, math = true;
            const verbp::AmbisonicsOutput conversion(verbp::AmbisonicsOutput::encode(normalization, db));
            for (int c = 0; c < count; ++c) {
                const int acn = ambisonic ? f8Acn(normalization, c) : c;
                const double gain = ambisonic ? f8Gain(normalization, db, acn) : 1.;
                if (ambisonic) math &= conversion.acn(c) == acn && std::abs(conversion.gain(c) - gain) <= 2.e-7;
                for (int i = 0; i < f8Blocks*f8Block; ++i) {
                    const float sample = rendered[(size_t)c*f8Blocks*f8Block+i];
                    const double expected = acn < count ? reference[(size_t)acn*f8Blocks*f8Block+i]*gain : 0.;
                    finite &= std::isfinite(sample);
                    worst = std::max(worst, std::abs(sample - expected));
                }
            }
            std::printf("  F8 normalization=%d gain=%d dB, max error %.9g\n", normalization, db, worst);
            check(math && finite && worst <= 2.e-7, "F8 samples match the independent MaxN/ACN/dB reference");
            if (count == 16 && normalization == 2 && db == -6) f8Export(*p, count, rendered);
        }
        check(p->getLoadSerial() == serial && p->isTailCurrent(), "F8 output format changes keep the running engine and tail");
        if (!ambisonic || count == 4) f8ClassicEditor(*p, ambisonic);
        if (count == 4) {
            f8Format(*p, 0, 0);
            f8RoundTrips(*p);
        }
    }
    return fails ? 1 : 0;
}
