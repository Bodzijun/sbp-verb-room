// Only the isolated test process owns this parent. Count real posted host messages.
struct LiveKeyParent {
    HWND hwnd=nullptr;
    struct Key { UINT message; WPARAM vk; };
    std::vector<Key> keys;
    static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){
        if(m==WM_KEYDOWN||m==WM_KEYUP||m==WM_SYSKEYDOWN||m==WM_SYSKEYUP){
            if(auto* self=reinterpret_cast<LiveKeyParent*>(GetWindowLongPtrW(h,GWLP_USERDATA)))self->keys.push_back({m,w});
            return 0;
        }return DefWindowProcW(h,m,w,l);
    }
    LiveKeyParent(){WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"VerbT9KeyParent";RegisterClassW(&wc);hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"Verb owned live test host",WS_OVERLAPPEDWINDOW,20,20,1340,900,nullptr,nullptr,wc.hInstance,nullptr);SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)this);ShowWindow(hwnd,SW_SHOWNOACTIVATE);}
    ~LiveKeyParent(){if(hwnd)DestroyWindow(hwnd);}
};
// Opt-in test host only. CDP drives the actual embedded WebView2; this endpoint
// exposes processor observations, not frontend mutation shortcuts.
static int runLivePointerHarness(VerbProcessor& p) {
    const auto original = settings();
    const auto root = juce::File::getCurrentWorkingDirectory().getChildFile("out/T6/live");
    root.createDirectory();
    settings().prefFileOverride = root.getChildFile("editor.ini");
    settings().udfRootOverride = root.getChildFile("profile");
    settings().prefFileOverride.getSiblingFile("web-preferences.json").replaceWithText("{}");
    std::map<juce::String, std::unique_ptr<HostListener>> listeners;
    for (auto* raw : p.getParameters()) if (auto* param = dynamic_cast<juce::RangedAudioParameter*>(raw)) {
        auto l = std::make_unique<HostListener>(); param->addListener(l.get()); listeners[param->paramID] = std::move(l);
    }
    juce::String reason;
    auto editor = VerbWebEditor::create(p, reason);
    if (!editor) { std::printf("T6 host failed: %s\n", reason.toRawUTF8()); return 1; }
    LiveKeyParent parent;
    editor->setSize(1280, 800); editor->addToDesktop(0,parent.hwnd); editor->setName("Verb T6 live pointer host"); editor->setVisible(true);
    EditorBridge observer(p, [](const juce::String&, const juce::var&){});
    const auto request = root.getChildFile("request.json"), response = root.getChildFile("response.json");
    std::printf("T6 host PID %lu; isolated WebView2 ready for CDP\n", GetCurrentProcessId());
    juce::AudioBuffer<float> block(2,256); juce::MidiBuffer midi;
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 7200000;
    bool processing=true;juce::MemoryBlock savedProject;
    bool quit = false, steadySignal=false; double signalAmp=.1, audioEnergy = 0; int n = 0;
    while (!quit && juce::Time::getMillisecondCounterHiRes() < deadline) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(6);
        // Quiet periodic test signal exercises processBlock's effective settings.
        block.clear(); if (++n % 200 == 0 || steadySignal) block.setSample(0,0,(float)signalAmp); if(processing)p.processBlock(block,midi);
        audioEnergy = block.getRMSLevel(0,0,256);
        if (!request.existsAsFile()) continue;
        const auto r = juce::JSON::parse(request); request.deleteFile();
        const auto op = prop(r,"op").toString();
        if(op.startsWith("meter")) { steadySignal=true; signalAmp=op=="meterSilence"?0:op=="meterQuiet"?.003:op=="meterMedium"?.1:1.; }
        if (op == "quit") quit = true;
        if(op=="pauseAudio")processing=false;
        if(op=="resumeAudio")processing=true;
        if(op=="saveProject")p.getStateInformation(savedProject);
        if(op=="restoreProject"&&savedProject.getSize())p.setStateInformation(savedProject.getData(),(int)savedProject.getSize());
        if(op=="rate44100")p.prepareToPlay(44100,256);
        if(op=="rate48000")p.prepareToPlay(48000,256);
        if(op=="block512")p.prepareToPlay(48000,512);
        if(op=="cpuLoad")p.setTestCpuLoadOverride((float)prop(r,"ratio"));
        if(op=="resetKeys")parent.keys.clear();
        // N2: the real classic editor inside the shell. "nativeNewUi" presses its New UI button (a JUCE component, so the hook triggers the click handler).
        if (op == "nativeNewUi") { if (auto* classic = dynamic_cast<VerbEditor*>(editor->getNativeEditor())) classic->testClickNewUi(); }
        if (op == "resetCounts") for (auto& l : listeners) l.second->begins = l.second->changes = l.second->ends = 0;
        if (op == "externalRevision") p.bumpRevision();
        bool roundtrip = true; double impulseEnergy = 0, impulsePeak = 0;
        if (op == "reload") { juce::String why; const auto preset=p.getRoomPresetText(); root.getChildFile("roundtrip.verbroom").replaceWithText(preset); roundtrip=p.loadRoomPresetText(preset,why)&&ready(p); }
        if (op == "impulse" || op == "reload") {
            for (int i=0;i<400;++i) { block.clear(); if(i==0) block.setSample(0,0,1); p.processBlock(block,midi); impulsePeak=std::max(impulsePeak,(double)block.getMagnitude(0,256)); for(int c=0;c<2;++c) for(int j=0;j<256;++j) impulseEnergy+=std::pow(block.getSample(c,j),2); }
        }
        auto counts = obj({}); for(const auto& l:listeners) counts.getDynamicObject()->setProperty(l.first,obj({{"begin",l.second->begins},{"change",l.second->changes},{"end",l.second->ends}}));
        auto diagnostics=obj({}); if(const auto d=p.getWebTailDiagnostics()) diagnostics=obj({{"rt60",numbers(d->rt60)},{"physicalRT60",numbers(d->physicalRT60)},{"effectiveMultipliers",numbers(d->effectiveMultipliers)},{"multipliers",numbers(d->multipliers)},{"reshapeStatus",numbers(d->reshapeStatus)},{"baseRays",d->baseRays},{"adaptive",d->adaptive},{"engineTailCurrent",p.isTailCurrent()},{"webSceneCurrent",p.isWebSceneCurrent()},{"webTailCurrent",p.isWebTailCurrent()}});
        const auto timing=editor->getStats();
        auto editorInfo = obj({});
        {
            auto* classic = dynamic_cast<VerbEditor*>(editor->getNativeEditor()); DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
            editorInfo = obj({{"timerGapMs",timing.maxTimerGapMs},{"bridgeTickMs",timing.maxBridgeTickMs},{"heartbeatGapMs",timing.maxHeartbeatGapMs},{"pings",timing.pingCalls},{"mode",(int)editor->getMode()},{"ready",editor->isReady()},{"classic",classic != nullptr},{"newUiButton",classic != nullptr && classic->newUiButtonShown()},{"newUiNote",classic != nullptr ? classic->newUiNoteText() : juce::String()},
                {"handles",(int)handles},{"user",(int)GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)},{"gdi",(int)GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)},{"preferredUi",preferredUi() == UiChoice::web ? "web" : "native"}});
        }
        auto hostKeys=array({});for(const auto& key:parent.keys)hostKeys.getArray()->add(obj({{"message",(int)key.message},{"vk",(int)key.vk}}));
        auto data = obj({{"processing",processing},{"hostKeys",hostKeys},{"id",prop(r,"id")},{"snapshot",observer.snapshot()},{"diagnostics",diagnostics},{"counts",counts},{"transactionOpen",p.getEditorSession().transactionOpen()},{"audioRms",audioEnergy},{"impulseEnergy",impulseEnergy},{"impulsePeak",impulsePeak},{"roundtrip",roundtrip},{"editor",editorInfo}});
        response.replaceWithText(juce::JSON::toString(data));
    }
    editor.reset(); for(auto* raw:p.getParameters()) if(auto* param=dynamic_cast<juce::RangedAudioParameter*>(raw)) param->removeListener(listeners[param->paramID].get());
    settings()=original; return 0;
}
