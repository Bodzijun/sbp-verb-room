#pragma once
#include "../P9Test.h"

static juce::String p9Evaluate(VerbWebEditor& editor, const juce::String& script) {
    auto result = std::make_shared<std::optional<juce::String>>();
    editor.testEvaluate(script, [result](juce::String value) { *result = std::move(value); });
    p9::waitFor([&] { return result->has_value(); }, 1500);
    return result->value_or("<timeout>");
}

static int runP9WebBake() {
    const auto root = juce::File::getCurrentWorkingDirectory().getChildFile(
        juce::SystemStats::getEnvironmentVariable("VERB_P9C_WEB_PROFILE", "out/P9/web-profile"));
    root.createDirectory();
    settings().udfRootOverride = root.getChildFile("profile");
    settings().prefFileOverride = root.getChildFile("editor.ini");
    const auto attach = [](VerbProcessor& processor) -> std::unique_ptr<p9::Editor> {
        processor.setOutlinerVisible(true);
        juce::String reason;
        auto editor = VerbWebEditor::create(processor, reason);
        if (!editor) { std::printf("P9_WEB create failed: %s\n", reason.toRawUTF8()); return {}; }
        editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);
        editor->setVisible(true);
        if (!p9::waitFor([&] { return editor->isReady(); }, 15000)) {
            std::printf("P9_WEB handshake failed: %s\n", editor->getFailureReason().toRawUTF8());
            return {};
        }
        const auto selectListener = R"js((() => {
            const edit = [...document.querySelectorAll('.header-actions button')].find(b => b.textContent.includes('Edit room'));
            edit?.click();
            const listener = [...document.querySelectorAll('.outliner-row .object-name')].find(b => b.textContent === 'Listener');
            listener?.click();
            return document.querySelector('[data-bake-path="32"]')?.textContent || 'missing';
        })())js";
        juce::String button;
        if (!p9::waitFor([&] { button = p9Evaluate(*editor, selectListener); return button == "Bake path"; }, 10000)) {
            std::printf("P9_WEB listener bake button unavailable: %s\n", button.toRawUTF8());
            return {};
        }
        return editor;
    };
    const auto trigger = [](p9::Editor& base) {
        auto& editor = static_cast<VerbWebEditor&>(base);
        return p9Evaluate(editor, R"js((() => {
            const button = document.querySelector('[data-bake-path="32"]');
            if (!button || button.disabled) return 'missing';
            button.click();
            return 'clicked';
        })())js") == "clicked";
    };
    const auto observe = [](p9::Editor& base) {
        auto& editor = static_cast<VerbWebEditor&>(base);
        const auto start = p9::Clock::now();
        juce::String footer;
        const bool seen = p9::waitFor([&] {
            footer = p9Evaluate(editor, "document.querySelector('.footer-bake-text')?.textContent || ''");
            return footer.containsIgnoreCase(" baked");
        }, 3000);
        const auto stats = editor.getStats();
        std::printf("P9_WEB visible=%d observedAfterProcessorMs=%.1f timerGapMs=%.1f bridgeTickMs=%.1f pings=%d footer=%s\n",
            (int)seen, p9::msSince(start), stats.maxTimerGapMs, stats.maxBridgeTickMs,
            stats.pingCalls, footer.toRawUTF8());
        return seen;
    };
    return p9::run(attach, trigger, observe);
}
