Бодя, перенесення здійсненне на JUCE 8.0.15. Рекомендую локальний WebView для Windows/macOS, збережений native редактор і окремий експериментальний WebView-варіант Linux. Спочатку переносимо наявні функції; нові акустичні можливості з макета не повинні блокувати міграцію.

# План перенесення UI Verb Room у WebView

Дата дослідження: **10.11.2026**. Це план, без реалізації, збірки, встановлення або Git-операцій. Єдиний створений файл — цей документ; `publish/sbp-verb-room` не використовувався. Шляхи нижче відносні до `E:\Claude\verb`.

## 1. Вихідний стан і межі достовірності

- Прочитано `AGENTS.md`, інструкції сцен/пресетів, оголошення й реалізацію редактора/процесора, `SourceModel`, `SceneObjects`, `SceneOutliner`, `SourceMixerView`, `TailDecayView`, `EnergyMapView`, `RoomLibrary`, `PluginLookAndFeel`, релевантні тести `PluginTest.cpp`, CMake, workflow, `ci/` та документацію пакування.
- Дизайн-база — **макет v6**, `ui-mockup/index.html` (189 982 байти на момент читання), актуальне доповнення REPORT та `POLISH_TODO.uk.md`; перевірено збережені кадри Main Physical/Creative, Energy, Sources, Listener, Mix, Output, Edit source/outliner/3D. Історичний опис v4 не замінює актуальний HTML: Input mode вже прибрано. Макет містить демонстраційні сцени, розрахунки та browser Save; це не дані процесора.
- `third_party/JUCE/CMakeLists.txt` і `juce_StandardHeader.h` підтверджують **8.0.15**, C++17. Сам JUCE вимагає CMake ≥3.22, хоча корінь проєкту оголошує 3.16.
- **Паралельна робота:** під час читання вже існували `mode`, `absorption`, `decayLow/Mid/High`, RT60 solver, `startTargetRT60` та `early` default 0 dB. Їхня наявність — спостереження робочого дерева, не підтвердження завершення/тестування. Перед реалізацією повторно звірити `makeLayout`, Physical semantics, RT60 API, `TailDiagnostics`, міграції старого стану та `Stage2Test.h`; не редагувати `src/`/`plugin/` одночасно з їхнім власником.
- Поточний процесор створює `VerbEditor`; `plugin/CMakeLists.txt` вимикає `JUCE_WEB_BROWSER=0` для plugin/test/stress. APVTS створено без `UndoManager`; undo редактора — локальний `vector<Scene>` до 60 знімків, не повний undo параметрів/маршрутів. Native UI оновлюється таймером 10 Hz.
- Є один workflow `.github/workflows/vst3.yml`: **macos-15 Universal + ubuntu-24.04**, Windows job зараз відсутній. Linux «portable» означає відсутність вимоги AVX2/FMA, а не незалежність від ОС: пакет орієнтований на glibc ≥2.39. Додавання WebKit — окреме розширення залежностей.

## 2. Платформи, хости та рішення про fallback

| Платформа | Перевірено в vendored JUCE | Рішення та критерій допуску |
|---|---|---|
| Windows x64 | `Options::Backend::webview2`; default backend — IE. `areOptionsSupported(options)` пробує створення WebView2 environment. `WinWebView2::withUserDataFolder`, `withDLLLocation` існують. | Явно запитувати WebView2; при невдачі відкривати native редактор, не допускати мовчазного IE fallback. Static loader рекомендований; наявність runtime перевіряти окремо від SDK/loader. Тестувати без runtime, із недоступним UDF та без інтернету. |
| macOS arm64/x86_64 | `WKWebViewImpl`, системний WebKit; `Options::Backend::defaultBackend`. `areOptionsSupported` перевіряє backend enum, а не працездатність сторінки. | Без окремого browser runtime в пакеті. Залишити deployment target 11.0, перевірити JS/CSS на найстарішій підтриманій WebKit, обидві архітектури, Retina, lifecycle. Native перемикач лишається до host QA. |
| Linux x86_64 | WebKitGTK 4.1 або 4.0 + GTK3/X11; CMake `NEEDS_WEB_BROWSER TRUE` додає include/options через **NOLINK**, бібліотеки відкриваються динамічно. VST3 потребує embedded subprocess helper. | **Native редактор — типовий і постійний fallback для portable Linux.** WebView спочатку окремим experimental артефактом, `VERB_AVX2=OFF`. WebKit не додаємо до обов'язкових залежностей native пакета. |

### Windows: runtime, UDF, багато екземплярів

Статичний `WebView2LoaderStatic.lib` прибирає потребу в `WebView2Loader.dll`, але **не в WebView2 Runtime**. Встановлений Edge не є достатньою гарантією. Рекомендований Evergreen Runtime; для offline машин — офіційний standalone installer окремо від VST3, перевірка/встановлення на рівні інсталятора, не в audio process. Fixed Version runtime значно збільшує пакет і вимагає власного обслуговування; цей JUCE передає `nullptr` як browser executable folder, а `withDLLLocation` задає лише loader. Fixed Version потребував би додаткової інтеграції, не є простим прапорцем. [Microsoft: розповсюдження WebView2](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution).

Перед створенням browser задати writable локальний UDF, наприклад `%LOCALAPPDATA%/SBP/VerbRoom/WebView/<session>/<instance-token>`. Токен процесора живе до його знищення, повторне відкриття редактора використовує ту саму папку; різні екземпляри й host processes спочатку ізольовані. Не використовувати VST3/DAW directory, спільний корінь temp як одну папку для всіх плагінів або UDF як джерело стану. Унікальні UDF збільшують кількість browser processes/RAM; профілювати 1/8/32 відкриті редактори. Спільний UDF може економити ресурси й не заборонений WebView2, але вводити його лише окремою оптимізацією після тестів ізоляції. Прибирати тільки власні застарілі папки після завершення browser processes, без блокування message thread. [Microsoft: UDF та спільне використання](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/user-data-folder).

### Linux: чому перевірки backend недостатньо

