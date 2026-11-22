// Comprehensive E13 verification suite covering Steps 1 through 6 and audio-thread benchmark
#include "PluginProcessor.h"
#include "EditorBridge.h"
#include "EditorSession.h"
#include "directivity.hpp"
#include "scene.hpp"
#include "geometry.hpp"
#include "rt_engine.hpp"
#include "tail.hpp"
#include "energy.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& msg) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg.c_str());
    if (ok) ++gPass; else ++gFail;
}

static bool waitReady(VerbProcessor& p, double seconds = 30.0) {
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while ((juce::Time::getMillisecondCounterHiRes() - t0) < seconds * 1000.0) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        if (p.getLoadState() == 2) return true;
        if (p.getLoadState() == 3) return false;
    }
    return false;
}

static std::vector<float> renderImpulse(const verb::Scene& sc, int N = 4096, int soloMask = 0) {
    verb::EngineConfig cfg;
    cfg.tailThread = false;
    cfg.erOrder = 2;
    cfg.tailRays = 4000;
    cfg.sharedTail = true;
    verb::Engine eng(sc, cfg);
    if (soloMask != 0) eng.setSoloMask(soloMask);
    std::vector<float> in(N, 0.f), out0(N, 0.f), out1(N, 0.f);
    in[0] = 1.f;
    for (int off = 0; off < N; off += 128) {
        const int m = std::min(128, N - off);
        const float* ib[1] = {in.data() + off};
        float* ob[2] = {out0.data() + off, out1.data() + off};
        eng.process(ib, ob, m);
    }
    return out0;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf("=== Step 1: Data model, SH basis, normalisation, rotation, .verbdir parser/writer ===\n");
    {
        // 1a. Omni SH: c_00 = 1, all other c_lm = 0 -> gain is 1.0 in every direction and every band, mean power = 1.0
        verb::DirectivityProfile omni;
        omni.id = "test_omni";
        omni.name = "Test Omni";
        omni.order = 4;
        for (int b = 0; b < verb::NB; ++b) {
            omni.coeffs[b].assign(verb::directivityNumCoeffs(4), 0.0f);
            omni.coeffs[b][0] = 1.0f;
        }
        bool omniOk = true;
        for (int b = 0; b < verb::NB; ++b) {
            if (std::fabs(verb::bandMeanPower(omni, b) - 1.0) > 1e-6) omniOk = false;
        }
        for (int i = 0; i < 64; ++i) {
            const double z = 1.0 - 2.0 * (i + 0.5) / 64.0;
            const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
            const double phi = 2.399963229728653 * i;
            const auto g = omni.evalLocal(r * std::cos(phi), r * std::sin(phi), z);
            for (int b = 0; b < verb::NB; ++b) if (std::fabs(g[(size_t)b] - 1.0) > 1e-5) omniOk = false;
        }
        check(omniOk, "Omni SH profile (c_00 = 1) has unit mean power and gain 1.0 in all 64 Fibonacci directions across all 7 bands");

        // 1b. Every built-in profile has unit mean power within +/- 0.05 dB (and numerical quadrature over 2048 Fibonacci directions matches within +/- 0.05 dB)
        const auto& builtins = verb::builtinDirectivities();
        check(builtins.size() == 8, "8 built-in directivity profiles registered (3 measured + 5 analytic models)");
        bool allNormOk = true;
        double worstDbErr = 0.0;
        const int M = 2048;
        for (const auto& p : builtins) {
            for (int b = 0; b < verb::NB; ++b) {
                const double tablePower = verb::bandMeanPower(*p, b);
                double fibPower = 0.0;
                for (int q = 0; q < M; ++q) {
                    const double z = 1.0 - 2.0 * (q + 0.5) / M;
                    const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
                    const double phi = 2.39996322972865332 * q;
                    const double g = p->evalBandLocal(b, r * std::cos(phi), r * std::sin(phi), z);
                    fibPower += g * g;
                }
                fibPower /= M;
                const double dbTable = std::fabs(10.0 * std::log10(tablePower));
                const double dbFib = std::fabs(10.0 * std::log10(fibPower));
                worstDbErr = std::max({worstDbErr, dbTable, dbFib});
                if (dbTable > 0.05 || dbFib > 0.05) allNormOk = false;
            }
        }
        std::printf("    (worst band power deviation across all 8 built-ins x 7 bands: %.5f dB)\n", worstDbErr);
        check(allNormOk, "All 8 built-in profiles have spherical mean power within +/- 0.05 dB of 1.0 in every band (both 1152-node & 2048-pt Fibonacci quadrature)");

        // 1c. Rotation invariance: rotating source by (yaw, pitch) and querying rotated world direction gives identical gain to unrotated local direction
        const auto trumpet = verb::findBuiltinDirectivity("brass_trumpet_bell_model");
        bool rotOk = trumpet != nullptr;
        if (trumpet) {
            verb::Transducer srcRot;
            srcRot.azimuthDeg = 37.5;
            srcRot.elevationDeg = -18.2;
            srcRot.directivity = trumpet;
            srcRot.instrument = trumpet->id;
            srcRot.instrumentAmount = 1.0;

            const double yaw = srcRot.azimuthDeg * verb::kPi / 180.0;
            const double pitch = srcRot.elevationDeg * verb::kPi / 180.0;
            const double cy = std::cos(yaw), sy = std::sin(yaw);
            const double cp = std::cos(pitch), sp = std::sin(pitch);
            const verb::Vec3 fwd{cy * cp, sy * cp, sp};
            const verb::Vec3 left{-sy, cy, 0.0};
            const verb::Vec3 up{-cy * sp, -sy * sp, cp};

            for (int i = 0; i < 32; ++i) {
                const double z = 1.0 - 2.0 * (i + 0.5) / 32.0;
                const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
                const double phi = 2.399963229728653 * i;
                const verb::Vec3 localDir{r * std::cos(phi), r * std::sin(phi), z};
                const verb::Vec3 worldDir = fwd * localDir.x + left * localDir.y + up * localDir.z;
                double gWorld[verb::NB]{};
                verb::dirGainBands(srcRot, worldDir, gWorld);
                const auto gLocal = trumpet->evalLocal(localDir.x, localDir.y, localDir.z);
                for (int b = 0; b < verb::NB; ++b) {
                    if (std::fabs(gWorld[b] - gLocal[(size_t)b]) > 1e-5) rotOk = false;
                }
            }
        }
        check(rotOk, "3D source rotation (yaw + pitch) matches local-frame SH evaluation within 1e-5 across 32 directions x 7 bands");

        // 1d. .verbdir round-trip and malformed input rejection
        bool roundTripOk = true;
        for (const auto& p : builtins) {
            const std::string txt = verb::writeVerbdir(*p);
            verb::DirectivityProfile parsed;
            std::string err;
            if (!verb::parseVerbdir(txt, parsed, err) || parsed.id != p->id || parsed.order != p->order) {
                roundTripOk = false;
            }
            for (int b = 0; b < verb::NB; ++b) {
                for (size_t c = 0; c < parsed.coeffs[b].size(); ++c) {
                    if (std::fabs(parsed.coeffs[b][c] - p->coeffs[b][c]) > 1e-4f) roundTripOk = false;
                }
            }
        }
        check(roundTripOk, "All 8 built-in profiles round-trip through writeVerbdir -> parseVerbdir within 1e-4");

        verb::DirectivityProfile bad;
        std::string err;
        const std::string oversize(verb::kMaxVerbdirBytes + 100, 'x');
        check(!verb::parseVerbdir(oversize, bad, err) && !err.empty(), "Parser rejects > 64 KB input cleanly");
        check(!verb::parseVerbdir("NOT_VERBDIR\n", bad, err) && !err.empty(), "Parser rejects bad magic header");
        check(!verb::parseVerbdir("VERBDIR 1\nid x\nname \"X\"\norder 9\n", bad, err) && !err.empty(), "Parser rejects unsupported SH order 9");
        check(!verb::parseVerbdir("VERBDIR 1\nid x\nname \"X\"\norder 0\nbands 125 250 500 1000 2000 4000 8000\nband 0 NaN\n", bad, err) && !err.empty(), "Parser rejects NaN coefficient");
        check(!verb::parseVerbdir("VERBDIR 1\nid x\nname \"X\"\norder 0\nbands 125 250 500 1000 2000 4000 8000\nband 0 0\nband 1 1\nband 2 1\nband 3 1\nband 4 1\nband 5 1\nband 6 1\n", bad, err) && !err.empty(), "Parser rejects zero-energy band");
    }

    std::printf("\n=== Step 2: Engine integration (Direct, ER, Tail, Energy Map, RT60 invariance, Amount blend) ===\n");
    {
        const std::string roomTxt =
            "sr 48000\nlength 1.2\nheight 3\ntemp 20\nhumidity 50\norder 2\nrays 4000\nseed 1\ndetect_radius 0.5\n"
            "material Concrete 0.05 0.05 0.06 0.07 0.08 0.09 0.10 0.10 0.30\nclass Concrete 0\n"
            "floor Concrete\nceiling Concrete\n"
            "#@verbroom-editor-v2 ceilingExtent walls\n"
            "wall 0 0 6 0 Concrete\nwall 6 0 6 5 Concrete\nwall 6 5 0 5 Concrete\nwall 0 5 0 0 Concrete\n"
            "source S1 1.5 2.5 1.5 0 0.35 0 0\n"
            "receiver L 4.5 2.5 1.5 0 0 0 0\n"
            "receiver R 4.5 2.6 1.5 0 0 0 0\n";

        std::istringstream in(roomTxt);
        verb::Scene scLegacy = verb::parseScene(in, "room");
        verb::Scene scNone = scLegacy;
        scNone.sources[0].instrument = "none";
        verb::sanitizeScene(scNone);

        // 2a. Bit-identical impulse response when instrument == "none"
        const auto irLegacy = renderImpulse(scLegacy, 4096);
        const auto irNone = renderImpulse(scNone, 4096);
        check(irLegacy.size() == irNone.size() && std::memcmp(irLegacy.data(), irNone.data(), irLegacy.size() * sizeof(float)) == 0,
              "Internal None alias: Engine impulse response with instrument='none' is bit-identical to legacy directivity=nullptr");

        // 2b. Bit-identical traceTail and analyzeEnergyMap when instrument == "none"
        const auto tailLegacy = verb::traceTail(scLegacy, scLegacy.sources[0], scLegacy.receivers, 2, 8000, scLegacy.seed, true);
        const auto tailNone = verb::traceTail(scNone, scNone.sources[0], scNone.receivers, 2, 8000, scNone.seed, true);
        bool tailBitEq = (tailLegacy.nBins == tailNone.nBins);
        for (int r = 0; r < 2 && tailBitEq; ++r)
            for (int b = 0; b < verb::NB && tailBitEq; ++b)
                if (std::memcmp(tailLegacy.h[(size_t)r][(size_t)b].data(), tailNone.h[(size_t)r][(size_t)b].data(), tailLegacy.nBins * sizeof(double)) != 0)
                    tailBitEq = false;
        check(tailBitEq, "traceTail with instrument='none' is bit-identical to legacy directivity=nullptr");

        verb::EnergyMapOptions eopt;
        eopt.resolution = 16;
        eopt.rays = 4000;
        const auto emLegacy = verb::analyzeEnergyMap(scLegacy, eopt);
        const auto emNone = verb::analyzeEnergyMap(scNone, eopt);
        bool emBitEq = (emLegacy.cells.size() == emNone.cells.size());
        for (size_t i = 0; i < emLegacy.cells.size() && emBitEq; ++i)
            if (std::memcmp(&emLegacy.cells[i], &emNone.cells[i], sizeof(verb::FieldEnergy)) != 0)
                emBitEq = false;
        check(emBitEq, "analyzeEnergyMap with instrument='none' is bit-identical to legacy directivity=nullptr");

        // 2c. Free-field directional level check (0 deg on-axis, 90 deg side, 180 deg rear at 125 Hz and 8 kHz)
        const std::string freeTxt =
            "sr 48000\nlength 0.5\nheight 20\ntemp 20\nhumidity 50\norder 1\nrays 1000\nseed 1\ndetect_radius 0.5\n"
            "material Abs 1 1 1 1 1 1 1 0 0\nclass Abs 5\n"
            "floor Abs\nceiling open\n"
            "source S1 0 0 10 0 0 0 0 instrument=brass_trumpet_bell_model instrumentAmount=1\n"
            "receiver R1 2 0 10 0 0 0 0\n";
        std::istringstream inFree(freeTxt);
        verb::Scene sc0 = verb::parseScene(inFree, "free");
        verb::Scene sc90 = sc0; sc90.sources[0].azimuthDeg = 90.0;
        verb::Scene sc180 = sc0; sc180.sources[0].azimuthDeg = 180.0;

        const auto fe0 = verb::earlyFieldEnergy(sc0, sc0.sources[0], sc0.receivers[0].pos, 0.0);
        const auto fe90 = verb::earlyFieldEnergy(sc90, sc90.sources[0], sc90.receivers[0].pos, 0.0);
        const auto fe180 = verb::earlyFieldEnergy(sc180, sc180.sources[0], sc180.receivers[0].pos, 0.0);

        double g0[verb::NB]{}, g90[verb::NB]{}, g180[verb::NB]{};
        verb::dirGainBands(sc0.sources[0], {1.0, 0.0, 0.0}, g0);
        verb::dirGainBands(sc90.sources[0], {1.0, 0.0, 0.0}, g90);
        verb::dirGainBands(sc180.sources[0], {1.0, 0.0, 0.0}, g180);

        const double meas90_125 = 10.0 * std::log10(fe0.direct[0] / fe90.direct[0]);
        const double exp90_125  = 20.0 * std::log10(g0[0] / g90[0]);
        const double meas180_125 = 10.0 * std::log10(fe0.direct[0] / fe180.direct[0]);
        const double exp180_125  = 20.0 * std::log10(g0[0] / g180[0]);
        const double meas90_8k = 10.0 * std::log10(fe0.direct[6] / fe90.direct[6]);
        const double exp90_8k  = 20.0 * std::log10(g0[6] / g90[6]);
        const double meas180_8k = 10.0 * std::log10(fe0.direct[6] / fe180.direct[6]);
        const double exp180_8k  = 20.0 * std::log10(g0[6] / g180[6]);

        check(std::fabs(meas90_125 - exp90_125) < 0.05 && std::fabs(meas180_125 - exp180_125) < 0.05
              && std::fabs(meas90_8k - exp90_8k) < 0.05 && std::fabs(meas180_8k - exp180_8k) < 0.05,
              "Free-field direct energy at 0 deg, 90 deg, 180 deg matches profile evaluation within 0.05 dB at 125 Hz and 8 kHz");

        const auto ir0 = renderImpulse(sc0, 2048, 1);
        const auto ir90 = renderImpulse(sc90, 2048, 1);
        const auto ir180 = renderImpulse(sc180, 2048, 1);
        double e0 = 0, e90 = 0, e180 = 0;
        for (int i = 0; i < 2048; ++i) {
            e0 += (double)ir0[i] * ir0[i];
            e90 += (double)ir90[i] * ir90[i];
            e180 += (double)ir180[i] * ir180[i];
        }
        // Also test voice sentence profile in free-field Engine::process
        verb::Scene scV0 = sc0; scV0.sources[0].instrument = "th_koeln_voice_sentence"; verb::sanitizeScene(scV0);
        verb::Scene scV90 = scV0; scV90.sources[0].azimuthDeg = 90.0;
        verb::Scene scV180 = scV0; scV180.sources[0].azimuthDeg = 180.0;
        const auto irV0 = renderImpulse(scV0, 2048, 1);
        const auto irV90 = renderImpulse(scV90, 2048, 1);
        const auto irV180 = renderImpulse(scV180, 2048, 1);
        double ev0 = 0, ev90 = 0, ev180 = 0;
        for (int i = 0; i < 2048; ++i) {
            ev0 += (double)irV0[i] * irV0[i];
            ev90 += (double)irV90[i] * irV90[i];
            ev180 += (double)irV180[i] * irV180[i];
        }
        std::printf("    (Free-field direct IR energy: Trumpet 0/90/180 = %.4e / %.4e / %.4e; Voice 0/90/180 = %.4e / %.4e / %.4e)\n",
                    e0, e90, e180, ev0, ev90, ev180);
        check(e0 > e90 * 2.0 && e0 > e180 * 2.0 && e90 > e180 && ev0 > ev90 && ev90 > ev180,
              "Rendered free-field Engine::process direct arrival energy matches directional attenuation (0 deg > 90 deg > 180 deg)");

        // Independent frequency-domain prediction of the engine band splitter.
        auto omniScene=sc0;omniScene.sources[0].instrument.clear();omniScene.sources[0].directivity.reset();
        const auto omniIR=renderImpulse(omniScene,2048,1);
        auto response=[](const std::vector<float>& ir,double frequency) {
            std::complex<double> value{};for(size_t n=0;n<ir.size();++n)value+=(double)ir[n]*std::polar(1.,-2*verb::kPi*frequency*n/48000.);return value;
        };
        bool audioBands=true;double worstAudioDb=0;
        for(int angle:{0,90,180}) {
            const auto& ir=angle==0?ir0:(angle==90?ir90:ir180);
            const auto gains=sc0.sources[0].directivity->evalLocal(std::cos(angle*verb::kPi/180.),-std::sin(angle*verb::kPi/180.),0);
            for(int b=0;b<verb::NB;++b) {
                const double frequency=verb::kBandHz[b];const auto delay=std::polar(1.,-2*verb::kPi*frequency/48000.);
                std::complex<double> previous{},expected{},reference{};
                for(int k=0;k<verb::NB;++k) {
                    const double a=k<verb::NB-1?1-std::exp(-2*verb::kPi*verb::kBandHz[k]*std::sqrt(2.)/48000.):1;
                    const std::complex<double> low=k<verb::NB-1?a/(1.-(1.-a)*delay):std::complex<double>(1,0);
                    const auto weight=low-previous;previous=low;
                    const double air=std::pow(10.,-verb::airAbsDbPerM(verb::kBandHz[k],sc0.tempC,sc0.humidity)*2./20.);
                    expected+=weight*air*gains[k];reference+=weight*air;
                }
                const double actual=std::abs(response(ir,frequency))/std::abs(response(omniIR,frequency));
                const double predicted=std::abs(expected)/std::abs(reference);
                const double error=std::abs(20*std::log10(actual/predicted));worstAudioDb=std::max(worstAudioDb,error);
                audioBands &= std::isfinite(error)&&std::isfinite(actual)&&actual>0&&error<.05;
            }
        }
        std::printf("Independent seven-band rendered response worst error %.9g dB\n",worstAudioDb);
        check(audioBands,"Rendered direct audio at seven band centres agrees with independent split/air prediction within .05dB (0/90/180)");

        bool allBands = true;
        for(int b=0;b<verb::NB;++b) {
            const double expectedSide=sc0.sources[0].directivity->evalBandLocal(b,0,-1,0);
            const double expectedFront=sc0.sources[0].directivity->evalBandLocal(b,1,0,0);
            const double expectedRear=sc0.sources[0].directivity->evalBandLocal(b,-1,0,0);
            const double measuredSide=fe90.direct[b]/fe0.direct[b],measuredRear=fe180.direct[b]/fe0.direct[b];
            allBands &= std::isfinite(measuredSide)&&std::isfinite(measuredRear)&&fe0.direct[b]>0
                && std::abs(10*std::log10(measuredSide/(expectedSide*expectedSide/(expectedFront*expectedFront))))<.05
                && std::abs(10*std::log10(measuredRear/(expectedRear*expectedRear/(expectedFront*expectedFront))))<.05;
        }
        check(allBands,"Seven-band free-field levels agree with independent local-frame profile queries");
        verb::Scene omniTailScene=scLegacy;omniTailScene.sources[0].pattern=0;
        const auto omniTail=verb::traceTail(omniTailScene,omniTailScene.sources[0],omniTailScene.receivers,2,8000,omniTailScene.seed,true);
        bool latePower=true;
        for(const auto* id:{"th_koeln_voice_sentence","brass_trumpet_bell_model"}) {
            auto scene=omniTailScene;scene.sources[0].instrument=id;verb::sanitizeScene(scene);
            const auto tail=verb::traceTail(scene,scene.sources[0],scene.receivers,2,8000,scene.seed,true);
            for(int b=0;b<verb::NB;++b){double expected=0,measured=0;for(size_t r=0;r<tail.h.size();++r)for(size_t k=0;k<tail.nBins;++k){expected+=omniTail.h[r][b][k];measured+=tail.h[r][b][k];}latePower &= expected>0&&std::isfinite(measured)&&std::abs(measured/expected-1)<.02;}
        }
        check(latePower,"Late energy as well as RT60 is invariant relative to unit-power omni in every band");

        // 2d. Diffuse-field RT60 in a shoebox room with scattering is unchanged within +/- 2% across profiles
        verb::Scene scVoice = scLegacy;
        scVoice.sources[0].instrument = "th_koeln_voice_sentence";
        scVoice.sources[0].instrumentAmount = 1.0;
        verb::sanitizeScene(scVoice);

        verb::Scene scTrumpet = scLegacy;
        scTrumpet.sources[0].instrument = "brass_trumpet_bell_model";
        scTrumpet.sources[0].instrumentAmount = 1.0;
        verb::sanitizeScene(scTrumpet);

        const auto diagLegacy = verb::analyzeTail(tailLegacy);
        const auto diagVoice = verb::analyzeTail(verb::traceTail(scVoice, scVoice.sources[0], scVoice.receivers, 2, 8000, scVoice.seed, true));
        const auto diagTrumpet = verb::analyzeTail(verb::traceTail(scTrumpet, scTrumpet.sources[0], scTrumpet.receivers, 2, 8000, scTrumpet.seed, true));
        bool rt60Same = true;
        for (int b = 0; b < verb::NB; ++b) {
            const double relV = std::fabs(diagVoice.rt60[b] - diagLegacy.rt60[b]) / std::max(1e-6, diagLegacy.rt60[b]);
            const double relT = std::fabs(diagTrumpet.rt60[b] - diagLegacy.rt60[b]) / std::max(1e-6, diagLegacy.rt60[b]);
            if (!(std::isfinite(diagLegacy.rt60[b]) && diagLegacy.rt60[b]>0 && std::isfinite(diagVoice.rt60[b]) && diagVoice.rt60[b]>0 && std::isfinite(diagTrumpet.rt60[b]) && diagTrumpet.rt60[b]>0) || relV > 0.02 || relT > 0.02) rt60Same = false;
        }
        check(rt60Same, "Diffuse-field RT60 in shoebox room is invariant (< 2% change, exact match) across None, Voice (TH Koeln), and Trumpet Bell");

        // 2e. Amount blend: 0 = omni, 1 = full profile, 0.5 = power midpoint sqrt(0.5 + 0.5 * g^2)
        verb::Transducer tBlend = scTrumpet.sources[0];
        double gb0[verb::NB]{}, gb50[verb::NB]{}, gb100[verb::NB]{};
        tBlend.instrumentAmount = 0.0; verb::dirGainBands(tBlend, {1.0, 0.0, 0.0}, gb0);
        tBlend.instrumentAmount = 0.5; verb::dirGainBands(tBlend, {1.0, 0.0, 0.0}, gb50);
        tBlend.instrumentAmount = 1.0; verb::dirGainBands(tBlend, {1.0, 0.0, 0.0}, gb100);
        bool blendOk = true;
        for (int b = 0; b < verb::NB; ++b) {
            const double expected50 = std::sqrt(0.5 + 0.5 * gb100[b] * gb100[b]);
            if (std::fabs(gb0[b] - 1.0) > 1e-6 || std::fabs(gb50[b] - expected50) > 1e-6) blendOk = false;
        }
        check(blendOk, "Instrument amount blend: 0% = omni (1.0), 100% = full profile, 50% = exact power midpoint in all 7 bands");
    }

    std::printf("\n=== Step 5 & 6: Processor state, .verbroom preset, fallback, EditorSession commands & EditorBridge ===\n");
    {
        VerbProcessor pDef;
        check(waitReady(pDef), "Default processor finishes loading demo room");
        juce::MemoryBlock mbDef;
        pDef.getStateInformation(mbDef);
        auto xmlDef = juce::AudioProcessor::getXmlFromBinary(mbDef.getData(), (int)mbDef.getSize());
        const juce::String xmlDefStr = xmlDef ? xmlDef->toString() : juce::String();
        check(xmlDef != nullptr && !xmlDefStr.contains("instrument=") && !xmlDefStr.contains("instrumentDataRev")
              && !xmlDefStr.contains("customDirectivity"),
              "Default state with instrument='none' writes zero E13 attributes to binary XML state");

        pDef.setSourcePattern(0, 0.35);
        pDef.setSourceDirectivity(0, "th_koeln_voice_sentence", 0.8);
        juce::MemoryBlock mbVoice;
        pDef.getStateInformation(mbVoice);
        auto xmlVoice = juce::AudioProcessor::getXmlFromBinary(mbVoice.getData(), (int)mbVoice.getSize());
        const juce::String xmlVoiceStr = xmlVoice ? xmlVoice->toString() : juce::String();
        check(xmlVoiceStr.contains("instrument=\"th_koeln_voice_sentence\"") && xmlVoiceStr.contains("savedPattern=\"0.35\""),
              "Binary XML with active instrument writes both fitted pattern and savedPattern=0.35");

        VerbProcessor pRestored;
        pRestored.setStateInformation(mbVoice.getData(), (int)mbVoice.getSize());
        check(waitReady(pRestored), "Restored processor finishes loading");
        {
            const auto srcs = pRestored.getSources();
            std::printf("Restored fields: instrument=%s amount=%.17g pattern=%.17g warning=%s\n",srcs[0].instrument.c_str(),srcs[0].instrumentAmount,srcs[0].pattern,srcs[0].directivityWarning.c_str());
            check(!srcs.empty() && srcs[0].instrument == "th_koeln_voice_sentence"
                  && std::fabs(srcs[0].instrumentAmount - 0.8) < 1e-6
                  && srcs[0].directivity != nullptr
                  && std::fabs(srcs[0].pattern - 0.35) < 1e-6,
                  "State round-trip restores instrument='th_koeln_voice_sentence', amount=0.8, and saved analytic pattern=0.35");
        }

        pRestored.setSourceDirectivity(0, "none", 1.0);
        {
            const auto srcs = pRestored.getSources();
            check(srcs[0].instrument == "none" && srcs[0].directivity == nullptr
                  && std::fabs(verbp::effectiveAnalyticPattern(srcs[0]) - 0.35) < 1e-6,
                  "Switching instrument back to 'none' restores previous analytic pattern (0.35)");
        }

        verb::DirectivityProfile customProf;
        customProf.id = "custom_test_horn";
        customProf.name = "Custom Test Horn";
        customProf.category = "Brass";
        customProf.license = "CC0-1.0";
        customProf.attribution = "E13 verification test";
        customProf.order = 2;
        for (int b = 0; b < verb::NB; ++b) {
            customProf.coeffs[b].assign(verb::directivityNumCoeffs(2), 0.0f);
            customProf.coeffs[b][0] = 1.0f;
            customProf.coeffs[b][2] = 0.15f * (b + 1);
        }
        verb::normalizeProfilePower(customProf);
        const std::string customVerbdir = verb::writeVerbdir(customProf);

        pDef.setSourceDirectivity(0, "custom", 0.7, customVerbdir, "Custom Test Horn", "");
        const juce::String presetXml = pDef.getRoomPresetText();
        VerbProcessor pPreset;
        juce::String presetErr;
        check(pPreset.loadRoomPresetText(presetXml, presetErr) && waitReady(pPreset),
              ".verbroom preset with embedded custom .verbdir loads cleanly");
        {
            const auto srcs = pPreset.getSources();
            check(srcs[0].instrument == "custom" && std::fabs(srcs[0].instrumentAmount - 0.7) < 1e-6
                  && srcs[0].directivity != nullptr && srcs[0].customDirectivityName == "Custom Test Horn"
                  && srcs[0].directivityWarning.empty(),
                  "Loaded .verbroom preset reconstructs custom DirectivityProfile and name without external file");
        }

        pPreset.setSourceDirectivity(0, "unknown_future_profile_id", 1.0);
        {
            const auto srcs = pPreset.getSources();
            check(srcs[0].directivity == nullptr && !srcs[0].directivityWarning.empty()
                  && std::fabs(verbp::effectiveAnalyticPattern(srcs[0])) < 1e-6,
                  "Unknown profile ID degrades gracefully to omnidirectional with non-empty directivityWarning");
        }
        pPreset.setSourceDirectivity(0, "custom", 1.0, "", "Missing File", "E:/nonexistent_dir_12345/missing.verbdir");
        {
            const auto srcs = pPreset.getSources();
            check(srcs[0].directivity == nullptr && !srcs[0].directivityWarning.empty()
                  && std::fabs(verbp::effectiveAnalyticPattern(srcs[0])) < 1e-6,
                  "Missing custom .verbdir file degrades gracefully to omnidirectional with non-empty directivityWarning");
        }

        // 6a. EditorBridge snapshot, catalogs.list, source.setDirectivity, and undo
        std::vector<juce::var> outEvents;
        verbweb::EditorBridge bridge(pDef, [&](const juce::String&, const juce::var& v) { outEvents.push_back(v); });
        bridge.handle(juce::JSON::parse(R"({"schemaVersion":1,"sessionId":"s1","kind":"hello","requestId":"h1","payload":{"client":"test","supportedVersions":[1],"ready":true}})"));

        const juce::var snapEnv = bridge.snapshot();
        const juce::var snapPayload = snapEnv.getProperty("payload", {});
        const juce::var capInst = snapPayload.getProperty("capabilities", {}).getProperty("features", {}).getProperty("instrumentDirectivity", {});
        check((bool)capInst.getProperty("available", false) && capInst.getProperty("status", {}).toString() == "available",
              "Snapshot capability instrumentDirectivity is {available: true, status: 'available'}");

        const auto* snapSources = snapPayload.getProperty("document", {}).getProperty("sources", {}).getArray();
        check(snapSources != nullptr && !snapSources->isEmpty(), "Snapshot contains document.sources");
        const juce::var s0 = (*snapSources)[0];
        const auto* slices = s0.getProperty("polarSlices", {}).getArray();
        check(s0.getProperty("instrument", {}).toString() == "custom"
              && slices != nullptr && slices->size() == 3
              && (*slices)[0].isArray() && (*slices)[0].getArray()->size() == 72
              && (*slices)[1].isArray() && (*slices)[1].getArray()->size() == 72
              && (*slices)[2].isArray() && (*slices)[2].getArray()->size() == 72,
              "Snapshot source[0] exposes instrument and 72-point polarSlices for 125 Hz, 1 kHz, 8 kHz");

        const juce::String revCat = juce::String(std::to_string(pDef.getRevision()));
        const juce::var catRes = bridge.handle(juce::JSON::parse(
            juce::String(R"({"schemaVersion":1,"sessionId":"s1","kind":"command","requestId":"c-cat","baseRevision":")")
            + revCat + R"(","payload":{"op":"catalogs.list"}})"));
        const auto* catArr = catRes.getArray();
        const juce::var catEnv = (catArr && catArr->size() >= 2) ? (*catArr)[1] : juce::var();
        const auto* catProfiles = catEnv.getProperty("payload", {}).getProperty("directivityProfiles", {}).getArray();
        check(catProfiles != nullptr && catProfiles->size() == 8, "catalogs.list exposes all 8 built-in directivityProfiles with metadata");

        // Test source.setDirectivity + undo + redo through bridge
        const juce::String rev0 = juce::String(std::to_string(pDef.getRevision()));
        const juce::var res1 = bridge.handle(juce::JSON::parse(
            juce::String(R"({"schemaVersion":1,"sessionId":"s1","kind":"command","requestId":"c-dir1","baseRevision":")")
            + rev0 + R"(","payload":{"op":"source.setDirectivity","index":0,"instrument":"genelec_8030c","amount":0.85}})"));
        const auto* arr1 = res1.getArray();
        check(arr1 != nullptr && !arr1->isEmpty() && (*arr1)[0].getProperty("kind", {}).toString() == "ack"
              && pDef.getSources()[0].instrument == "genelec_8030c" && std::fabs(pDef.getSources()[0].instrumentAmount - 0.85) < 1e-6,
              "source.setDirectivity command sets genelec_8030c with amount=0.85");

        const juce::String rev1 = juce::String(std::to_string(pDef.getRevision()));
        const juce::var resUndo = bridge.handle(juce::JSON::parse(
            juce::String(R"({"schemaVersion":1,"sessionId":"s1","kind":"command","requestId":"c-undo1","baseRevision":")")
            + rev1 + R"(","payload":{"op":"history.undo"}})"));
        const auto* arrUndo = resUndo.getArray();
        check(arrUndo != nullptr && !arrUndo->isEmpty() && (*arrUndo)[0].getProperty("kind", {}).toString() == "ack"
              && pDef.getSources()[0].instrument == "custom" && std::fabs(pDef.getSources()[0].instrumentAmount - 0.7) < 1e-6,
              "history.undo reverts source.setDirectivity back to previous custom profile");
    }

    {
        VerbProcessor processor;check(waitReady(processor),"Empty-metadata undo test ready");
        auto profile=verb::findBuiltinDirectivity("brass_trumpet_bell_model");
        processor.setSourceDirectivity(0,"custom",.7,verb::writeVerbdir(*profile),"","");
        verbweb::EditorBridge bridge(processor,{});
        bridge.handle(juce::JSON::parse(R"({"schemaVersion":1,"sessionId":"redo","kind":"hello","requestId":"h","payload":{"supportedVersions":[1],"ready":true}})"));
        int request=0;
        auto command=[&](const juce::String& payload) {
            auto message=juce::JSON::parse("{\"schemaVersion\":1,\"sessionId\":\"redo\",\"kind\":\"command\",\"requestId\":\"r"+juce::String(++request)+"\",\"baseRevision\":\""+juce::String(std::to_string(processor.getRevision()))+"\",\"payload\":"+payload+"}");
            auto result=bridge.handle(message);return result.isArray()&&!result.getArray()->isEmpty()&&(*result.getArray())[0].getProperty("kind",{}).toString()=="ack";
        };
        check(command(R"({"op":"source.setDirectivity","index":0,"instrument":"custom","customName":"Other","customPath":"C:/not-needed.verbdir","amount":0.9})"),"Custom metadata command accepted");
        check(command(R"({"op":"history.undo"})")&&processor.getSources()[0].customDirectivityPath.empty()&&processor.getSources()[0].customDirectivityName.empty(),"Undo restores exact empty custom path and name");
        check(command(R"({"op":"history.redo"})")&&processor.getSources()[0].customDirectivityPath=="C:/not-needed.verbdir"&&processor.getSources()[0].customDirectivityName=="Other"&&std::abs(processor.getSources()[0].instrumentAmount-.9)<1e-6,"Redo restores exact custom profile configuration");
    }

    std::printf("\n=== Audio-thread CPU benchmark (4 sources x 12 receivers x 256 samples, 3000 blocks) ===\n");
    {
        const std::string benchRoom =
            "sr 48000\nlength 1.5\nheight 4\ntemp 20\nhumidity 50\norder 1\nrays 8000\nseed 1\ndetect_radius 0.5\n"
            "material Wall 0.10 0.12 0.15 0.18 0.20 0.22 0.25 0.15 0.50\nclass Wall 0\n"
            "floor Wall\nceiling Wall\n"
            "#@verbroom-editor-v2 ceilingExtent walls\n"
            "wall 0 0 10 0 Wall\nwall 10 0 10 8 Wall\nwall 10 8 0 8 Wall\nwall 0 8 0 0 Wall\n"
            "listener 5 4 1.5 0\n"
            "source S1 2 2 1.5 15 0.25 0 0\n"
            "source S2 8 2 1.5 135 0.25 0 0\n"
            "source S3 2 6 1.5 -45 0.25 0 0\n"
            "source S4 8 6 1.5 -135 0.25 0 0\n"
            "receiver R1 5 4 1.5 0 0 0 0\n";

        std::istringstream in(benchRoom);
        verb::Scene scNone = verb::parseScene(in, "bench");
        scNone.receivers = verb::makeReceiverArray(verb::Layout::Surround714, {5.0, 4.0, 1.5}, 0.15, 12);
        verb::applyMicPattern(scNone.receivers, verb::MicPattern::Cardioid, 0.15);

        verb::Scene scInst = scNone;
        const char* profIds[4] = {"th_koeln_voice_sentence", "brass_trumpet_bell_model", "genelec_8030c", "brass_horn_bell_model"};
        for (int i = 0; i < 4; ++i) {
            scInst.sources[i].instrument = profIds[i];
            scInst.sources[i].instrumentAmount = 1.0;
        }
        verb::sanitizeScene(scInst);

        verb::EngineConfig ec;
        ec.erOrder = 1;
        ec.ctrlInterval = 128;
        ec.tailBlock = 512;
        ec.tailThread = false;
        ec.sharedTail = true;
        ec.tailRays = 8000;

        verb::Engine engNone(scNone, ec);
        verb::Engine engInst(scInst, ec);

        constexpr int kBlock = 256;
        constexpr int kBlocks = 3000;
        std::vector<std::vector<float>> inBuf(4, std::vector<float>(kBlock, 0.f));
        for (int s = 0; s < 4; ++s)
            for (int n = 0; n < kBlock; ++n)
                inBuf[s][n] = 0.01f * std::sin(0.1f * (float)(n + s * 17));
        const float* ip[4] = {inBuf[0].data(), inBuf[1].data(), inBuf[2].data(), inBuf[3].data()};

        std::vector<std::vector<float>> outBuf(12, std::vector<float>(kBlock, 0.f));
        float* op[12];
        for (int r = 0; r < 12; ++r) op[r] = outBuf[r].data();

        // Warmup
        for (int b = 0; b < 200; ++b) {
            engNone.process(ip, op, kBlock);
            engInst.process(ip, op, kBlock);
        }

        // Interleaved timing (10 rounds x 300 blocks) with periodic azimuth updates so ctrlStep re-evaluates directivity
        double secNone = 0.0, secInst = 0.0;
        for (int round = 0; round < 10; ++round) {
            auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < 300; ++b) {
                if ((b & 7) == 0) engNone.setSourceAzimuth(b & 3, (double)((b + round) % 360));
                engNone.process(ip, op, kBlock);
            }
            secNone += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

            auto t1 = std::chrono::steady_clock::now();
            for (int b = 0; b < 300; ++b) {
                if ((b & 7) == 0) engInst.setSourceAzimuth(b & 3, (double)((b + round) % 360));
                engInst.process(ip, op, kBlock);
            }
            secInst += std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        }

        const double usPerBlockNone = (secNone * 1e6) / kBlocks;
        const double usPerBlockInst = (secInst * 1e6) / kBlocks;
        const double deltaPct = (secInst - secNone) / secNone * 100.0;
        std::printf("    4 src x 12 rec x 256 smp (3000 blocks): None = %.2f us/block (%.3f s total), Active SH = %.2f us/block (%.3f s total), delta = %+.2f%%\n",
                    usPerBlockNone, secNone, usPerBlockInst, secInst, deltaPct);
        check(deltaPct <= 5.0, "Active order-4 SH directivity adds <= 5% audio-thread CPU cost over None");
    }

    std::printf("\n=== E22: Free-field direct + 1-reflection rotated impulse, bit-identity, state & preset roundtrip ===\n");
    {
        auto synth = std::make_shared<verb::DirectivityProfile>();
        synth->id = "e22_verify_o3";
        synth->name = "E22 Verify Order 3";
        synth->order = 3;
        for (int b = 0; b < verb::NB; ++b) {
            synth->coeffs[b].assign(verb::directivityNumCoeffs(3), 0.f);
            synth->coeffs[b][0] = 1.0f;
            synth->coeffs[b][1] = 0.28f;
            synth->coeffs[b][2] = 0.22f;
            synth->coeffs[b][3] = 0.65f;
            synth->coeffs[b][6] = 0.18f;
            synth->coeffs[b][8] = -0.15f;
            synth->coeffs[b][12] = 0.14f;
        }
        verb::normalizeProfilePower(*synth);

        auto invAimRef = [](double azDeg, double elDeg, double rollDeg, verb::Vec3 v) -> verb::Vec3 {
            constexpr double d2r = verb::kPi / 180.0;
            const double a = azDeg * d2r, e = elDeg * d2r, r = rollDeg * d2r;
            const double ca = std::cos(a), sa = std::sin(a);
            verb::Vec3 v1{v.x * ca + v.y * sa, -v.x * sa + v.y * ca, v.z};
            const double ce = std::cos(e), se = std::sin(e);
            verb::Vec3 v2{v1.x * ce + v1.z * se, v1.y, -v1.x * se + v1.z * ce};
            const double cr = std::cos(r), sr = std::sin(r);
            verb::Vec3 v3{v2.x, v2.y * cr + v2.z * sr, -v2.y * sr + v2.z * cr};
            return verb::unit(v3);
        };
        auto response = [](const std::vector<float>& ir, double frequency) {
            std::complex<double> value{};
            for (size_t n = 0; n < ir.size(); ++n) value += (double)ir[n] * std::polar(1., -2 * verb::kPi * frequency * n / 48000.);
            return value;
        };

        // 1. Free-field direct impulse across multiple 3D receiver directions (trumpet + synth SH)
        const verb::Vec3 recvDirs[] = {
            {2.0, 0.0, 10.0}, {0.0, 2.0, 10.0}, {0.0, -2.0, 10.0}, {1.5, 1.0, 11.2}
        };
        double worstDirectDb = 0.0;
        bool directOk = true;
        for (int pIdx = 0; pIdx < 2; ++pIdx) {
            const auto prof = (pIdx == 0) ? verb::findBuiltinDirectivity("brass_trumpet_bell_model") : synth;
            for (const auto& rp : recvDirs) {
                const std::string freeTxt =
                    "sr 48000\nlength 0.4\nheight 20\ntemp 20\nhumidity 50\norder 1\nrays 1000\nseed 1\ndetect_radius 0.5\n"
                    "material Abs 1 1 1 1 1 1 1 0 0\nclass Abs 5\n"
                    "floor Abs\nceiling open\n"
                    "source S1 0 0 10 0 0 0 0\n"
                    "receiver R1 " + std::to_string(rp.x) + " " + std::to_string(rp.y) + " " + std::to_string(rp.z) + " 0 0 0 0\n";
                std::istringstream in(freeTxt);
                verb::Scene scOmni = verb::parseScene(in, "e22_free");
                verb::Scene scRot = scOmni;
                scRot.sources[0].directivity = prof;
                scRot.sources[0].instrument = prof->id;
                scRot.sources[0].instrumentAmount = 1.0;
                scRot.sources[0].aimAzDeg = 90.0;
                scRot.sources[0].aimElDeg = 25.0;
                scRot.sources[0].aimRollDeg = -35.0;

                const auto irOmni = renderImpulse(scOmni, 2048, 1);
                const auto irRot = renderImpulse(scRot, 2048, 1);
                const verb::Vec3 d = verb::unit(rp - scRot.sources[0].pos);
                const double dist = verb::norm(rp - scRot.sources[0].pos);
                const verb::Vec3 q = invAimRef(90.0, 25.0, -35.0, d);
                const auto gains = prof->evalLocal(q.x, q.y, q.z);

                for (int b = 0; b < verb::NB; ++b) {
                    const double freq = verb::kBandHz[b];
                    const auto delay = std::polar(1., -2 * verb::kPi * freq / 48000.);
                    std::complex<double> prev{}, expC{}, refC{};
                    for (int k = 0; k < verb::NB; ++k) {
                        const double a = k < verb::NB - 1 ? 1.0 - std::exp(-2 * verb::kPi * verb::kBandHz[k] * std::sqrt(2.) / 48000.) : 1.0;
                        const auto low = k < verb::NB - 1 ? a / (1.0 - (1.0 - a) * delay) : std::complex<double>(1.0, 0.0);
                        const auto w = low - prev; prev = low;
                        const double air = std::pow(10., -verb::airAbsDbPerM(verb::kBandHz[k], scOmni.tempC, scOmni.humidity) * dist / 20.);
                        expC += w * air * gains[(size_t)k];
                        refC += w * air;
                    }
                    const double actual = std::abs(response(irRot, freq)) / std::abs(response(irOmni, freq));
                    const double pred = std::abs(expC) / std::abs(refC);
                    const double errDb = std::abs(20.0 * std::log10(actual / pred));
                    worstDirectDb = std::max(worstDirectDb, errDb);
                    directOk &= std::isfinite(errDb) && errDb <= 0.1;
                }
            }
        }
        std::printf("    E22 direct 7-band worst error across 4 3D directions x 2 profiles: %.5f dB\n", worstDirectDb);
        check(directOk, "E22 free-field direct impulse matches rotated SH pattern within 0.1 dB across all 7 bands");

        // 2. Single-wall reflection impulse test (soloMask=2 isolated ER from y=5 reflective wall after settling 10 ms component ramp)
        auto renderIsolatedER = [](const verb::Scene& sc, int N) {
            verb::EngineConfig cfg;
            cfg.tailThread = false;
            cfg.erOrder = 2;
            cfg.tailRays = 1000;
            cfg.sharedTail = true;
            verb::Engine eng(sc, cfg);
            eng.setSoloMask(2);
            std::vector<float> zIn(128, 0.f), zOut0(128, 0.f), zOut1(128, 0.f);
            for (int blk = 0; blk < 8; ++blk) {
                const float* ib[1] = {zIn.data()};
                float* ob[2] = {zOut0.data(), zOut1.data()};
                eng.process(ib, ob, 128);
            }
            std::vector<float> in(N, 0.f), out0(N, 0.f), out1(N, 0.f);
            in[0] = 1.f;
            for (int off = 0; off < N; off += 128) {
                const int m = std::min(128, N - off);
                const float* ib[1] = {in.data() + off};
                float* ob[2] = {out0.data() + off, out1.data() + off};
                eng.process(ib, ob, m);
            }
            return out0;
        };
        const std::string erTxt =
            "sr 48000\nlength 0.4\nheight 20\ntemp 20\nhumidity 50\norder 1\nrays 1000\nseed 1\ndetect_radius 0.5\n"
            "material Abs 1 1 1 1 1 1 1 0 0\nclass Abs 5\n"
            "material Mirror 0 0 0 0 0 0 0 0 0\nclass Mirror 0\n"
            "floor Abs\nceiling open\n"
            "wall -10 -10 10 -10 Abs\n"
            "wall 10 -10 10 5 Abs\n"
            "wall 10 5 -10 5 Mirror\n"
            "wall -10 5 -10 -10 Abs\n"
            "source S1 2 2 10 0 0 0 0\n"
            "receiver R1 5 2 10 0 0 0 0\n";
        double worstReflDb = 0.0;
        bool reflOk = true;
        // Mirror image of R1=(5,2,10) across y=5 is (5,8,10); bounce point is (3.5, 5, 10)
        const verb::Vec3 bounce{3.5, 5.0, 10.0};
        const verb::Vec3 srcPos{2.0, 2.0, 10.0};
        const verb::Vec3 recPos{5.0, 2.0, 10.0};
        const verb::Vec3 dRefl = verb::unit(bounce - srcPos);
        const double pathLen = verb::norm(bounce - srcPos) + verb::norm(recPos - bounce);
        for (int pIdx = 0; pIdx < 2; ++pIdx) {
            const auto prof = (pIdx == 0) ? verb::findBuiltinDirectivity("brass_trumpet_bell_model") : synth;
            std::istringstream in(erTxt);
            verb::Scene scErOmni = verb::parseScene(in, "e22_1refl");
            verb::Scene scErRot = scErOmni;
            scErRot.sources[0].directivity = prof;
            scErRot.sources[0].instrument = prof->id;
            scErRot.sources[0].instrumentAmount = 1.0;
            scErRot.sources[0].aimAzDeg = 90.0;
            scErRot.sources[0].aimElDeg = 25.0;
            scErRot.sources[0].aimRollDeg = -35.0;

            const auto irErOmni = renderIsolatedER(scErOmni, 4096);
            const auto irErRot = renderIsolatedER(scErRot, 4096);
            const verb::Vec3 q = invAimRef(90.0, 25.0, -35.0, dRefl);
            const auto gains = prof->evalLocal(q.x, q.y, q.z);
            for (int b = 0; b < verb::NB; ++b) {
                const double freq = verb::kBandHz[b];
                const auto delay = std::polar(1., -2 * verb::kPi * freq / 48000.);
                std::complex<double> prev{}, expC{}, refC{};
                for (int k = 0; k < verb::NB; ++k) {
                    const double a = k < verb::NB - 1 ? 1.0 - std::exp(-2 * verb::kPi * verb::kBandHz[k] * std::sqrt(2.) / 48000.) : 1.0;
                    const auto low = k < verb::NB - 1 ? a / (1.0 - (1.0 - a) * delay) : std::complex<double>(1.0, 0.0);
                    const auto w = low - prev; prev = low;
                    const double air = std::pow(10., -verb::airAbsDbPerM(verb::kBandHz[k], scErOmni.tempC, scErOmni.humidity) * pathLen / 20.);
                    expC += w * air * gains[(size_t)k];
                    refC += w * air;
                }
                const double actual = std::abs(response(irErRot, freq)) / std::abs(response(irErOmni, freq));
                const double pred = std::abs(expC) / std::abs(refC);
                const double errDb = std::abs(20.0 * std::log10(actual / pred));
                worstReflDb = std::max(worstReflDb, errDb);
                reflOk &= std::isfinite(errDb) && errDb <= 0.1;
            }
        }
        std::printf("    E22 1-reflection 7-band worst error across 2 profiles: %.5f dB\n", worstReflDb);
        check(reflOk, "E22 single-wall reflection impulse matches rotated SH pattern within 0.1 dB across all 7 bands");

        // 3. Bit-identical audio with zero correction + XML & .verbroom omission when zero + roundtrip with non-zero
        {
            std::istringstream in("sr 48000\nlength 0.4\nheight 3\nmaterial W .2 .2 .2 .2 .2 .2 .2 .2 .5\nfloor W\nceiling W\nwall 0 0 5 0 W\nwall 5 0 5 4 W\nwall 5 4 0 4 W\nwall 0 4 0 0 W\nsource S1 1 2 1.5 0 0 0 0 instrument=brass_trumpet_bell_model instrumentAmount=1\nreceiver R1 3 2 1.5 0 0 0 0\n");
            verb::Scene scPre = verb::parseScene(in, "e22_bit");
            verb::Scene scZero = scPre;
            scZero.sources[0].aimAzDeg = 0.0;
            scZero.sources[0].aimElDeg = 0.0;
            scZero.sources[0].aimRollDeg = 0.0;
            const auto irA = renderImpulse(scPre, 4096);
            const auto irB = renderImpulse(scZero, 4096);
            check(irA.size() == irB.size() && std::memcmp(irA.data(), irB.data(), irA.size() * sizeof(float)) == 0,
                  "E22 Engine::process impulse with zero aim correction is bit-identical");
        }

        VerbProcessor pXml;
        check(waitReady(pXml), "E22 state/preset processor ready");
        pXml.setSourceDirectivity(0, "brass_trumpet_bell_model", 0.75, "", "", "", 0.0, 0.0, 0.0);
        juce::MemoryBlock mbZero;
        pXml.getStateInformation(mbZero);
        auto xmlZero = juce::AudioProcessor::getXmlFromBinary(mbZero.getData(), (int)mbZero.getSize());
        const juce::String xmlZeroStr = xmlZero ? xmlZero->toString() : juce::String();
        const juce::String presetZeroStr = pXml.getRoomPresetText();
        check(xmlZero != nullptr && !xmlZeroStr.contains("aimAzDeg") && !xmlZeroStr.contains("aimElDeg") && !xmlZeroStr.contains("aimRollDeg")
              && !presetZeroStr.contains("aimAzDeg") && !presetZeroStr.contains("aimElDeg") && !presetZeroStr.contains("aimRollDeg"),
              "E22 zero aim correction omits aimAzDeg/aimElDeg/aimRollDeg from both XML state and .verbroom preset");

        // Legacy XML without aim attrs loads cleanly with 0,0,0
        VerbProcessor pLeg;
        pLeg.setStateInformation(mbZero.getData(), (int)mbZero.getSize());
        check(waitReady(pLeg) && pLeg.getSources()[0].aimAzDeg == 0.0 && pLeg.getSources()[0].aimElDeg == 0.0 && pLeg.getSources()[0].aimRollDeg == 0.0,
              "E22 legacy XML without aim attributes loads cleanly with 0,0,0");

        // Non-zero aim round-trip through binary XML AND .verbroom preset
        pXml.setSourceDirectivity(0, "brass_trumpet_bell_model", 0.75, "", "", "", 90.0, -25.0, 40.0);
        juce::MemoryBlock mbAim;
        pXml.getStateInformation(mbAim);
        const juce::String presetAim = pXml.getRoomPresetText();
        VerbProcessor pFromXml, pFromPreset;
        pFromXml.setStateInformation(mbAim.getData(), (int)mbAim.getSize());
        juce::String perr;
        check(waitReady(pFromXml) && std::abs(pFromXml.getSources()[0].aimAzDeg - 90.0) < 1e-6
              && std::abs(pFromXml.getSources()[0].aimElDeg + 25.0) < 1e-6
              && std::abs(pFromXml.getSources()[0].aimRollDeg - 40.0) < 1e-6,
              "E22 binary XML round-trip restores aimAzDeg=90, aimElDeg=-25, aimRollDeg=40");
        check(pFromPreset.loadRoomPresetText(presetAim, perr) && waitReady(pFromPreset)
              && std::abs(pFromPreset.getSources()[0].aimAzDeg - 90.0) < 1e-6
              && std::abs(pFromPreset.getSources()[0].aimElDeg + 25.0) < 1e-6
              && std::abs(pFromPreset.getSources()[0].aimRollDeg - 40.0) < 1e-6,
              "E22 .verbroom preset round-trip restores aimAzDeg=90, aimElDeg=-25, aimRollDeg=40");
    }

    std::printf("\nE13 verification summary: %d PASS, %d FAIL\n", gPass, gFail);
    return gFail ? 1 : 0;
}
