// Optional smoke test (target VerbWebVst3Smoke, not part of CTest): loads the BUILT VST3 binary as a host would, with
// VERB_UI=web, opens its editor and looks at the pixels of the editor window until the shell page shows its "ready"
// colours (dark page background + the emerald status line). It proves that the shipped binary (static WebView2 loader,
// embedded resources, factory) really opens the web editor. Usage: VerbWebVst3Smoke "<path to Verb Room.vst3>" [web|native]
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstmessage.h>

#include <windows.h>

#include <cstdio>
#include <cmath>
#include <thread>
#include <vector>

struct Shot { int w = 0, h = 0; std::vector<unsigned char> bgra; };

static bool grab(HWND hwnd, Shot& out) {
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return false;
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.w = w; out.h = h;
    out.bgra.assign((size_t)w * h * 4, 0);
    GetDIBits(mem, bmp, 0, (UINT)h, out.bgra.data(), &bi, DIB_RGB_COLORS);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return true;
}

static bool closeTo(const unsigned char* p, int r, int g, int b, int tol) {
    return std::abs(p[2] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[0] - b) <= tol;
}

static bool queryRawVst3ParameterInfo(const juce::String& vst3BundlePath,
                                        int paramIndex,
                                        Steinberg::Vst::ParameterInfo& outInfo,
                                        bool& outMatchedCount) {
    outMatchedCount = false;
    const juce::File root(vst3BundlePath);
    const juce::File dllFile = root.isDirectory()
        ? root.getChildFile("Contents/x86_64-win/Verb Room.vst3")
        : root;
    HMODULE mod = LoadLibraryW(dllFile.getFullPathName().toWideCharPointer());
    if (!mod) return false;
    using GetFactoryFn = Steinberg::IPluginFactory* (PLUGIN_API*)();
    auto getFactory = reinterpret_cast<GetFactoryFn>(GetProcAddress(mod, "GetPluginFactory"));
    bool ok = false;
    if (getFactory) {
        if (auto* factory = getFactory()) {
            const Steinberg::int32 nClasses = factory->countClasses();
            Steinberg::TUID compCid{};
            bool foundComp = false;
            for (Steinberg::int32 i = 0; i < nClasses; ++i) {
                Steinberg::PClassInfo ci{};
                if (factory->getClassInfo(i, &ci) == Steinberg::kResultOk
                    && std::strcmp(ci.category, "Audio Module Class") == 0) {
                    std::memcpy(compCid, ci.cid, sizeof(compCid));
                    foundComp = true;
                    break;
                }
            }
            Steinberg::Vst::IComponent* comp = nullptr;
            if (foundComp && factory->createInstance(compCid, Steinberg::Vst::IComponent::iid, (void**)&comp) == Steinberg::kResultOk && comp) {
                comp->initialize(nullptr);
                Steinberg::TUID ctrlCid{};
                Steinberg::Vst::IEditController* ctrl = nullptr;
                if (comp->getControllerClassId(ctrlCid) == Steinberg::kResultOk
                    && factory->createInstance(ctrlCid, Steinberg::Vst::IEditController::iid, (void**)&ctrl) == Steinberg::kResultOk && ctrl) {
                    ctrl->initialize(nullptr);
                    Steinberg::Vst::IConnectionPoint* cpComp = nullptr;
                    Steinberg::Vst::IConnectionPoint* cpCtrl = nullptr;
                    comp->queryInterface(Steinberg::Vst::IConnectionPoint::iid, (void**)&cpComp);
                    ctrl->queryInterface(Steinberg::Vst::IConnectionPoint::iid, (void**)&cpCtrl);
                    if (cpComp && cpCtrl) {
                        cpComp->connect(cpCtrl);
                        cpCtrl->connect(cpComp);
                    }
                    const Steinberg::int32 count = ctrl->getParameterCount();
                    outMatchedCount = (count == 300);
                    if (paramIndex >= 0 && paramIndex < count
                        && ctrl->getParameterInfo(paramIndex, outInfo) == Steinberg::kResultOk) {
                        ok = true;
                    }
                    if (cpCtrl && cpComp) {
                        cpCtrl->disconnect(cpComp);
                        cpComp->disconnect(cpCtrl);
                    }
                    if (cpCtrl) cpCtrl->release();
                    if (cpComp) cpComp->release();
                    ctrl->terminate();
                    ctrl->release();
                }
                comp->terminate();
                comp->release();
            }
            factory->release();
        }
    }
    FreeLibrary(mod);
    return ok;
}