У `juce_WebBrowserComponent_linux.cpp` `areOptionsSupported` повертає true для default backend **без перевірки бібліотек**. Завантаження очікує `libgtk-3.so`, `libglib-2.0.so`, `libwebkit2gtk-4.1.so`, `libjavascriptcoregtk-4.1.so`, `libsoup-3.0.so` або відповідний комплект 4.0/2.4. Це невірсіоновані імена, які можуть бути лише у dev-пакетах. Коли symbols недоступні, `init()`/`goToURL()` нічого не запускають, fallback paint білий: плагін може завантажитися, але UI не працюватиме. Така поведінка також описана у первинному [JUCE issue #1730](https://github.com/juce-framework/JUCE/issues/1730).

Дії: native-only пакет узагалі не компілює browser; experimental WebView пакет має preflight і обов'язковий JS ready-handshake із timeout та native fallback. Для повноцінного Linux WebView-релізу потрібне окреме рішення щодо версійованих soname: upstream fix або маленький документований vendored patch із runtime-only container test. Не вимагати від кінцевого користувача dev-пакети й не підміняти системні symlink інсталятором. Якщо стороння залежність стане прямою link-time залежністю, її відсутність може зламати завантаження самого VST3 до будь-якого fallback: перевіряти `ldd`/ELF NEEDED на фінальному пакеті.

Цей backend примусово обирає **X11** (`gdk_set_allowed_backends("x11")`), використовує XEmbed, вимикає WebKit hardware acceleration. Wayland/XWayland, sandbox DAW і `/tmp` із `noexec` — окремі ризики: JUCE розпаковує executable subprocess helper у temp. Офлайн тут означає відсутність мережі для UI, а не відсутність усіх тимчасових файлів.

### Focus, DPI та відомі host-ризики

- CSS layout працює у logical pixels, canvas backing store — через актуальний `devicePixelRatio` + `ResizeObserver`; не множити JUCE scale вдруге. Тестувати 100/125/150/200%, перенесення між моніторами під час drag, host UI scale, Retina, мінімальний розмір і resize з відкритим dock. Врахувати font fallback: macOS/Linux не мають гарантованого Segoe UI; обрати системний stack або вбудований ліцензований шрифт.
- DOM shortcuts діють лише у відповідному контексті. `Ctrl/Cmd+Z` у text input редагує текст; у canvas викликає C++ undo. Delete/Backspace не видаляє сцену під час введення. Blur, `pointercancel`, `lostpointercapture`, зміна сторінки/редактора завершують host gestures та транзакції.
- WebView може споживати Space/keyboard MIDI й не передавати їх хосту; `setWantsKeyboardFocus(false)` і JS `preventDefault(false)` не гарантують передачу через native child window. Потрібні відтворювані тести REAPER floating/FX-chain/embedded/separate process: введення імен, числа, Tab, Space поза input, Escape, host shortcuts, повернення фокуса після FileChooser. Якщо потрібний native forwarding, реалізувати вузький platform adapter після прототипу, без глобального перехоплення клавіш.
- Є firsthand звіти про перехоплення клавіш у macOS Ableton/Logic та Windows; вони різняться між конфігураціями. Це ризик, **не підтверджена поломка Verb Room у JUCE 8.0.15**. Logic не підтримує VST3; його згадка характеризує backend, не створює вимогу AU. [JUCE: focus issue](https://forum.juce.com/t/fixing-webview-keyboard-focus-issue/63987), [передача клавіш DAW](https://forum.juce.com/t/can-webbrowsercomponent-forward-key-presses-to-the-daw/50729).
- Окремий звіт описує WebKit ProcessThrottler crash у macOS 26.2 / Live 12.3.2 / JUCE 8.0.12; виправлення для нашого дерева не доведене. Тестувати repeated open/close, hide/show, sleep/wake, host restore без активного editor. Native fallback не рятує від падіння всього host process: на проблемній конфігурації browser має не створюватися. [Первинний звіт](https://forum.juce.com/t/webkit-processthrottler-crash-in-juce-8-webview-plugin-macos/68055).

## 3. Архітектура: C++ володіє звуком і документом

### Вбудований frontend і перевірені API

Пропоновані нові частини: `VerbWebEditor`, `EditorBridge`, `EditorSession`/command service, `EnergyAnalysisService`, resource adapter; імена — проєктні пропозиції, не наявні API. Command service та історію утримує процесор, bridge/selection — конкретний editor; перемикання оболонки не губить документ. `VerbProcessor`/APVTS/engine залишаються авторитетними для параметрів, сцен, маршрутів, матеріалів, фізичних оцінок, host state та preset I/O. JS має відображення, navigation/selection і короткочасний optimistic drag; не обробляє аудіо та не підміняє engine макетними формулами.

1. Виробничі HTML/CSS/JS/SVG/шрифти компілюються `juce_add_binary_data` в окремий resource target; browser відкриває `WebBrowserComponent::getResourceProviderRoot()` через `goToURL`.
2. `Options::withNativeIntegrationEnabled()`, `withResourceProvider(...)`, `withNativeFunction(name, callback)`, `withEventListener(id, listener)`, `withInitialisationData(...)`, `withOptionsFrom(relay)` — точні назви цього JUCE. `ResourceProvider` повертає `optional<Resource>`, `Resource` містить `vector<byte> data` і `String mimeType`. Provider отримує `/` для index та шляхи ресурсів. Явний MIME, allowlist шляхів, відсутність довільного доступу до filesystem.
3. `NativeFunction` приймає `const Array<var>&` і `NativeFunctionCompletion`; callback викликається на message thread, completion дозволено викликати з іншого потоку. Коротка команда повертає результат/номер job; тривала робота йде у worker. Відповіді/події browser відправляє message-thread adapter із захистом lifetime.
4. C++ → JS: `emitEventIfBrowserIsVisible`; JS → C++: `getNativeFunction` або `window.__JUCE__.backend.emitEvent`; слухачі JS — `addEventListener`/`removeEventListener`. Ідентифікатори `__juce*` зарезервовані; власні — `verb.*`. Не будувати міст на рядках `evaluateJavascript` із підставленими даними.
5. `getResourceProviderRoot()` у цьому дереві — `https://juce.backend/` на Windows, `juce://juce.backend/` на macOS/Linux; це локальна видача, не HTTP server. JS helper `getBackendResourceAddress(path)` приховує різницю.
6. У release немає localhost, CDN, telemetry чи network dependency; зовнішня navigation/new windows заборонені через callbacks, CSP дозволяє лише потрібні локальні ресурси. `allowedOriginIn` provider залишити порожнім; dev origin дозволяти лише окремим dev flag. CSP протестувати з JUCE injected scripts на кожному backend, не переносити сувору політику навмання.
7. Relays створити перед browser, attachments — після; знищення у зворотному порядку. На close відписати listeners, закрити gestures, зупинити UI timer, анулювати session callbacks. Закриття редактора не знищує процесорний стан або export job.
8. Default JUCE unload при приховуванні може втратити JS state. Спочатку дозволити unload і відновлювати через handshake; `withKeepPageLoadedWhenBrowserIsHidden()` — перевірена, але опційна оптимізація після RAM/lifecycle QA, не обіцяне лікування WebKit crash.

Перевірені джерела API: `juce_gui_extra/misc/juce_WebBrowserComponent.h/.cpp`, `juce_WebControlRelays.h/.cpp`, `juce_WebControlParameterIndexReceiver.h`, `juce_audio_processors/utilities/juce_ParameterAttachments.h/.cpp`, `juce_gui_extra/native/javascript/index.js` і `check_native_interop.js`, `examples/Plugins/WebViewPluginDemo.h`. У прикладі React — вибір прикладу, не вимога JUCE. Для нашого `/juce/index.js` брати саме vendored `native/javascript/index.js` разом із його імпортом, без незалежного оновлення npm frontend пакета.

### 3.1. Автоматизація параметрів

| Тип APVTS | C++ relay + attachment | JS API |
|---|---|---|
| Float: позиції, gain, mix, decay тощо | `WebSliderRelay` + `WebSliderParameterAttachment(RangedAudioParameter&, relay, UndoManager* = nullptr)` | `getSliderState`, `getScaledValue`, `getNormalisedValue`, `setNormalisedValue`, `sliderDragStarted`, `sliderDragEnded`; `valueChangedEvent`/`propertiesChangedEvent` |
| Bool: mute/solo/positioned dry | `WebToggleButtonRelay` + `WebToggleButtonParameterAttachment` | `getToggleState`, `getValue`, `setValue` |
| Choice: mode | `WebComboBoxRelay` + `WebComboBoxParameterAttachment` | `getComboBoxState`, `getChoiceIndex`, `setChoiceIndex` (індекс від 0) |

Зареєструвати стабільні relays за parameter ID для всіх слотів джерел, не перестворювати їх при перемиканні вибору. У прочитаному layout 243 параметри, включно з 5 новими: кількість підтвердити після завершення паралельної роботи. Кілька DOM-контролів одного параметра використовують один JS state. Метаінформацію range/skew/default/choices отримувати з C++; відмінний видимий діапазон координат canvas не змінює APVTS range. Double-click reset бере default/physical value з metadata, не з констант макета.

Для drag почати/завершити gesture один раз; wheel/typing/reset теж оформлювати завершеним gesture. Позиційний drag охоплює відповідні 2 параметри, RMB rotation — az/yaw. Після blur/cancel/destruction гарантовано завершити жест, включно з native захистом від незакритого JS gesture. Host automation оновлює UI через attachments без зворотного echo. Не змінювати IDs, порядок старих параметрів або VST3 identity; нові параметри лише додавати в кінець.

Host learn/control identification: додати `WebControlParameterIndexReceiver` через `withOptionsFrom`, JS `ControlParameterIndexUpdater(annotationName).handleMouseMove(event)` і DOM annotation з `parameterIndex`; `VerbWebEditor::getControlParameterIndex(Component&)` повертає індекс receiver. Не вважати relays повною заміною host context-menu: окремо перевірити й за потреби додати native menu adapter.

### 3.2. Структурний стан: запропонований контракт v1

Контракт — окремий від `.verbroom`/`VerbState`. Втілити його machine-readable JSON Schema і спільними fixtures перед C++/JS реалізацією; нижче визначення обов'язкових даних. `schemaVersion=1`, невідома major version — відмова, не частковий запис. Capabilities дозволяють приховати незреалізовані контролі.

Приклад envelope (числа й назви тут ілюструють протокол):

```json
{"schemaVersion":1,"sessionId":"editor-token","requestId":"17","kind":"command","baseRevision":"42","transactionId":"drag-3","payload":{"op":"surface.translate","refs":[{"kind":"wall","index":0}],"deltaM":[0.1,0,0]}}
```

| Повідомлення / тип | Обов'язкові поля і правила |
|---|---|
| Envelope | `schemaVersion:integer`, `sessionId:string`, `requestId:string` для request/reply, `kind:hello/snapshot/command/ack/error/patch/telemetry`, `payload:object`; для mutations `baseRevision:string`, `transactionId:string` у жесті. Revision — десятковий uint64 як string, без втрат JS >2^53. |
| Hello → snapshot | JS реєструє listeners, передає підтримані версії та readiness. C++ відповідає `revision`, `loadSerial`, `capabilities`, `limits`, `parameterMetadata`, `document`, `preferences`, `jobs`. Лише після цього дозволити редагування. |
| Document | `scene`, `sourceConfigs[]`, `output`, `tailSettings`; pose/gain/mute/solo — APVTS, передаються окремим read-only parameter snapshot при старті, надалі relays. Не створювати другу записувану копію listener/source pose у document. |
| Scene | `heightM`, `tempC`, `humidityPct`, `floorMaterial`, `ceilingMaterial:null|integer`, `materials[]`, `walls[]`, `floorZones[]`, `ceilingZones[]`, `clutter`, `roughness`, `roughSeed`, `diffraction`; зберігати також `sr`, `length`, `order`, `rays`, `seed`, `detectRadius`, початкові transducers/listener anchor, навіть якщо UI їх не показує. |
| Material | `name:string`, `classId:integer`, `alpha:number[7]`, `tau:number[7]`, `scatterLF:number`, `scatterHF:null|number` (`null` = legacy inherit). `basedOn`/display colour — окрема необов'язкова metadata, зараз не мають persistence у сцені. Смуги строго 125/250/500/1000/2000/4000/8000 Hz. |
| Wall / zone | Wall: `a:number[2]`, `b:number[2]`, `zBottomM`, `zTopM:null|number` (`null` = full height), `material:integer`, `editor`. Zone: `vertices:number[][2]`, `heightM:null|number` (`null` = patch), `material:null|integer` (opening лише ceiling), `editor`. Порядок zones значущий: остання перекриває попередні. |
| Surface reference / editor | `kind:wall/floor/ceiling/floorZone/ceilingZone`, `index:integer` від 0 (для базових floor/ceiling 0; adapter перетворює native `-1`). `editor:{name:string,group:string,hidden:boolean,locked:boolean}`. Групи плоскі; sources/listener поки не учасники surface groups. |
| SourceCfg | `name`, `format:integer 0..7`, `channels:integer 1..16`, `firstIn:integer 0..63`, `sum:boolean`, `spreadM:0..20`, `pattern:0..1`, `freePoints:boolean`, `offsetsM:number[][3]` за каналами в локальній системі джерела. Перевіряти довжину offsets, LFE, effectiveChannels, весь маршрут. |
| Listener / OutputCfg | Listener pose — relays `lx/ly/lz/laz`; document `output:{layout:integer -1..8,radiusM:0..1,micPattern:integer 0..7}`. Effective layout/rendered channels/host channels — read-only. `ringChannels` зараз не існує, не приймати його як чинне поле. |
| Clutter / tail | Clutter: `count`, `minLenM`, `maxLenM`, `minHeightM`, `maxHeightM`, `tlMinDb`, `tlMaxDb`, `minDistM`, `seed`. Tail: `baseRays:8000..384000`, `adaptive:boolean`. Не змішувати ray budget scene/analysis/live-tail. |
| Commands | Allowlist: `transaction.begin/update/commit/cancel`; `surface.translate/vertex/setMaterial/setBand/setTransmission/setHeights/create/delete/setMetadata/link/unlink`; `room.scale/setHeight/setBaseMaterial/setOpening/setAir/setClutter/setRoughness/setDiffraction/clear`; `source.add/remove/setConfig/setPattern/setSpread/setPointOffset`; `output.set`; `tail.setQuality`; `history.undo/redo`; `preset.list/load/save`; `ir.export`; `analysis.start/cancel`; `rt60.start/cancel`; `preferences.set`; `comparison.capture/activate`. Реалізовувати лише оголошені capabilities. |
| Ack / error / patch | Ack: `requestId`, `revision`, `status:accepted/noChange`, canonical changed fields або job ID. Error: `requestId`, `code`, `message`, `currentRevision`, `field?`. Коди: `invalid`, `unsupported`, `staleRevision`, `locked`, `budgetExceeded`, `notReady`, `cancelled`, `ioError`. Patch: `fromRevision`, `toRevision`, typed replacements/removals, `originRequestId`; розрив послідовності → full snapshot. |

Усі числа finite, відхиляти NaN/Infinity та malformed arrays. До sanitize перевірити бюджети, material refs, self-intersections/zero edges, висоти й потрібні зазори; sanitize може мовчки видалити zone або перетворити 9-ту plate на patch. Спочатку валідувати кандидат, потім застосувати атомарно, повернути canonical результат. Наявний `editScene` повертає void і sanitizer не є достатнім ack: command service мусить визначати no-op/rejection самостійно, перевіряти прийнятий стан і тестувати його. Межі payload/рядків/черги встановити у schema (наприклад 1 MiB для control document, 16 KiB для drag update, 128 символів імен); це нові захисні межі протоколу, не заявлені engine limits.

Індекси поверхонь не стабільні після delete/sanitize. Для v1 достатньо references з обов'язковою revision і full resync після структурної зміни; не вигадувати persistent UUID у старому `.txt`. Після source removal чинний C++ зсуває позиції/gain/M/S наступних слотів: зберегти поведінку, попереджати про індексну автоматизацію, не переприв'язувати host параметри за DOM ID. Revision bridge має змінюватися і для live pattern/spread/offset edits: сам `sceneSerial` зараз не покриває всі setters. Для host restore/async load інвалідувати активні транзакції та snapshots; одних кількох послідовних getters недостатньо для гарантовано узгодженого стану — потрібен узгоджений snapshot adapter.

**Undo належить C++.** Винести правила редагування й історію з `VerbEditor` в спільний command service без browser dependency. Один non-parameter жест — одна транзакція з initial/final state; incremental deltas рахуються від початкового знімка, не накопичуються при повторній доставці. `cancel` відновлює початок; stale revision abort → resync. Parameter automation лишається APVTS + host gestures; не дублювати її JS undo stack. Compound operation (scale + poses, source delete + slot shift, A/B) має явний C++ snapshot потрібних параметрів і non-parameter state; не відновлювати весь APVTS при звичайному undo стіни. Native спочатку лишається зі старими hooks; далі вони делегують цьому ж service. Нові redo/compound semantics — окрема перевірювана зміна.

Throttling: локально canvas реагує кожний `requestAnimationFrame`; parameter values передавати до 60 Hz на активний контрол із гарантованим final update; структурні geometry/material updates — спочатку до 20 Hz з coalescing і final flush. Rebuild setters (`setSources`, `setOutputCfg`) викликати на commit або debounce 150–250 ms, а live setters pattern/spread/offset — під час жесту. Черга bounded, повторні requestId ідемпотентні, не виконувати другий раз add/delete/export. Metadata rename/hide/lock використовує `editSceneMetadata`, не акустичний rebuild. На non-parameter commits забезпечити host dirty notification; зараз не всі setters роблять це послідовно.

### 3.3. Дані високої частоти й потоки

| Дані | Наявне джерело | Доставка / частота |
|---|---|---|
| Вхідні meters | `inputPeak(ch)` атоміки, `hostInputChannels`, input-map; значення до source gain | Message-thread UI sampler 20–30 Hz одним масивом до 64 каналів; smoothing/peak-hold у frontend. E12: Output peak/RMS після кінцевого тракту, `meters.output` до 16 host-output каналів; RMS 150 ms, peak hold 1.5 s у UI, clip защіпнутий до кліку (`output.resetClip`). |
| Tail / Decay | `getTailDiagnostics()`: immutable `shared_ptr<const TailDiagnostics>` | При зміні pointer/revision; до 5–10 Hz перевірка, без повторного stream незмінених arrays. 7×256 `decayDb` + 7×256 `physicalDecayDb`, RT60/physicalRT60/reliableSeconds/active/duration/multipliers/raysUsed/baseRays/rayLimit/shared/adaptive/needsMoreRays. |
| Energy | `getEnergyScene(source)` → worker `analyzeEnergyMap` | Винести worker/cancellation/signature/result із `EnergyMapView` в service; snapshot на message thread, worker рахує на копії, completion не торкається browser. Events: progress ≤10 Hz, result-ready з ID/revision. Старий результат зберігати зі stale flag. |
| Load / export / target | `getLoadState/getStatus`, `getExportState/getExportMessage`, `getTargetRT60State/getTargetRT60Achieved` | 5–10 Hz компактні статуси; не придумувати відсоток там, де API дає лише стан/текст. `startTargetRT60` замість синхронного `setTargetRT60`; repeated start зараз може join попередній worker — перевірити неблокувальний lifecycle. |

Audio callback не парсить JSON, не викликає WebView, dialogs чи message locks і не створює UI buffers. Наявний `processBlock` бере try-lock engine, а load worker міняє bundle під коротким lock; не додавати довге UI утримання `engineLock`. `getTailDiagnostics()` брати лише для короткого shared-pointer snapshot, сериалізація — після release; `getDisplayScene` копіює під `dispLock`, не робити це на кожний meter tick. Параметричні callbacks можуть приходити не з message thread — UI оновлення marshalled, стандартний `ParameterAttachment` вже має механізм такого узгодження.

Для Energy максимум 96×96 cells: 21 float32 на cell (3 компоненти ×7 смуг) ≈756 KiB + mask. Не передавати це JSON 30 разів/с і не кодувати base64. Зберігати immutable result cache C++; event містить descriptor. JS дістає локальний `application/octet-stream` через resource provider / `getBackendResourceAddress`: версія binary layout, resultId, nx/ny, bounds, slice, float32 little-endian і mask. Для звичайного перегляду достатньо проєкції вибраної band/components ≈36 KiB + mask; зміна band/contrast не запускає retracing. Обмежити cache кількома результатами й одну незавершену fetch на view; route повертає готові байти, не рахує акустику. Той самий принцип придатний для кривих, але їхній невеликий обсяг допускає JSON при зміні.

### 3.4. Пресети, файли, A/B і локальні настройки

- Native async `FileChooser::launchAsync`, AlertWindow для імені/overwrite/error лишаються C++; JS просить семантичну дію, не передає довільний filesystem path. Список бібліотеки через opaque IDs. Винести `libraryDir`, scan/open/load/save зі старого editor в спільний library controller; директорія `Documents/SBP/Verb Room/Rooms` зберігається.
- Builtins — `verbp::builtinRooms()` і `loadRoomWithSetup` для setup-bearing rooms; не копіювати три демонстраційні PRESETS із HTML у продукт. `.txt` іде у `loadSceneText`, повний обмін — `getRoomPresetText/loadRoomPresetText` `.verbroom` форматів 1/2. Поточний host state лишається `VerbState`, без JSON як нового основного формату. Невідомий preset version/неповний payload має відхилятися перед зміною.
- Keep I/O використовує C++ `setKeepRoutingEnabled` і чинну семантику: список/імена/формати/маршрути/offsets/output локальні, кімната/позиції/mix імпортуються за індексом. Не застосовувати цю політику до DAW project restore; не дублювати її в JS.
- IR export — `startExportIR` на processor worker, один multichannel WAV на spatial point та room `.txt`; UI показує реальний статус/помилки. На час вибору/підтвердження export зафіксувати налаштування або чітко показати, який snapshot використаний: поточна реалізація читає частину APVTS/offsets усередині job. Public cancel export API зараз немає; не обіцяти кнопку cancel без нового API. FileChooser callback захищений SafePointer/session; закриття UI не скасовує processor export.
- **A/B — нова C++ controller-функція**, не JSON у localStorage. Рекомендовано повний акустичний state двох слотів (mode, усі параметри, сцена, routing/output/quality), локальні UI preferences поза слотами; capture/activate транзакційно, undo визначений. Спочатку session-only без persistence слотів, пізніше необов'язковий сумісний child у host state. Активація A/B має власну політику без Keep I/O; уникнути прихованої зміни кількості каналів і стрибків/множинних rebuild.
- Теми/zoom/dock/selection не DSP. Теми зберігати через C++ local settings, не залежати від ephemeral UDF/localStorage; per-source/material colours потребують нової display metadata, якщо їх треба зберігати у проєкті/пресеті. Не додавати кольори в `Material.alpha/tau/cls`. Поточні `keepRouting/inputMeters/mixerCompact/navLocked/showOutliner` у project state та local-on-preset семантика зберігаються.

## 4. Рендеринг і справжні бюджети

**Перший реліз: Canvas 2D для Plan/Front/Side і наявної projected 3D.** Це відповідає затвердженому вигляду, не потребує Three.js і працює без вимоги GPU. Абстрагувати projection/picking/render layers; WebGL тільки після профілювання, окремим backend із Canvas fallback. На Linux JUCE вимикає acceleration, тому WebGL не можна зробити обов'язковим.

| Бюджет | Джерело / значення | Наслідок для UI |
|---|---|---|
| Стіни | Editor/`editScene`: 128. Engine early order ≤1/2/3/4: 128/64/24/12; VST3 зараз `erOrder=1` | Показувати фактичний залишок; clutter використовує той самий wall budget, при 128 ручних стінах новий clutter не поміститься. Не додавати порядок відбиттів за `.txt order`. |
| Зони | `sanitizeScene`: до 32 floor +32 ceiling, 3–64 vertices кожна | До 4096 vertices, порядок перекриття зберігати. Відхиляти надлишок до sanitizer. |
| Платформи / навіси | `kMaxPlates=8` сумарно, це частина zones | 9-та plate не має мовчки стати patch. Геометрія 2.5D: вертикальні стіни/горизонтальні площини, не довільний mesh. |
| Джерела / точки / входи / виходи | 32 /64 /64 /16; Source Ring 1–16 у C++ (UI 2–16) | Рахувати через `buildPoints`, Sum усереднює групу до однієї точки, LFE виключений зі spatial points. `firstIn+effectiveChannels≤64`, actual host availability окремо. |
| Координати | APVTS X/Y ±200 m, Z 0–30 m, az/yaw ±180°; room height 1–30 m | Не копіювати широкі parser межі в parameter UI. Зберегти можливість розміщення поза контуром для open/free-field scenes; макетне обов'язкове inside-placement звужує чинні можливості. |
| Clutter | UI count 0–80; sanitizer до 1000, але engine обмежує сумою 128 walls | Показувати requested/generated, генерувати через C++, не JS PRNG іншого алгоритму. |
| Tail / Energy / Decay | live rays 8000–384000, Adaptive до 4× бази; tail до 20 s; Energy long side 8–96; Decay 7×256 | Не змішувати resolution з rays; це вартість аналізу, не кількість DOM nodes. |

За таких обсягів Canvas 2D має бути достатнім; це інженерне очікування, **вимірювань немає**. Ціль QA на зафіксованій машині: drag/orbit p95 frame ≤16.7 ms Windows/macOS, ≤33 ms software Linux, parameter feedback ≤50 ms, без сталих tasks >50 ms та без деградації audio underruns порівняно з native. Перевіряти worst case 128 walls +64 zones ×64 vertices +64 points +16 receivers, 96² map, 1/8/32 редактори; окремо VSL 32 stereo Sum groups на 64 входах.

Прибрати макетний повний `refresh()`/metrics/path/energy recompute на кожну зміну: dirty flags, cached Path2D/projections, окремий raster Energy, spatial hit-test cache, DOM updates лише змінених полів, redraw тільки видимих canvas. 3D depth-sort сотень faces дешевший за підтримку нового GPU стека. `requestAnimationFrame` не крутиться без змін. На DPR 2 збільшується саме raster memory; за потреби обмежити rendering DPR після перевірки читабельності.

Energy зараз C++ рахує **XY slice на probe height**, не Front/Side вертикальні зрізи, а HTML малює ілюстрацію навіть там. Спочатку overlay лише Plan; Front/Side вимкнути з поясненням або окремо розширити `EnergyMapOptions` і solver координатами площини та тестами. Справжній vertical slice — нова analysis-функція, не перестановка пікселів XY карти. Reflections/path timing та volume/area у HTML теж ілюстративні: потрібен C++ diagnostic adapter або чітко позначений геометричний preview; не називати JS `paths()` повним списком чутних engine taps.

## 5. Мапа всіх сімейств контролів макета

Позначення: **P** — чинний automatable parameter ID; **M** — наявний C++ метод/поле, потрібен bridge; **UI** — лише відображення; **NEW** — нова controller/analysis/DSP можливість. Повторення одного контролу в Main/Edit/таблиці використовують одне джерело стану. Для `sN` N=1..32; listener ID yaw саме `laz`.

| Контролі макета | Відповідність | Уточнення інтеграції |
|---|---|---|
| Room dropdown, ‹/›, назва/розміри | **M** `builtinRooms`, `getSceneName`, `loadSceneText/loadRoomWithSetup/loadRoomPresetText`; **UI** browse | Додати My rooms, Load file, Open library folder зі старого editor; headers читають реальні дані. |
| Save | **M** `getRoomPresetText`; native save controller | Browser snapshot Save замінити справжнім `.verbroom`, overwrite/error через C++. |
| Export IR | **M** `startExportIR/getExportState/getExportMessage` | Не HTML download. |
| Physical / Creative | **P** `mode` (0/1), паралельна реалізація | Показувати stored/effective окремо; режим не стирає overrides. |
| A / B | **NEW controller**, §3.4 | Не engine algorithm; не залишати незалежні JS слоти. |
| Theme у двох headers | **NEW UI persistence** | Готові Amber/Ember red/Ice blue/Violet/Monochrome/Classic emerald/Custom; 9 ролей, 4 Energy palettes, accent preset/picker, wall override, reset — без зміни DSP. |
| Edit room / Done; нижні Decay/Sources/Mix/Output | **UI** routes/layout | Selection/context синхронний, viewport не стрибає при зміні вкладки. |
| Plan/Front/Side/3D, orbit/pan/wheel/reset, −/zoom/+ | **UI** projection/navigation | Старі `showPlane/setAutoView/zoomView` — методи editor, не processor API. JS переносить взаємодію; world coordinates зберігаються. |
| Reflections toggle; Distance/arrival/path disclosure | **UI + NEW diagnostics adapter** | Немає public processor API списку reflection paths; C++ `orientObject` не є path diagnostic. Поважати z, zones, transmission, diffraction. |
| Energy on/off; Band; Cells; Range; Auto contrast | **M** extraction з `EnergyMapView`/`analyzeEnergyMap` | 7 bands+broadband; resolution/contrast окремі; додати реальні Analyze/Cancel/progress/stale та advanced controls. Front/Side — NEW slice, §4. |
| Scene RT60 main/Low/Mid/High, volume/area, counts, mode/overrides | **M** `getTailDiagnostics/getRT60Estimate`, scene/source/output getters; **NEW** summary adapter | Показувати estimate/unknown, а не macro target як вимірювання. Volume/area для відкритих/незамкнених сцен не видавати за точний фізичний об'єм. |
| Listen Direct/Early/Tail | **P** `soloDirect`, `soloEarly`, `soloTail` | Дозволені комбінації; усі off = normal. Зберегти LFE semantics. |
| Freeze; Duck | **NEW DSP + params** | Немає backend. Приховати/disable з поясненням; Duck потребує визначення detector/sidechain, Freeze — поведінки хвоста. Creative сам собою їх не реалізує. |
| Size macro 50–160% | Частково **M** `scaleRoom(factor, includeObjects)`, але **NEW compound macro** | Немає `size` параметра. Поточний scaleRoom не масштабує zones; потрібні absolute baseline, zones/heights/objects/offsets і undo. Не перетворювати cumulative scale у automatable Size без окремого DSP/design рішення. |
| Absorption macro | **P** `absorption`, паралельна реалізація | Макет 10–500%; поточний параметр 0.3–3.0 =30–300%, skew metadata. Не розширювати engine range через JS. Доступний в обох режимах. |
| RT60 target macro | **M** `startTargetRT60/cancelTargetRT60/getTargetRT60State/getTargetRT60Achieved`, **NEW UI workflow** | `setTargetRT60` синхронний — не у native callback. Commit на кінці жесту; target/achieved/physical estimate різні. Поточний solver цілиться у mid 500 Hz–2 kHz через absorption; показати unreachable/cancel. |
| Predelay; Stereo width | **NEW DSP + params** | Немає `predelay`/`width` parameter. Source spread ≠ stereo width, відстань/c ≠ додатковий predelay. У першому релізі неактивні/приховані. |
| Mix macro | **P** `mix` 0–100 | Поважати `spatialDry` і Solo: це не завжди звичайний input bypass crossfade. |
| Mix: Early, Tail, Early diffusion, Doppler, Output gain | **P** `early`, `tail`, `diffusion`, `doppler`, `gain` | Межі відповідно −60..6 dB, −48..12 dB, 0..1, 0..2, −36..12 dB. **Розбіжність:** макет блокує diffusion у Physical і виводить із scatter; прочитаний engine застосовує `diffusion` в обох режимах. Узгодити з агентом рушія; не показувати брехливий lock/ghost. |
| Positioned dry | **P** `spatialDry` | Старим states/formats зберегти raw bypass default, новим — positioned default. |
| Decay Low/Mid/High handles, reset double-click | **P** `decayLow/Mid/High` 0.3–3.0, паралельна реалізація | Low 125–250; Mid 500/1k/2k; High 4k/8k. Physical unity, Creative multipliers. Не малювати лише прямі mock lines замість diagnostics. |
| Decay Direct/Early/Tail/physical legend | **M** TailDiagnostics для tail; **NEW** direct/early diagnostics якщо потрібна реальна energy timeline | Наявний `TailDecayView` має 7 октав, active flags, solid/extrapolated/faint physical та T20; макетні 3 handles не скасовують точність/семантику 7 кривих. |
| + Source / − Source | **M** `addSource/removeSource` | Мінімум одне; preflight каналів/точок до mutation. З Output прибрати згідно POLISH_TODO; у Sources/контексті — той самий controller. |
| Keep I/O | **M** `keepRoutingEnabled/setKeepRoutingEnabled` | Показувати у Sources; зберегти семантику повних пресетів, не browser approximation. |
| Sources name, first input, format, channel count | **M** `getSources/setSources`, `SourceCfg` | Назви не параметри; input UI від 1 ↔ backend від 0. Mono/Stereo/LCR/Quad/5.0/5.1/7.1/Ring; fixed count read-only, Ring 2–16 UI. |
| Sources gain/M/S у таблиці й inspector | **P** `sNg`, `sNmute`, `sNsolo` | Mute має пріоритет; існуючі tails згасають природно. Не перестворювати DOM під focused input. |
| Source Sum; Spread; Pattern; Free points | **M** `setSources` + `sum/freePoints`, `freezeLayout`; `setSourceSpread`, `setSourcePattern`, `setPointOffset` | Pattern — scalar omni/cardioid/figure-8, не спектральна модель інструмента. Offset local X forward/Y left/Z up; Sum вимикає spatial layout, LFE не draggable point. |
| Instrument Omni/Violin/Cello/Trumpet/Voice/Piano/Organ | **NEW DSP + model data/persistence** | Немає frequency-dependent instrument directivity presets; не мапити назву Violin на чинний scalar pattern як еквівалент. |
| Source X/Y/Z, azimuth; drag/RMB rotation | **P** `sNx/sNy/sNz/sNaz`; **M** `orientObject` лише для aim-at | Макетне відносне RMB обертання можна надсилати az relay; aim-at C++ має іншу семантику. |
| Source custom colour | **NEW display metadata** | Native має детермінований colour за індексом, persisted source colour відсутній. |
| Listener X/Y/Z/yaw | **P** `lx/ly/lz/laz` | Source inspector/Listener/viewport мають спільну selection model. |
| Listener та Output Layout, Mic, Radius | **M** `getOutputCfg/setOutputCfg`; `layoutDefs/micPatternDefs` | Наявні 9 layouts і 8 patterns, ORTF/XY/Human ears через C++ receiver geometry. Ears radius 0.0875 m при виборі; Human ears — head shadow, не HRTF renderer. |
| Ring channels у Output/Listener | **NEW OutputCfg/persistence/engine setup** | Зараз Ring count = min(host outputs,16), `OutputCfg` не має count. Спочатку read-only actual count або окреме engine завдання; не переробляти DAW buses із JS. |
| Show microphone array to scale | **UI**, receiver descriptor з C++ | Не змінює звук. Показувати configured і фактично rendered count: engine обрізає до host outputs. |
| Output Convolution / Hybrid FDN | Convolution чинний engine; **NEW DSP** Hybrid FDN | Поточний tail — ray energy histogram → band noise → convolution, не FDN. Capability-based selector, тільки чинний варіант активний. |
| Tail Quality Draft/Normal/High/Adaptive, Rays, Adaptive checkbox | **M** `getTailSettings/setTailSettings` | Замінити макетні 1024–65536/default 8192 на 8000–384000/default 48000. Native presets Preview 12000/Mix 48000/Render 192000/Custom; adaptive незалежний bool, не четверта взаємовиключна quality. |
| Directional tail; LF room modes; Velvet transition | **NEW DSP + params/settings** | Немає таких виборів/API. Shared/per-source tail, smooth crossfade чи матеріал Velvet_drapes не є цими можливостями. |
| Edge diffraction у Output й Edit | **M** `editScene` → `Scene.diffraction` | Наявний engine feature, не параметр APVTS. Один controller/undo для обох місць. |
| Binaural; AmbiX | **NEW DSP/layout/format**; **відсутні у v6 LAYOUTS** | Згадані у завданні як майбутні; HRTF/binaural decoder та Ambisonic ACN/SN3D encoding не знайдено. Human ears ≠ повний Binaural, Ring ≠ AmbiX. Не включати в обсяг parity. |
| Edit Move/Box/Add wall/Floor zone/Ceiling zone/Delete/Undo/Clear | **M** `editScene` + **NEW shared command service**, правила зі старого editor | Add/delete індексів/матеріалів, locked groups, shared corners; Clear лишає sources/listener/air й обнуляє walls/zones/clutter/roughness. Undo один крок, clear confirmation зберегти. |
| Snap 10 cm; Auto-link corners | **UI + C++ validation** | Snap є в native (Alt free); auto-link як explicit switch новий UI. Зміна з'єднаного corner не має пересувати locked сусіда. |
| Auto view; Outliner open/close; resize dock | **UI**, `setOutlinerVisible`; **NEW** dock-width preference | Native dock fixed 220+6 px, макет 150–310 px; resize доступний клавіатурою. Додати відсутній Lock pan. |
| Walls/Floors/Ceilings/Plates layers | **UI** shown/locked/hidden | Відрізняти тимчасову layer mask від persisted per-surface metadata; ніщо з цього не вимикає акустику. |
| Outliner select/multiselect/collapse/rename/eye/lock/Link/Unlink/group name | **M** `editSceneMetadata`, `SurfaceTag`, helpers у `SceneObjects.h`; **NEW** command wrapper | Група з хоча б одним locked member не рухається/не видаляється частково. Макетні source/listener hide/lock не мають backend metadata: session UI-only або окреме сумісне persistence завдання, не acoustic mute. |
| Surface class tag; filter chips; search; preset; Custom based on | **M** `surfaceClasses/surfacePresets/presetByName`, `editScene`; **UI** filters; **NEW** provenance metadata | Клас у v6 read-only; existing native Class selector свідомо не переноситься як акустичний control. Clone material per selected surface; зміна однієї band не копіює всі bands першої поверхні на групу, як робить макетний `shareMaterial`. |
| Material colour picker/swatches/palette; material legend | **NEW display metadata/UI** | Колір незалежний від coefficients/class; precedence theme/source/material описати й зберегти. |
| 7 absorption bars, scatter LF/HF, TL LF/MF/HF | **M** `editScene` → `Material.alpha/tau/scatter/scatterHf` | alpha+tau≤1. Зберігати 7 tau; TL = −10log10(tau), opaque = tau 0. Native opaque 60 dB UI, mock 99; не зберігати sentinel як фізичний TL. Native TL grouping 2/2/3 bands, mock 2/3/2: новий web grouping має бути явний і не змінювати неохоплені bands при одному edit. |
| Wall Bottom/Top; Plate height | **M** `editScene` → `Wall.z0/z1`, `Zone.z` | Full-height/patch/opening передавати явно, не Number Infinity. Негативна plate height означає patch; floor plate at 0 sanitizer робить patch. |
| Roughness all surfaces | **M** `editScene` → `roughness/roughSeed` | Звук/seed детерміновані C++; display colour не впливає. |
| Room ceiling height; Floor/Ceiling material; Open sky | **M** `editScene` → `height/floorMat/ceilMat` | Також зберегти **opening окремої ceiling zone**, чого глобальний Open sky не замінює. |
| Scale room factor / Apply / Move sources-listener | Частково **M** `scaleRoom`; **NEW corrected compound command** | Потрібне масштабування zones і перевірка висот, offset/spread policy; native зараз не тотожний macro/mock geometry. |
| Air Temperature/Humidity; speed/air-loss readout | **M** `editScene`, `tempC/humidity`, `speedOfSound/airAbsDbPerM` | Рахувати C++; native діапазони −20..45°C,5..100%. |
| Clutter Count/Min-Max length/height/TL/Seed/Randomize/Show | **M** `editScene` → `ClutterCfg`; **UI** Show | Не акустично видаляти clutter при Show off. Seed/roughSeed зв'язати за чинною UI політикою. |
| Clutter Source clearance | `ClutterCfg.minDist` є, але **NEW persistence** | Parser/serializer clutter не читає/не пише minDist: зміна зараз губиться при reload. До сумісного розширення формату показувати 0.5 m read-only. |
| Status/footer, budgets, notices, selection ×/Esc, details disclosures | **M** load/status/host counts + **UI** | Реальні errors/not-ready/stale, X/Y/Z units, невідомі estimates — без демонстраційних чисел; footer версія справжньої збірки. |

### Що native вміє, але макет не покриває повністю

1. **Energy advanced:** Analyze/Cancel, All/one source, probe height, Direct/Early/Tail components, integration horizon, independent analysis rays, result hover values, stale/result preservation. Залишити компактний overlay, додати розкривний Analysis panel. Відокремити ці components від Listen audio Solo.
2. **Decay:** 7 octave visibility toggles, T20 RT60, reliable vs extrapolated curves, physical/result curves, rays used/limit/shared/sparse warning. 3 Creative handles лишаються macro editor над реальними 7-band diagnostics.
3. **Source mixer:** input peak meters до gain, доступ до всіх 32 strips, Compact/Normal з local preference; таблиця v6 не є повним strip mixer. Відновити meters і density switch/альтернативний strips view.
4. **Routing/output:** Match track auto layout (`layout=-1`), actual host in/out, channel truncation warning і input channel meter toggle. Відсутній Input mode не повертати: параметра `inputmode` у `makeLayout` немає, поля editor — залишок старого UI.
5. **Navigation/edit:** Lock pan, repeated-click cycling overlapping zones/walls, Alt select one linked member, wall top/bottom drag у Front/Side, local ceiling opening, бібліотека користувача/load/reveal. Вони мають acceptance scenarios, а не лише візуальні кнопки.
6. **Sound passes master** і editable Class у native відсутні в v6. Рекомендація: свідомо замінити Sound passes на точний TL/opaque control; Class лишити read-only + display colours. Вказати це як погоджені відмінності parity, не приховану втрату.

## 6. Frontend і робочий цикл дизайну

Залишити **plain ES modules**, DOM/CSS, Canvas/SVG. Framework для двох сторінок і такого навантаження не потрібний. Додати невеликий детермінований build step (наприклад esbuild з lockfile) для bundling/transpile/minify і ресурсного manifest; Node потрібний розробці/CI, не плагіну. Нижню JS/CSS планку визначити macOS 11 WebKit: поточні `.at`, `replaceAll`, `structured`/сучасні CSS можливості перевіряти, потрібні polyfills включати локально. Transpiler сам не виправляє runtime builtins/CSS.

Пропонований поділ нового `web-ui/` (макет лишається затвердженою reference-копією):

- `src/app.js`, `state/store.js`, `state/selectors.js`, `bridge/contract.js`, `bridge/juce.js`, `bridge/mock.js`, `bridge/parameters.js`.
- `components/knob.js`, `slider-readout.js`, `toggle.js`, `menu.js`; єдина gesture/fine/wheel/type/reset логіка, без заміни knob на slider.
- `pages/main.js`, `pages/edit-room.js`, `panels/context.js`, `sources.js`, `mix.js`, `output.js`, `surface.js`, `outliner.js`, `analysis.js`, `decay.js`.
- `viewport/projection.js`, `picking.js`, `tools.js`, `render2d.js`, `render3d.js`, `energy-layer.js`; координати тільки у метрах, без подвійного K.size scaling.
- `styles/tokens.css`, `layout.css`, `controls.css`, `themes.css`; `fixtures/` з C++ snapshots/capabilities; `tests/` для browser/contract.
- `vendor/juce/` — точна копія двох vendored JS helpers з attribution; `dist/` у build tree, explicit manifest для BinaryData. Не включати shots/test fixtures/source maps у release.

Нормальний browser dev server дозволений лише в майбутній розробці: mock bridge реалізує той самий async інтерфейс/validation errors/revisions/jobs, включно з automation updates, delay, cancellation, stale і unsupported capabilities. Дизайнери міняють layout без DAW; acoustic fixtures походять із C++, а не з mock metrics. Browser-mode не створює фальшиву production `window.__JUCE__`: adapter обирається до імпорту JUCE helper. Проганяти однакові fixtures через mock і native bridge. Не обіцяти аудіопрослуховування в mock browser.

Під час переносу виконати всі 4 `POLISH_TODO`: full-button tab highlight; забрати Source/Keep I/O з Output; підняти Mix knobs/зменшити порожню висоту; numeric rows → slider+readout, rotary лишаються rotary. UI може лишатися англійським, документація/комунікація українською. Theme tokens, keyboard/ARIA states, reduced motion й збереження focus/scroll при updates обов'язкові.

## 7. Етапи, перевірки, CI та пакування

Пропоновані project options (нові, не JUCE API): `VERB_ENABLE_WEB_UI` (початково OFF), `VERB_DEFAULT_EDITOR=NATIVE|WEB|AUTO`, `VERB_WEB_UI_DEV` (OFF у release). Native завжди доступний у перехідних збірках. Runtime preference — локальний; зміна редактора при наступному open або у wrapper без видалення editor під час callback. Зовнішня до browser native кнопка/меню «Native editor» та автоматичний fallback після failed ready/невідповідного schema/resource error. Timeout робити асинхронним із loading screen, без блокування DAW; встановити після вимірювання cold start (початковий орієнтир 5 s, не заявлена гарантія).

| Етап | Поставка | Як приймаємо |
|---|---|---|
| 0. Узгодження | Стабілізований processor API, matrix контролів/capabilities, schema fixtures, перелік свідомих parity відмінностей | Агент рушія завершив Physical/Creative; повторно звірені ranges/міграції/diagnostics, особливо diffusion, Ring, scale zones і minDist. |
| 1. Shell spike | Browser з BinaryData, один gain і toggle, native fallback; Win/mac лише opt-in | Offline open, missing runtime/UDF/error fallback, DPI/focus, repeated close, 8 instances. Аудіо та state не змінюються від вибору editor. Це go/no-go до великого переносу. |
| 2. Parameters/Main | Усі чинні relays, Sources/Mix/Output/Listener, mode/effective values, реальні library/load/save/export | Host automation write/read/gestures, 64 input routing, M/S/LFE, state/preset formats1/2 + Keep I/O round-trip, impulse до/після editor switch. |
| 3. Edit parity | C++ commands/undo + walls/zones/materials/clutter/groups/hidden/locked, усі views/navigation | Old editor command tests + bridge tests, browser tool scenarios; геометрія/висоти/бюджети, impulse і reload `.verbroom`. Жодного JS-only акустичного edit. |
| 4. Diagnostics/polish | C++ Energy service, Decay telemetry, advanced panels, themes, A/B controller, всі POLISH_TODO | Cancel/stale/rays/reliable curves, UI snapshots порівняно з v6, large-scene performance; A/B не губить routing або stored Creative values. NEW DSP вимкнені за capabilities. |
| 5. Release gates | Web за замовчуванням Windows/macOS лише після host matrix; Linux native stable + optional experimental | Clean-machine install/offline/missing deps, host scan/render/save/reopen, multi-instance RAM/close stress, package dependency/signature checks, rollback через native. |

### Збереження тестів без тихого пропуску

`PluginTest.cpp` багаторазово робить `dynamic_cast<VerbEditor*>(p.createEditor())`, шукає `Slider/Button/ComboBox/SceneOutliner/EnergyMapView` серед child components, синтезує `MouseEvent` та `createComponentSnapshot`. Просто повернути WebEditor і лишити `if (ed)` означає **пропустити частину перевірок**; деякі гілки вже явно перевіряють cast і впадуть. Поточний `Stage2Test.h` навіть розіменовує результат такого cast без null-check — при зміні factory це можливий crash, а не тільки skip. WebView native child window також не можна перевірити звичайним JUCE component snapshot.

- Залишити native test target із `JUCE_WEB_BROWSER=0` і явною native editor factory (або окремими compile definitions), усі старі hooks: `selectWall/selectWalls/selectZone/selectSurfaceObjects/selectedSurfaces`, `linkSelection/unlinkSelection`, `undoEdit/deleteSelection/applyPresetToSelection/clearEverything`, `chooseBuiltinRoom/numBuiltinRooms`, `showPlane/showPage`, `testWorldPoint/testCanvas/testViewPsi/testPickCount/testSetLayer`, `setAutoView/zoomView`. Зберігати також прямі mouse/key сценарії та child-control assertions; невідповідний editor type — failure, не skip.
- Винесені команди тестувати headless без browser: validation/no-op/rejection, lock/group rigid move, незалежні material bands, shared material cloning, source deletion remap, transaction cancel/final flush, stale/dedup/schema limits, preset versions/Keep I/O, async restore під час жесту. Relays — attachment/automation tests з fake transport/host event spy; жодного JSON в audio path.
- Playwright Chromium **і WebKit** з mock bridge: усі 16 v6 станів і 4 polish зміни, pointer capture/RMB повторний drag, Front/Side overlap picking, dock reflow, keyboard numeric editing, mode/ghosts, focus/ARIA/reduced motion, scroll/32 sources; viewport 900×600,900×760,1160×760,1280×800,1600×900 і DPR1/2. Відмінності системних шрифтів не маскувати великими screenshot tolerances. `qa-v6.cjs` та context-v5 — джерело сценаріїв, не доказ роботи real bridge.
- Native WebView harness/test host на кожній платформі перевіряє production resource scheme, native JS helpers, ready/automation/event/resources/reopen/error fallback. Browser WebKit не еквівалент системному WKWebView; headless Chromium не перевіряє WebView2. До release потрібні реальні host smoke tests, включно з REAPER та Live/іншим доступним VST3 host.
- Processor/DSP regression лишається незалежною: `portable_presets`, `spatial_editor`, `energy_editor`, `vsl_hall_presets`, `tail_regression`, `quality_test`, `energy_map`, `stage2_engine/stage2_processor` і потрібні env-гілки mixer/source switches/routing/quality. Поточні env-гілки не всі зареєстровані в CTest — явно додати потрібні окремі tests. Після акустичних edits обов'язкові impulse (перша/остання група й стереоканали, finite/non-silent/tail continuity) та preset reload; parser success недостатній.

### CMake і CI

| Частина | Заплановані зміни |
|---|---|
| CMake targets | Додати resource library та WebEditor/bridge sources умовно, явний `juce::juce_gui_extra`; зняти безумовне `JUCE_WEB_BROWSER=0` лише з web targets. Definitions узгодити для всіх TU target і JUCE modules, не змішувати 0/1. `NEEDS_WEB_BROWSER`/`NEEDS_WEBVIEW2` передавати у `juce_add_plugin/juce_add_*` до створення target; test/stress native targets лишаються browser-free. |
| Windows | `NEEDS_WEBVIEW2 TRUE`, `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1`, `JUCE_WEB_BROWSER=1`, curl=0. Пінований Microsoft.Web.WebView2 NuGet SDK; `JUCE_WEBVIEW2_PACKAGE_LOCATION` вказує батьківську директорію пакета. `FindWebView2.cmake` шукає `build/native/include/WebView2.h` і `build/native/x64/WebView2LoaderStatic.lib`; у його підказці версія 1.0.3485.44, вибір релізу зафіксувати в CI, не брати випадковий перший із кількох. Додати відсутній Windows MSVC job; runtime для browser integration test, плюс окремий missing-runtime сценарій. |
| Динамічний loader — альтернатива | `JUCE_USE_WIN_WEBVIEW2=1` без static macro, SDK headers + архітектурно правильний `WebView2Loader.dll`, `withDLLLocation` абсолютним bundle-relative шляхом. `NEEDS_WEBVIEW2` у цьому JUCE саме підключає static library; не видавати його за автоматичне DLL packaging. Static варіант простіший. |
| macOS | `JUCE_WEB_BROWSER=1`, default backend; WebKit framework уже оголошений у модулі. Зберегти Universal/AVX2 OFF/deployment11/lipo/codesign; Node/frontend checks і реальний WKWebView integration. Не додавати WebView2 macros на Apple. |
| Linux stable | Без browser/GTK/WebKit, наявні X11/audio deps, AVX2 OFF, xvfb. Довести, що пакет зберіг системні вимоги й не підтягує WebKit. |
| Linux experimental | Окремий build dir/артефакт; `NEEDS_WEB_BROWSER TRUE`, GTK3/WebKitGTK4.1 dev packages на build runner, JUCE helper automatic для plugin. Для console harness окремо налаштувати його lifecycle. Clean runtime-only container з WebKit, без WebKit, без dev symlinks, noexec temp, X11/XWayland. Не переносити workaround sandbox-disable у production. |
| Workflow frontend | Paths додати `web-ui/**`, schema/fixtures і релевантні assets (нині `ui-mockup/**` не тригерить workflow); Node lockfile/cache, `npm ci`, frontend build/contract/browser QA до CMake; dist manifest як dependency resource target. Native-only build може не потребувати Node. |
| Результати QA | Звіти CTest/bridge/browser + screenshots/runtime/backend versions і p95/RAM measurements в artifacts; не лише headless JS green. Піновані fixtures/hash узгоджуються з C++ capabilities. |

Точні CMake джерела: `third_party/JUCE/extras/Build/CMake/JUCEUtils.cmake` (`_juce_link_optional_libraries`, `_juce_create_pkgconfig_target`, `juce_link_with_embedded_linux_subprocess`, `juce_add_binary_data`), `FindWebView2.cmake`, `docs/CMake API.md`. Для Linux не стверджувати, що `NEEDS_WEB_BROWSER` обов'язково додає прямий link WebKit: тут використано NOLINK/dlopen.

### Пакування, залежності й розмір

`ci/package_vst3.py`/`binary_readme.py` і `docs/Cross_platform_builds_en.md`, `Packaging_en.md` доповнити backend/runtime requirements, способами native fallback та полем UI backend у BUILDINFO. Windows зараз пакується вручну: додати перевірений staging script/job, а не змінювати publishing mirror. Зберегти bundle/permissions/checksum; на macOS ad-hoc signature не стає Developer ID/notarization від зміни UI. Для публічної notarized поставки потрібний окремий процес. Не змінювати AVX2 policy або VST3 identity разом із UI.

Frontend близько 190 KB до поділу, але це не приріст фінального VST3: додадуться bridge, browser backend/glue/static loader, за потреби шрифт і Linux helper; native fallback залишає старий editor у binary. Очікування — assets порядку сотень KB, додатковий native код може бути від сотень KB до кількох MB; **оцінка, не вимірювання**. Системні WebView2/WKWebView/WebKitGTK не вбудовуються цілком у plugin. Найбільший ризик — RAM/процеси при багатьох UDF, а не розмір HTML. На етапі 1 зафіксувати native vs hybrid raw bundle/ZIP size, cold/warm open time, working set host + browser children, UI CPU з відкритими/закритими editors; прийнятні пороги затвердити на вимірюванні. Build size report не підміняти обіцянкою «+190 KB».

Інсталятор Windows може пропонувати офіційний Evergreen offline/online installer; ZIP повідомляє runtime prerequisite і native fallback, без автоматичного мережевого download із плагіна. Linux stable не інсталює WebKit; experimental package описує точний runtime комплект після виправлення soname. Не ставити VST3 поверх завантаженого host файлу, не закривати незбережені користувацькі проєкти. Offline render/headless processor не створює browser і не потребує browser readiness.

## 8. Черга незалежних завдань для майбутніх агентів

Кожний рядок — приблизно 0.5–3 дні сфокусованої роботи, не обіцянка календарного терміну. Це розподіл для подальшої реалізації; під час цього дослідження підлеглі агенти не запускались. Зміни спільних файлів виконує один інтегратор; outputs інших — нові модулі/чіткі interfaces. `publish/` поза всіма задачами.

| № / задача | Залежність | Власні файли | Acceptance / паралельність |
|---|---|---|---|
| T0 API freeze та capabilities (0.5–1д) | Завершення поточного engine агента | Нові `docs/WebView_contract_v1.md`, `web-ui/schema/*`, `fixtures/*`; читання processor | Звірені IDs/ranges/effective values/міграції, diffusion/Ring/scale рішення. Жодних одночасних edits `src/`/`plugin/`. |
| T1 Win/mac shell spike (1–2д) | T0 базовий contract | Нові `plugin/VerbWebEditor.h/.cpp`, `WebResources.*`; інтегратор змінює factory/CMake | Offline resource/gesture/ready/fallback, UDF, DPI/focus/close/8 instances. Після первинного interface може паралельно з T2/T3. |
| T2 Frontend extraction + mock (2–3д) | T0 | `web-ui/src/app`, `bridge/mock`, `styles`, components, package/lockfile | Вигляд v6, 4 polish, однакова gesture логіка, mock fixtures/errors, browser QA; не редагує C++ або viewport/panels іншого агента. |
| T3 C++ command model/undo (2–3д) | T0 | Нові `plugin/EditorSession.*`, `SceneCommands.*`, `tests/EditorCommandTest.cpp` | Validations/locks/metadata/per-band edits/transactions/revision/dedup. Native hooks wiring — інтегратор після completion. Паралельно T1/T2. |
| T4 Relays + bridge transport (1–2д) | T1,T3 | Нові `plugin/EditorBridge.*`, `WebParameterBindings.*`; `web-ui/src/bridge/juce.js/parameters.js` | Host gestures/automation/initialisation/learn/destruction, no-op/error/resync. Один власник цих bridge файлів. |
| T5 Main/context/Sources/Mix/Output (2–3д) | T2,T4 contract | `web-ui/src/pages/main.js`, відповідні panels | Усі чинні parameters/configs,32 sources,actual buses,mode semantics,unsupported controls. Паралельно T6/T7 без правок shared store/components. |
| T6 Viewport/Edit/outliner/materials (2–3д) | T2,T3 contract | `web-ui/src/viewport/*`, `pages/edit-room.js`, edit panels | Plan/elevations/projected3D, mixed groups, overlap picking, local openings, heights/undo/locked corners. C++ додаткові команди — через власника T3. |
| T7 Telemetry/Energy/Decay (2–3д) | T3/T4 | Нові `plugin/EnergyAnalysisService.*`, `EditorTelemetry.*`; `web-ui/src/panels/analysis.js/decay.js`, energy layer окремим файлом | Worker cancellation/stale/binary result,7-band curves,progress,нема audio-thread UI. Перенесення `EnergyMapView` на service — інтегратор, не цей агент паралельно. |
| T8 Library/dialog/export/A-B/preferences (2–3д; за потреби два послідовні пакети) | T3,T4 | Нові `RoomLibraryController.*`, `EditorFileActions.*`, `ComparisonState.*`, `EditorPreferences.*` | FileChooser safety,preset1/2,Keep I/O,session A/B,safe export snapshot,theme persistence. Не міняє `RoomLibrary.h` presets. |
| T9 Tests і factory/native wiring (1–3д) | T3–T8 поступово | **Інтегратор:** `PluginProcessor.*`, `PluginEditor.*`, `EnergyMapView.*`, `PluginTest.cpp`, `Stage2Test.h` | Старі hooks діють, cast failures явні,нова bridge/browser/harness coverage,impulse+reload. Тільки після звільнення файлів engine агентом. |
| T10 CMake/CI/package (1–2д) | T1 resource interface; фінал після T9 | **Build інтегратор:** root/plugin CMake, `.github/workflows/vst3.yml`, `ci/*`, build/packaging docs | Windows новий job,mac Universal,дві Linux конфігурації,frontend assets/runtime-only tests,sign/checksum/dependency reports. Без edits frontend/C++ під час інтеграції. |
| T11 Linux backend hardening (1–3д + QA) | T1, окреме погодження scope | Ізольований JUCE patch або upstream version pin, Linux harness/tests/docs | Відсутні dev symlinks/WebKit/helper execute permission → native fallback; runtime-only з libs працює. Не змішувати з T10 правками vendor/CMake одночасно. |
| T12 Host/performance release audit (1–3д) | T9,T10; Linux web після T11 | Лише QA reports/docs, fixes через відповідних власників | REAPER/Live matrix,scan/render/save/reopen,DPI/keys,1/8/32instances,cold-start/RAM,offline clean install; рішення default WEB/AUTO. |

NEW DSP (Binaural/AmbiX/FDN/directional/modal/velvet/predelay/width/freeze/duck/instrument models), persistent Ring count і automatable Size — окремий backlog із власними DSP/compatibility specs та impulse tests. T5 не має самостійно створювати engine parameters, щоб «оживити» макет.

## 9. Питання власнику з рекомендованими відповідями

| Питання | Рекомендована відповідь |
|---|---|
| Чи Linux зберігає native? | **Так, типовий stable редактор і довгостроковий fallback; WebView лише opt-in experimental до чистого runtime/host QA.** |
| Чи всі NEW контролі мають запрацювати разом із новим UI? | **Ні; спершу parity наявного рушія + поточний Physical/Creative.** Неактивні можливості приховати або явно позначити unavailable. |
| Які перші release hosts? | Windows REAPER як обов'язковий; macOS REAPER + Ableton Live за доступністю; Linux REAPER X11/XWayland native. Решта — поступово, без обіцянки перевірки Logic VST3. |
| Що робити з diffusion Physical розбіжністю? | Завершити один engine contract з поточним агентом. До зміни DSP відображати чинне реальне effective value; не симулювати material-derived lock у JS. |
| Чи Size є automation macro? | Спочатку одноразове коректне Scale room command; continuous Size відкласти до baseline/geometry/automation специфікації. |
| Ring count має бути незалежним від host? | Перший реліз показує actual host-derived count; незалежний count — окреме сумісне OutputCfg розширення. |
| Що порівнює A/B і чи зберігаються слоти? | Повний акустичний стан у C++ session-only; themes/navigation поза A/B; persistence слотів додати пізніше. |
| Plain JS чи framework, які розміри UI? | Plain modules + малий pinned bundler. Default 1280×800, підтримати нинішній мінімум 900×600 адаптивним scroll/dock collapse; не підвищувати minimum мовчки. |
| Чи розширюємо `.txt`/`.verbroom` заради colours/minDist? | UI-міграція сама формат не змінює; colours спочатку local preferences, minDist read-only. Якщо потрібен перенос між користувачами — окремий backward-compatible форматний пакет із reload tests. |

## 10. Що дослідження не підтверджує

Жодної збірки, host run, browser запуску, інсталяції, імпульсу або автоматичних тестів цього плану не виконано. Існуючі REPORT/QA — звіти макета, не нова перевірка плагіна. Не виміряні performance/RAM/binary delta; не перевірені runtime на машині користувача, фактичний focus у REAPER, WKWebView на macOS11, поведінка WebKitGTK у чистій Linux системі та завершеність паралельного engine коду. Локальні API назви/прапорці й наведені межі перевірені за vendored source; зовнішні host reports — ризики для перевірки, не встановлені дефекти цієї збірки. Перший наступний крок реалізації — T0, потім короткий T1 go/no-go.
