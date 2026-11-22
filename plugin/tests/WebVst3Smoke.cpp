// Optional smoke test (target VerbWebVst3Smoke, not part of CTest): loads the BUILT VST3 binary as a host would, with
// VERB_UI=web, opens its editor and looks at the pixels of the editor window until the shell page shows its "ready"
// colours (dark page background + the emerald status line). It proves that the shipped binary (static WebView2 loader,
// embedded resources, factory) really opens the web editor. Usage: VerbWebVst3Smoke "<path to Verb Room.vst3>" [web|native]
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <windows.h>

#include <cstdio>
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