static int runG8HostTest(juce::AudioPluginInstance& inst, const juce::String& vst3Path) {
    int passes = 0, fails = 0;
    auto check = [&](bool ok, const char* msg) {
        if (ok) ++passes;
        else { ++fails; std::printf("FAIL [G9 VST3]: %s\n", msg); }
    };

    const auto& params = inst.getParameters();
    check(params.size() == 300, "VST3 host sees 283 + 16 Mod Out + 1 VST3 Bypass = 300 parameters");
    auto* p0 = params.size() > 0 ? params[0] : nullptr;
    auto* pMakeup = params.size() > 249 ? params[249] : nullptr;
    auto* pLpath = params.size() > 282 ? params[282] : nullptr;
    auto* pBypass = params.size() > 299 ? params[299] : nullptr;
    check(p0 && p0->getName(64).contains("Source 1")
          && pMakeup && pMakeup->getName(64) == "Makeup"
          && pLpath && pLpath->getName(64).contains("Path")
          && pBypass && pBypass->getName(64) == "Bypass",
          "VST3 first 283 parameter slots preserve legacy positions and VST3 Bypass sits at 299");

    bool outParamsOk = params.size() == 300;
    for (int s = 0; s < 16 && outParamsOk; ++s) {
        auto* prm = params[283 + s];
        if (!prm || prm->getName(64) != "Mod Out " + juce::String(s + 1)
            || !prm->isAutomatable()
            || prm->getCategory() != juce::AudioProcessorParameter::genericParameter
            || std::abs(prm->getDefaultValue() - 0.5f) > 1e-6f
            || std::abs(prm->getValue() - 0.5f) > 1e-6f) {
            outParamsOk = false;
        }
    }
    check(outParamsOk, "VST3 host sees Mod Out 1..16 at indices 283..298 as automatable genericParameter (neutral 0.5)");

    Steinberg::Vst::ParameterInfo mo1Info{};
    bool rawCountOk = false;
    const bool gotRawInfo = queryRawVst3ParameterInfo(vst3Path, 283, mo1Info, rawCountOk);
    if (gotRawInfo) {
        const juce::String mo1Title(juce::CharPointer_UTF16(reinterpret_cast<const juce::CharPointer_UTF16::CharType*>(mo1Info.title)));
        const juce::String mo1Short(juce::CharPointer_UTF16(reinterpret_cast<const juce::CharPointer_UTF16::CharType*>(mo1Info.shortTitle)));
        const juce::String mo1Units(juce::CharPointer_UTF16(reinterpret_cast<const juce::CharPointer_UTF16::CharType*>(mo1Info.units)));
        const bool canAuto = (mo1Info.flags & Steinberg::Vst::ParameterInfo::kCanAutomate) != 0;
        const bool isRO = (mo1Info.flags & Steinberg::Vst::ParameterInfo::kIsReadOnly) != 0;
        const bool isHidden = (mo1Info.flags & Steinberg::Vst::ParameterInfo::kIsHidden) != 0;
        const bool isBypass = (mo1Info.flags & Steinberg::Vst::ParameterInfo::kIsBypass) != 0;
        std::printf("VST3 ParameterInfo[283] id=0x%08x title=\"%s\" shortTitle=\"%s\" units=\"%s\" stepCount=%d default=%.3f flags=0x%x (kCanAutomate=%d kIsReadOnly=%d kIsHidden=%d kIsBypass=%d)\n",
                    (unsigned)mo1Info.id, mo1Title.toRawUTF8(), mo1Short.toRawUTF8(), mo1Units.toRawUTF8(),
                    (int)mo1Info.stepCount, mo1Info.defaultNormalizedValue, (unsigned)mo1Info.flags,
                    (int)canAuto, (int)isRO, (int)isHidden, (int)isBypass);
        check(rawCountOk && mo1Title == "Mod Out 1"
              && mo1Info.flags == Steinberg::Vst::ParameterInfo::kCanAutomate
              && canAuto && !isRO && !isHidden && !isBypass
              && std::abs(mo1Info.defaultNormalizedValue - 0.5) < 1e-6,
              "Raw VST3 IEditController::getParameterInfo reports mo1 flags == kCanAutomate (no kIsReadOnly/kIsHidden/kIsBypass)");
    } else {
        check(false, "Raw VST3 IEditController::getParameterInfo readable for mo1");
    }

    // Configure Creative mode + ModTarget::None route to hostOut=1 on driver X [0, 10] via VST3 state XML (<VST3PluginState><IComponent>)
    juce::MemoryBlock stateMb;
    inst.getStateInformation(stateMb);
    auto outerXml = juce::AudioProcessor::getXmlFromBinary(stateMb.getData(), (int)stateMb.getSize());
    auto* icompEl = outerXml ? outerXml->getChildByName("IComponent") : nullptr;
    juce::MemoryBlock innerMb;
    const bool hasInner = icompEl && innerMb.fromBase64Encoding(icompEl->getAllSubText());
    auto xml = hasInner ? juce::AudioProcessor::getXmlFromBinary(innerMb.getData(), (int)innerMb.getSize()) : nullptr;
    check(xml != nullptr && xml->getTagName() == "VerbState", "VST3 inner <IComponent> XML <VerbState> readable");
    if (xml) {
        for (auto* ch : xml->getChildIterator()) {
            const auto id = ch->getStringAttribute("id");
            if (id == "mode") ch->setAttribute("value", 1.0);
            if (id == "s1x") ch->setAttribute("value", 2.0);
        }
        while (auto* oldMod = xml->getChildByName("Modulation"))
            xml->removeChildElement(oldMod, true);
        auto* modEl = new juce::XmlElement("Modulation");
        auto* rtEl = new juce::XmlElement("Route");
        rtEl->setAttribute("enabled", 1);
        rtEl->setAttribute("object", 0);
        rtEl->setAttribute("driver", "x");
        rtEl->setAttribute("targetObject", -1);
        rtEl->setAttribute("target", "none");
        rtEl->setAttribute("hostOut", 1);
        rtEl->setAttribute("attackMs", 0.0);
        rtEl->setAttribute("releaseMs", 0.0);
        auto* cvEl = new juce::XmlElement("Curve");
        cvEl->setAttribute("inputMin", 0.0);
        cvEl->setAttribute("inputMax", 10.0);
        cvEl->setAttribute("outputMin", 0.0);
        cvEl->setAttribute("outputMax", 1.0);
        cvEl->setAttribute("invert", 0);
        cvEl->setAttribute("clamp", 1);
        auto* n0 = new juce::XmlElement("Node"); n0->setAttribute("x", 0.0); n0->setAttribute("y", 0.0);
        auto* n1 = new juce::XmlElement("Node"); n1->setAttribute("x", 1.0); n1->setAttribute("y", 1.0);
        cvEl->addChildElement(n0); cvEl->addChildElement(n1);
        rtEl->addChildElement(cvEl);
        modEl->addChildElement(rtEl);
        xml->addChildElement(modEl);

        juce::MemoryBlock modifiedInner;
        juce::AudioProcessor::copyXmlToBinary(*xml, modifiedInner);
        icompEl->deleteAllTextElements();
        icompEl->addTextElement(modifiedInner.toBase64Encoding());
        while (auto* ec = outerXml->getChildByName("IEditController"))
            outerXml->removeChildElement(ec, true);

        juce::MemoryBlock nextMb;
        juce::AudioProcessor::copyXmlToBinary(*outerXml, nextMb);
        inst.setStateInformation(nextMb.getData(), (int)nextMb.getSize());
    }

    // Pump message loop briefly so the loaded scene builds asynchronously before audio processing
    for (int i = 0; i < 40; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(25);

    struct VstHostListener final : juce::AudioProcessorParameter::Listener {
        std::atomic<int> count{0};
        std::atomic<float> last{-1.f};
        void parameterValueChanged(int, float v) override { last.store(v); count.fetch_add(1); }
        void parameterGestureChanged(int, bool) override {}
    } hostLst;

    auto* mo1 = params[283];
    auto* pS1x = params[0];
    auto* pMode = [&]() -> juce::AudioProcessorParameter* {
        for (auto* prm : params)
            if (prm && prm->getName(64) == "Mode") return prm;
        return nullptr;
    }();
    mo1->addListener(&hostLst);

    // Run audio processing on a real non-message audio thread so JUCE's VST3 wrapper routes changes via data.outputParameterChanges!
    auto runAudioBlocks = [&](int blocks) {
        std::thread audioThread([&] {
            juce::AudioBuffer<float> buf(std::max(2, inst.getTotalNumOutputChannels()), 256);
            juce::MidiBuffer midi;
            for (int b = 0; b < blocks; ++b) {
                buf.clear();
                inst.processBlock(buf, midi);
            }
        });
        audioThread.join();
    };

    runAudioBlocks(70); // ~373ms at 256@48kHz so creativeBlend settles cleanly to 1.0
    const float moAtX2 = mo1->getValue();
    check(std::abs(moAtX2 - 0.2f) < 0.02f && hostLst.count.load() > 0,
          "VST3 outputParameterChanges updates host Mod Out 1 to ~0.2 for s1x=2 on audio thread");

    // Move s1x via host parameter to x=8.0 (query exact normalized mapping via parameter text/parser)
    const float normX8 = std::clamp(pS1x->getValueForText("8.0"), 0.0f, 1.0f);
    pS1x->setValueNotifyingHost(normX8);
    runAudioBlocks(12);
    const float moAtX8 = mo1->getValue();
    check(std::abs(moAtX8 - 0.8f) < 0.02f, "VST3 outputParameterChanges tracks s1x=8.0 -> Mod Out 1 ~ 0.8 on host");

    // Host echo write via inputParameterChanges is ignored by plugin without corrupting output or causing feedback
    mo1->setValueNotifyingHost(0.11f);
    runAudioBlocks(4);
    const float normX9 = std::clamp(pS1x->getValueForText("9.0"), 0.0f, 1.0f);
    pS1x->setValueNotifyingHost(normX9);
    runAudioBlocks(12);
    check(std::abs(mo1->getValue() - 0.9f) < 0.02f, "VST3 Mod Out stays immune to host echo writes and continues tracking 0.9");

    // Switch back to Physical -> host sees smooth settle back to 0.5 (~373ms)
    check(pMode != nullptr, "VST3 host finds Mode parameter");
    if (pMode) {
        pMode->setValueNotifyingHost(0.0f);
        runAudioBlocks(80);
        check(std::abs(mo1->getValue() - 0.5f) < 1e-4f, "VST3 Physical mode returns host Mod Out 1 to 0.5 neutral");
    }

    mo1->removeListener(&hostLst);
    std::printf("FINAL G8 VST3 host smoke: %d pass, %d fail (mo@2=%.4f, mo@8=%.4f)\n",
                passes, fails, moAtX2, moAtX8);
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String path = argc > 1 ? argv[1] : "build-msvc/plugin/VerbPlugin_artefacts/Release/VST3/Verb Room.vst3";
    const juce::String mode = argc > 2 ? argv[2] : "web";
    SetEnvironmentVariableA("VERB_UI", mode.toRawUTF8());

    juce::AudioPluginFormatManager fm;
    fm.addFormat(new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> descs;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile(descs, juce::File(path).getFullPathName());
    if (descs.isEmpty()) { std::printf("FAIL: no plugin description found in %s\n", path.toRawUTF8()); return 2; }
    juce::String err;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    auto inst = fm.createPluginInstance(*descs[0], 48000.0, 256, err);
    if (inst == nullptr) { std::printf("FAIL: cannot instantiate: %s\n", err.toRawUTF8()); return 2; }
    std::printf("loaded %s in %d ms (UI mode requested: %s)\n", inst->getName().toRawUTF8(), (int)(juce::Time::getMillisecondCounterHiRes() - t0), mode.toRawUTF8());
    inst->prepareToPlay(48000.0, 256);

    if (mode == "g8-host") {
        const int rc = runG8HostTest(*inst, path);
        inst.reset();
        return rc;
    }

    std::unique_ptr<juce::AudioProcessorEditor> ed(inst->createEditorIfNeeded());
    if (ed == nullptr) { std::printf("FAIL: no editor\n"); return 2; }
    std::printf("editor size %dx%d\n", ed->getWidth(), ed->getHeight());
    ed->setTopLeftPosition(40, 40);
    ed->addToDesktop(juce::ComponentPeer::windowIsTemporary);
    ed->setVisible(true);

    const double t1 = juce::Time::getMillisecondCounterHiRes();
    bool seen = false;
    double bgFrac = 0, accent = 0;
    while (juce::Time::getMillisecondCounterHiRes() - t1 < 20000) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(250);
        Shot s;
        if (!grab((HWND)ed->getWindowHandle(), s)) continue;
        size_t bg = 0, ac = 0, n = (size_t)s.w * s.h;
        for (size_t i = 0; i < n; ++i) {
            const unsigned char* p = &s.bgra[i * 4];
            if (closeTo(p, 0x10, 0x15, 0x13, 6)) ++bg;
            else if (closeTo(p, 0x2f, 0xd4, 0x9a, 14)) ++ac;
        }
        bgFrac = (double)bg / (double)n;
        accent = (double)ac;
        if (bgFrac > 0.55 && ac >= 60) { seen = true; break; }
    }
    const int ms = (int)(juce::Time::getMillisecondCounterHiRes() - t1);
    std::printf("window pixels: page background %.1f %%, accent-coloured pixels %d (after %d ms)\n", bgFrac * 100.0, (int)accent, ms);
    std::printf("%s: the editor window %s the web shell's 'ready' look\n", seen ? "WEB-SHELL-SEEN" : "WEB-SHELL-NOT-SEEN", seen ? "shows" : "does not show");

    ed.reset();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
    inst.reset();
    return seen == (mode == "web") ? 0 : 1;
}
