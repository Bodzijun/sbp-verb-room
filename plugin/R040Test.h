#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <vector>

static int testR040Stems() {
    const int before = fails;
    constexpr int sources = 4, channels = 2, block = 128, frames = 8192;
    std::istringstream text(R"(
sr 24000
length 0.35
height 3.5
material hard 0.08 0.10 0.12 0.15 0.18 0.22 0.25 0.10 0.70
material soft 0.05 0.12 0.30 0.50 0.65 0.70 0.70 0.20 0.60
floor soft
ceiling hard
wall 0 8 0 0 hard
wall 0 0 10 0 hard
wall 10 0 10 8 hard
wall 10 8 0 8 hard
listener 5 1.4 1.6 90
source S1 1.5 2.8 1.2 -90
source S2 2.5 2.8 1.3 -80
source S3 3.5 2.8 1.4 -70
source S4 4.5 2.8 1.2 -60
receiver L 5 1.4 1.6 55 0.5
receiver R 5 1.4 1.6 -55 0.5
)");
    const auto scene = verb::parseScene(text, "R040 stems room");
    struct Case {
        const char* name;
        bool creative, positionedDry, motion;
        int groups;
    };
    std::array<std::vector<float>, sources> program;
    for (int s = 0; s < sources; ++s) {
        program[s].resize(frames);
        for (int i = 0; i < 2048; ++i)
            program[s][i + 160 * s] = .1f * std::sin(i * (.074f + .086f * s));
    }
    for (const auto& test : {
             Case{"Physical", false, false, false, 2},
             Case{"Creative", true, false, false, 3},
             Case{"Positioned dry", false, true, false, 4},
             Case{"Motion", false, false, true, 2},
         }) {
        const auto started = std::chrono::steady_clock::now();
        verb::EngineConfig config;
        config.erOrder = 1;
        config.tailRays = 2048;
        config.rebuildWorkers = 2;
        config.tailThread = false;
        config.manualWorker = true;
        config.sharedTail = false;
        config.motionTail = test.motion;
        if (test.creative) config.decayMultipliers = {{1.3, 1.0, .8}};
        auto configure = [&](verb::Engine& engine) {
            engine.setReverbMix(test.positionedDry ? .5 : 1.);
            if (test.creative) {
                engine.setEarlyLevel(-3);
                engine.setTailLevel(3);
                engine.setCreativeWet(35, 160, false, 0, 300);
            }
        };
        std::array<std::array<float, block>, sources> input{};
        std::array<const float*, sources> in{};
        std::array<std::array<float, block>, channels> output{};
        std::array<float*, channels> out{};
        for (int s = 0; s < sources; ++s) in[s] = input[s].data();
        for (int ch = 0; ch < channels; ++ch) out[ch] = output[ch].data();

        verb::Engine reference(scene, config);
        configure(reference);
        auto ready = [&] {
            return reference.tailIsCurrent()
                && (!test.motion || reference.motionTailState() == verb::Engine::MotionTailState::Ready);
        };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            reference.process(in.data(), out.data(), block);
            reference.runWorkerStep();
        } while (!ready() && std::chrono::steady_clock::now() < deadline);
        if (!ready()) {
            std::printf("R040 %s\n", test.name);
            check(false, "stems reference tail becomes ready");
            continue;
        }

        std::vector<float> master(frames * channels);
        std::vector<double> sum(frames * channels);
        bool copied = true;
        auto renderConfig = config;
        // Start with a deferred tail, then select the required mode before
        // installing the reference IR. No render pass traces its own tail.
        renderConfig.motionTail = true;
        for (int group = -1; group < test.groups; ++group) {
            verb::Engine engine(scene, renderConfig);
            engine.setMotionTail(test.motion);
            copied &= engine.copyTailForOffline(reference);
            configure(engine);
            for (int offset = 0; offset < frames; offset += block) {
                for (int s = 0; s < sources; ++s) {
                    // Match Source Solo's input gate while retaining every source in the room.
                    const bool audible = group < 0 || s % test.groups == group;
                    for (int i = 0; i < block; ++i)
                        input[s][i] = audible ? program[s][offset + i] : 0.f;
                }
                engine.process(in.data(), out.data(), block);
                for (int ch = 0; ch < channels; ++ch) {
                    for (int i = 0; i < block; ++i) {
                        const int sample = ch * frames + offset + i;
                        if (group < 0) master[sample] = output[ch][i];
                        else sum[sample] += output[ch][i];
                    }
                }
            }
        }
        double signal = 0, error = 0, tail = 0;
        for (std::size_t i = 0; i < master.size(); ++i) {
            const double delta = sum[i] - master[i];
            signal += double(master[i]) * master[i];
            error += delta * delta;
            if (i % frames >= frames / 2)
                tail += double(master[i]) * master[i];
        }
        const double residualDb = 10 * std::log10(error / signal);
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::printf("R040 %s, %d groups: RMS residual %.2f dB relative to master (%.2f s)\n",
                    test.name, test.groups, residualDb, seconds);
        // ST1 measured about -140 dB in four-source cases. Allow 20 dB for
        // platform and signal differences while still detecting a lost stem.
        check(copied && tail > 1e-10 && error <= signal * 1e-12, "sum of solo stems matches the master");
    }
    return fails == before ? 0 : 1;
}
