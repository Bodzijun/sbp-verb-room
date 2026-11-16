Бодя, це контракт v1.2 між C++ плагіном і WebView-фронтендом. Розділи 0–12 зберігають вихідний аудит T0/v1; реалізований T4 і виправлення описано в розділі 13, який має пріоритет щодо статусів, транспорту й persistence. Рішення власника з [файлу рішень](WebView_UI_decisions_uk.md) мають пріоритет над планом. Усі числа й поведінка нижче звірені з кодом і мають посилання `файл:рядок`; те, що не вдалося перевірити без запуску, винесено в розділ 12.

# Контракт WebView UI Verb Room, версія 1.2

Супровід: [JSON Schema](../web-ui/schema/) · [fixtures](../web-ui/fixtures/). Станом на 11.11.2026 (CMake `project(verb VERSION 0.1.15)`, `CMakeLists.txt:2`).

## 0. Межі достовірності

- Зроблено: прочитано всі обов'язкові джерела з брифу. `src/`, `plugin/`, `publish/`, макет, CI і версії **не змінювалися**; збірка, тести й виконувані файли **не запускалися**.
- Фікстури, які залежать від виконання плагіна (значення клітинок Energy, точний вихід `builtinRooms()`), **не отримані**. Малу кімнату й велику залу відновлено вручну за кодом `RoomLibrary.h` (позначено `derived-by-hand`); фікстура legacy взята з реального `plugin/fixtures/stage2/legacy.verbroom`. Позначки походження кожного поля — у `fixtureMeta.valueSources` кожної фікстури.
- Схеми й фікстури перевірено одноразовим скриптом у тимчасовій теці (Python `jsonschema`, Draft 2020-12): 9 схем коректні, 7 фікстур валідні, 14 негативних перевірок відхиляються. Скрипт у проєкт не додавався.
- «Верифіковано» означає «прочитано в коді». Не означає «перевірено запуском».

## 1. Принципи й протокол

1. **C++ авторитетний.** Фронтенд не має власної копії акустичного стану; він тримає лише відображення, вибір, навігацію й короткочасний optimistic drag.
2. **Три канали:** (а) **параметри APVTS** — через JUCE relays/attachments (розділ 2), без власного JSON; (б) **структурний стан** — версіонований JSON із командами (розділ 3); (в) **телеметрія високої частоти** — події й бінарні ресурси (розділ 6).
3. **Envelope** (`web-ui/schema/envelope.schema.json`): `schemaVersion:1`, `sessionId`, `kind` ∈ `hello|snapshot|command|ack|error|patch|event`, `requestId`, `baseRevision` (десятковий uint64 як рядок), `transactionId`, `payload`. Невідома мажорна версія → `error.unsupported`, без часткового застосування. Тестові фікстури додають `x-fixture`; приймач його ігнорує.
4. **Життєвий цикл:** JS реєструє слухачів → надсилає `hello{supportedVersions, ready}` → C++ відповідає `snapshot` (ревізія, `loadSerial`, capabilities, limits, значення параметрів + effective, document, host, load, jobs). До snapshot редагування заборонене. Повторний `hello` у будь-який момент = запит нового snapshot. Для JUCE-інтеграції браузер вивантажується при приховуванні вікна (план §3, п. 8), тому **весь стан відновлюється з `hello`**.
5. **Нові елементи, яких немає в чинному C++** (протокольні пропозиції, не заявлені межі рушія): поле `revision`, command service, `transactionId`, ліміти payload (1 MiB / 16 KiB / 4 KiB view state), код помилки `internal`. Кожен із них позначений у відповідному розділі.

## 2. Параметри (APVTS)

Верифіковано в `makeLayout()` (`plugin/PluginProcessor.cpp:42-78`). **Усього 243 параметри** (32 джерела × 5 + слухач × 4 = 164; +6 глобальних; +4 bool; +64 mute/solo; +5 stage-2 = 243). Повний машинний список — `web-ui/fixtures/parameters.default.json` (схема `parameters.schema.json`); порядок `index` = порядок оголошення, нові параметри лише додаються в кінець (`PluginProcessor.cpp:71`). Усі `ParameterID` мають version hint 1, усі **automatable** (атрибути `withAutomatable` не задано), усі float мають крок `0.001`.

### 2.1 Таблиця

| ID | Тип | Діапазон | Skew | Default | Од. | Режим | Рядок оголошення |
|---|---|---|---|---|---|---|---|
| `s{N}x`, `s{N}y` (N=1…32) | float | −200…200 | лінійний | 0 | m | завжди | `:51-52` |
| `s{N}z` | float | 0…30 | лінійний | 1.5 | m | завжди | `:53` |
| `s{N}az` | float | −180…180 | лінійний | 0 | deg | завжди | `:54` |
| `s{N}g` | float | −60…12 | лінійний | 0 | dB | завжди | `:55` |
| `lx`, `ly`, `lz`, `laz` (слухач; `laz` = Yaw) | float | як x,y,z,az | лінійний | 0/0/1.5/0 | m/deg | завжди | `:49-54` |
| `s{N}mute`, `s{N}solo` | bool | 0/1 | — | 0 | — | завжди | `:67-70` |
| `gain` (Output Gain) | float | −36…12 | лінійний | 0 | dB | завжди | `:57` |
| `early` | float | −60…6 | лінійний | **0** | dB | **лише Creative** | `:58` |
| `diffusion` | float | 0…1 | лінійний | 0.5 | — | **завжди** | `:59` |
| `doppler` | float | 0…2 | лінійний | 1 | — | **лише Creative** | `:60` |
| `tail` | float | −48…12 | лінійний | 0 | dB | **лише Creative** | `:61` |
| `mix` | float | 0…100 | лінійний | 100 | % | завжди | `:62` |
| `spatialDry` | bool | 0/1 | — | **1** (старі стани: 0) | — | завжди | `:63` |
| `soloDirect`, `soloEarly`, `soloTail` | bool | 0/1 | — | 0 | — | завжди | `:64-66` |
| `mode` | choice `["Physical","Creative"]` | 0/1 | — | **0** (старі стани: Creative) | — | селектор | `:72` |
| `absorption` | float | 0.3…3.0 | центр 1 (`setSkewForCentre(1)`) | 1 | — | завжди | `:73-76` |
| `decayLow`, `decayMid`, `decayHigh` | float | 0.3…3.0 | центр 1 | 1 | — | **лише Creative** | `:73-76` |

«Лише Creative» означає: у Physical рушій отримує нейтральне значення, а збережене користувацьке значення **не стирається** і повертається в Creative (`PluginProcessor.cpp:193-198`; `docs/Physical_creative_stage2_uk.md`). Нейтральні значення: `early`=0 dB, `tail`=0 dB, `doppler`=1, `decay*`=1 (поле `physicalValue` дескриптора).

### 2.2 Правила відображення тексту

- Рядок значення робить C++ (`RangedAudioParameter::getText`). Для float із кроком 0.001 стандартний JUCE-текст — `String(v, 3)` (кількість знаків обчислюється з інтервалу: `third_party/JUCE/modules/juce_audio_processors_headless/utilities/juce_AudioParameterFloat.cpp:50-78`); для bool — «On»/«Off» (`juce_AudioParameterBool.cpp:47`); для choice — рядок вибору. Одиниця — окремо (`unit`).
- Чинний нативний редактор перевизначає форматування лише локально: позиції 2 знаки (`PluginEditor.cpp:189`), `mix` — ціле + « %» (`:623`), `tail` — з явним «+» і 1 знаком (`:628`). Для web-UI: JS форматує за `unit`/`decimals` дескриптора під час drag; авторитетний рядок для host-відображення — з C++.
- Назви чотирьох stage-2 множників у хості збігаються з ID (`absorption`, `decayLow`…): `PluginProcessor.cpp:75` передає `id` як `name`. Це лише відображення, ідентичність параметра не змінюється (див. відкрите питання 15).

### 2.3 Ефективні значення: що UI має показувати

Об'єкт `parameters.effective` (схема `parameters.schema.json#/$defs/Effective`) — це те, що рушій реально застосовує, а не збережений стан.

| Ситуація | Що застосовує код | Що показує UI |
|---|---|---|
| Physical і ручка Creative-only (`early`, `tail`, `doppler`, `decay*`) | `e.setEarlyLevel(creative?early:0)`, `setTailLevel`, `setDopplerStrength(… :1)`, `setDecayMultipliers(creative?…:1)` — `PluginProcessor.cpp:194-198` | Збережене значення приглушено + мітка effective (0 dB / 1.0). Не стирати. Показувати «недоступно в Physical», але ручку не ховати |
| `diffusion` у Physical | застосовується **завжди** (`:196`) | Активна ручка в обох режимах. Макет блокує її в Physical — розбіжність (розділ 10, D11) |
| `s{N}z`, `lz` вище стелі | `cur[i][2]=min(max(z,0),zTop)`, `zTop = ceilOpen ? 30 : sceneHeight` (`:167-168`, `:177`, `:184`); точка = `min(max(z+oz,0),zTop)` (`:324`) | Значення параметра лишається, поряд — effective Z і попередження «обмежено висотою кімнати» |
| `s{N}g` ≤ −60 dB | `decibelsToGain(g, -60)` → тиша (`:178`); Energy також виключає такі джерела (`:1164`) | «−∞ / тиша» |
| `s{N}mute`/`solo` | `gate = mute<.5 && (!anySolo || solo≥.5)`: **mute має пріоритет**, наявний хвіст згасає природно (`:174,179-181`) | Mute завжди перемагає Solo |
| `mix` при активному Solo-компоненті | `mixP = solo ? 1 : …` (`:204`) | `mix` позначити «ігнорується (Solo)» |
| `mix` при `spatialDry=1` | `setReverbMix(positioned && !solo ? mixP : 1)` і `wetG=1, dryG=0` (`:208-210`): 0% = лише позиційний прямий тракт, не обхід входу | Підпис «позиційний dry», не звичайний crossfade |
| Solo-компоненти | бітова маска Direct=1/Early=2/Tail=4; LFE проходить лише при Direct-solo або без solo (`:199-202`) | Усі вимкнені = звичайний режим |
| `absorption` | застосовується воркером зі стабілізацією ≥150 ms (`:930-937`), `alpha' = clamp(alpha·abs, 0, 1−tau)` (`docs/Physical_creative_stage2_uk.md`); застосоване значення приватне (`engineAbsorption`, `PluginProcessor.h:221`) | `requested` + булевий `tailCurrent` (`isTailCurrent()` `:815-819`). Окремого getter застосованого значення **немає** (відкрите питання 12) |
| `decay*` обмежені 20 с | `TailDiagnostics.effectiveMultipliers[7]` + `reshapeStatus[7]` ∈ 0…4: 0 застосовано, 1 немає вимірюваного спаду, 2 нижче роздільності 12 bins, 3 обмежено 20 с, 4 відхилений fit (`src/tail.hpp:19-21,44-46`; тексти `plugin/TailDecayView.h:46`). Для незалежних хвостів додатково `sourceEffectiveMultipliers/sourceReshapeStatus` | Запитане множник × effective по смугах, причина обмеження, «mixed limits» якщо джерела розходяться. Смуга без застосування = unity |
| Switch `mode` | Early/Doppler плавно змінюються (20 ms); хвіст перебудовується воркером і переходить crossfade (`stage2` doc) | Показувати «перехід», доки `tailCurrent=false` |
| RT60 target | `startTargetRT60` (воркер, Cancel); стани 0 idle, 1 working, 2 reached, 3 unreachable (найближчий), 4 cancelled/error, 5 invalid (`PluginProcessor.h:125`, `:851`, `:892-894`); ціль = середній RT60 500 Hz–2 kHz, змінює **absorption** (`:891`) | `requested`, `achieved`, фізична оцінка `getRT60Estimate()` — три різні числа. `setTargetRT60` синхронний — не з native callback |

## 3. Структурний стан

Схема: `state.schema.json`. Не змішується з `.verbroom`/`VerbState` (обмін лишається форматом 1/2). Усе, крім позицій/gain/mute/solo/слухача (APVTS), — у `document`.

### 3.1 JSON-форма

| Блок | Поля (деталі в схемі) | Джерело в коді |
|---|---|---|
| `document.scene` | `heightM` 1…30, `tempC`, `humidityPct`, `floorMaterial`, `ceilingMaterial` (null = відкрите небо), `materials[]`, `walls[]`, `floorZones[]`, `ceilingZones[]`, `floorEditor`, `ceilingEditor`, `clutter`, `roughness`, `roughSeed`, `diffraction`, `preserved{sr,lengthS,order,rays,seed,detectRadiusM}`, `home{x,y,z,yawDeg}` | `scene.hpp:199-221` |
| `Material` | `name`, `classId` 0…10, `alpha[7]`, `tau[7]`, `scatterLF`, `scatterHF` (null = успадкування legacy). Смуги 125/250/500/1000/2000/4000/8000 Hz; `alpha+tau≤1`. TL = −10·log10(tau); непрозорий = tau 0, **sentinel 60/99 dB на дроті не передається** | `scene.hpp:34-41,52-53,292-301` |
| `Wall` | `a`, `b`, `zBottomM`, `zTopM` (null = повна висота, `z1≥1e8`), `material`, `editor` | `scene.hpp:155-161` |
| `Zone` | `vertices` 3…64, `heightM` (null = патч, число = плита), `material` (null лише для ceiling = отвір), `editor`. Порядок значущий: пізніша перекриває | `scene.hpp:166-174` |
| `editor` (tag) | `name`, `group`, `hidden`, `locked` — **метаінформація, акустику не змінює** | `scene.hpp:149-153` |
| `Clutter` | `count`, `minLenM`, `maxLenM`, `minHeightM`, `maxHeightM`, `tlMinDb`, `tlMaxDb`, `minDistM`, `seed` | `scene.hpp:190-197` |
| `document.sources[]` | `name`, `format` 0…7, `channels` 1…16, `firstIn` 0…63 (0-based на дроті, UI показує +1), `sum`, `spreadM` 0…20, `pattern` 0…1, `freePoints`, `offsetsM[][3]` (по каналах, локальна система джерела: X уперед, Y ліворуч, Z вгору) | `SourceModel.h:39-57`, `PluginProcessor.cpp:397-415,436-458` |
| `document.output` | `layout` −1…8 (−1 = за треком), `radiusM` 0…1, `micPattern` 0…7. Layout: 0 Mono, 1 ORTF, 2 XY, 3 LCR, 4 Quad, 5 5.1, 6 7.1, 7 7.1.4, 8 Ring. Mic: 0 layout default, 1 Omni, 2 Subcardioid, 3 Cardioid, 4 Supercardioid, 5 Hypercardioid, 6 Figure-8, 7 Human ears | `PluginProcessor.h:27-33`, `layouts.hpp:13-27,101-116` |
| `document.tail` | `baseRays` 8000…384000, `adaptive` | `PluginProcessor.h:35-38`, `:798` |
| `document.editorFlags` | `navLocked`, `outlinerVisible`, `mixerCompact`, `inputMeters`, `keepRouting` (+ новий опційний `layerModes`, розділ 4.2) | `PluginProcessor.cpp:387-391,468-472` |
| `host` | `inputChannels`, `outputChannels`, `renderedChannels`, `effectiveLayout`, `numPoints`, `sampleRate` — лише читання | `PluginProcessor.h:127-144` |
| `load` | `state` 0 idle/1 loading/2 ready/3 error, `status` (текст як у `:1118-1120`), `tailCurrent` | `PluginProcessor.h:72,115` |
| `limits` | `maxWalls` 128, зони 32+32, вершини 3…64, плит 8, джерел 32, точок 64, входів 64, виходів 16, каналів джерела 16, довжина імені 128 + нові ліміти payload | див. 3.4 |
| `capabilities` | розділ 7 | — |

Приймач `output.receivers` (геометрія віртуальних мікрофонів для «show microphone array to scale») окремого getter не має; його можна відтворити чистими функціями `makeReceiverArray` + `applyMicPattern` (`layouts.hpp:75-136`) з `output`, `listener` і кількості відтворюваних каналів (`min(hostCh,16)`, `PluginProcessor.cpp:1028-1032`). Кількість Ring-каналів — **лише фактична від хоста** (рішення №4).

### 3.2 Ревізія

- Поле `revision` (uint64 як рядок) — **нове**, веде command service. Збільшується на кожну прийняту зміну структурного стану, включно з live-сеттерами й завершенням асинхронного завантаження. `loadSerial` (`PluginProcessor.h:82`) збільшується при побудові нового рушія.
- Чинний `sceneSerial` для цього не годиться: він збільшується в `editScene` (`PluginProcessor.cpp:600`), `editSceneMetadata` (`:612`), `setSources` (`:634`), `setOutputCfg` (`:787`), `setTailSettings` (`:803`) і при завершенні завантаження (`:1113`), але **не** в `setSourcePattern` (`:684-690`), `setSourceSpread` (`:692-704`), `setPointOffset` (`:758-768`).
- `editScene` повертає `void` і мовчки відхиляє (`>128` стін, невалідні індекси: `:593-595`), а `sanitizeScene` мовчки видаляє нульові стіни, зайві зони та перетворює 9-ту плиту на патч (`scene.hpp:304-335`). **Command service зобов'язаний** сам валідувати кандидат до sanitize, порівнювати результат і повертати `accepted|noChange` або помилку; `sceneSerial` як підтвердження недостатній.

### 3.3 Команди

Схема: `command.schema.json` (по одному `$defs` на команду, дискримінатор `op`). Усі числа скінченні. Мутації надсилаються з `baseRevision`; жест = `transaction.begin` → мутації з тим самим `transactionId` (останнє значення виграє, дельти рахуються від початкового знімка, не накопичуються) → `commit` або `cancel` (відновлює початок). Заява плану про окремий `transaction.update` **не прийнята**: оновлення — це звичайні мутації з `transactionId`; `room.setBaseMaterial` теж не потрібна — матеріал підлоги/стелі змінюють `surface.*` над `ref{kind:"floor"|"ceiling",index:0}` так само, як нативний `editSurface`.

«Пере-побудова» = `rebuild()` (повний новий рушій у фоні) або `geomDirty` (тихе оновлення сцени з debounce ≥150 ms).

| Команда | Аргументи → валідація | Нативний аналог | Дія в рушії | Undo сьогодні |
|---|---|---|---|---|
| `transaction.begin/commit/cancel` | `label ≤60` | — | — | групує |
| `surface.translate` | `refs[]`, `deltaM[2..3]`, `includeGroups`; відхилити `locked` (група з одним заблокованим учасником не рухається) | `translateSurfaces`, `SceneObjects.h:42-63` | geomDirty | так (`PluginEditor.cpp:977`) |
| `surface.moveVertex` | `ref`, `vertex`, `positionM`, `autoLink`(default true); нульова довжина/самоперетин → `invalid`; locked → `locked` | перетягування вершини, `PluginEditor.cpp:2216-2224` | geomDirty | так (`:2220`) |
| `surface.create` | `kind`, `a,b` або `vertices`, `heightM`, `material`/`preset`; бюджети → `budgetExceeded` | інструменти Add wall / zone, `:2305` | geomDirty | так |
| `surface.delete` | `refs[]`; locked → `locked` | `deleteSelectedWall`, `:1269-1293` | geomDirty | так (`:1273`) |
| `surface.applyPreset` | `refs[]`, `preset` ім'я; клонує матеріал на поверхню | `:300-308`, `ownMaterialFor` `:25-51` | geomDirty | так (`:305`) |
| `surface.setAbsorption` | `refs[]`, `bands[{band 0..6, alpha 0..1}]`; клемп `alpha+tau≤1` (`:1243-1247`) | `absRows` `:327-334` | geomDirty | лише при drag (`:382`) |
| `surface.setTransmission` | `refs[]`, `bands[{band, tau}]` | `tlRows`, `passRow` `:349-374` (TL групи 125–250/500–1k/2k–8k = 2/2/3 смуги, `:359`) | geomDirty | лише при drag (`:384-385`) |
| `surface.setScatter` | `scatterLF`, `scatterHF` (null = успадкувати) | `scRows` `:335-346` | geomDirty | лише при drag (`:383`) |
| `surface.setHeights` | `zBottomM`, `zTopM` (null = повна), `heightM` (null = патч); locked → `locked`; одна поверхня | `wallBotRow/wallTopRow/zoneZRow` `:408-456` | geomDirty | при drag (`:454-456`) |
| `surface.setMetadata` | `name`, `hidden`, `locked` (≤128 символів); **не** `geomDirty` | `editSceneMetadata` `:78,94` | metadata (host dirty: `PluginProcessor.cpp:614`) | так (`:78,94`) |
| `surface.link/unlink` | `groupName` (унікалізується суфіксом, `:943-949`); ≥2 рухомих | `:937-960` | metadata | так (`:950,957`) |
| `room.scale` | `factor 0.05…20` (`PluginProcessor.cpp:725`), `includeObjects`. Нативний UI: 0.25…4 (`PluginEditor.cpp:457`). **Zones не масштабуються** (`:734-746`) | `scaleRoom` | geomDirty + параметри | так (`:462`) |
| `room.setHeight` | 1…30 | `:376-380` | geomDirty | при drag (`:386`) |
| `room.setOpening` | `ref` ceiling/ceilingZone, `open` | `openSky` `:309-325` | geomDirty | так (`:312`) |
| `room.setAir` | `tempC −20…45`, `humidityPct 5…100` (UI; sanitize −60…70 / 0…100, `scene.hpp:272-275`) | `:489-490` | geomDirty | **ні** |
| `room.setClutter` | `count 0…80` (UI; sanitize ≤1000, у рушії ≤128 стін разом), довжина .1…3/.2…5, висота .1…3/.2…4, TL 0…30/0…40, `seed 1…9999` (задає також `roughSeed`, `:497-500`) | `editClutter` | geomDirty | **ні** |
| `room.setRoughness` | 0…1 | `:502-503` | geomDirty | **ні** |
| `room.setDiffraction` | bool | `:504-505` | geomDirty | **ні** |
| `room.clear` | підтвердження — на стороні UI | `clearRoom` `:1295-1311` (лишає sources/listener/air; обнуляє walls/zones/clutter.count/roughness) | geomDirty | так (`:1296`) |
| `source.add/remove` | `remove`: мінімум одне; попередня перевірка каналів/точок. Після remove позиції/gain/mute/solo наступних **зсуваються** (`PluginProcessor.cpp:667-677`) — попередити про індексну автоматизацію | `addSource/removeSource` | rebuild | **ні** |
| `source.setConfig` | `name`, `format`, `channels` (1…16; UI Ring 2…16, `PluginEditor.cpp:223`), `firstIn`, `sum`, `freePoints`; `firstIn+effectiveChannels≤64`, точок ≤64 → `budgetExceeded` | `editSource`, `setSources` | rebuild | **ні** |
| `source.setPattern/setSpread/setPointOffset` | pattern 0…1, spread 0…20, offset ±100 | live сеттери (`:684-704,758-768`) | live, без rebuild | **ні** |
| `output.set` | `layout −1…8`, `radiusM 0…1`, `micPattern 0…7`; при виборі Human ears радіус 0.0875 (`PluginEditor.cpp:663-666`) | `setOutputCfg` | rebuild | **ні** |
| `tail.setQuality` | `baseRays 8000…384000`, `adaptive` | `setTailSettings` | `qualityDirty`, воркер | **ні** |
| `history.undo` | — | `undo` `:1257-1267` | geomDirty | — |
| `history.redo` | **unavailable** (немає в native) | — | — | — |
| `preset.list/load/save`, `ir.export` | розділ 8; C++ тримає FileChooser/AlertWindow | `PluginEditor.cpp:1014-1092` | rebuild/воркер | — |
| `analysis.start/cancel`, `rt60.start/cancel` | розділ 6 | `EnergyMapView`, `startTargetRT60` | воркери | — |
| `telemetry.subscribe` | теми `meters|tail|status|analysis` | — | — | — |
| `session.setViewState`, `preferences.set` | розділи 4, 8 | — | — | — |
| `comparison.capture/activate` | **unavailable** (A/B — нова функція) | — | — | — |

**Групування undo.** Один не-параметричний жест = одна транзакція. Чинний undo — **знімки всієї сцени** (`pushUndo` кладе `getDisplayScene()`, до 60, `PluginEditor.cpp:1252-1255`) у **редакторі**; `undo` підставляє весь знімок (`:1261`). Наслідки, які контракт фіксує: (1) повернення знімка також відкочує *пізніші* зміни повітря/clutter/roughness/diffraction, які самі undo не створюють; (2) undo не охоплює джерела, вихід, якість хвоста, параметри; (3) стек живе в редакторі й губиться при його закритті та при завантаженні кімнати (`:1024`). Для v1 — паритет: undo стек переїжджає в процесор (T3), семантика та сама; розширення охоплення — окреме рішення.

### 3.4 Коди помилок і ресинхронізація

Коди (`common.schema.json#ErrorCode`): `invalid`, `unsupported`, `staleRevision`, `locked`, `budgetExceeded`, `notReady`, `cancelled`, `ioError` (з плану) та **`internal`** (додано). Відповідність: `invalid` — NaN/Infinity, за межами діапазону, нульова стіна, самоперетин, посилання на матеріал, що не існує; `unsupported` — невідома `op`/версія або вимкнена capability; `locked` — `surfacesLocked` (`SceneObjects.h:42-45`); `budgetExceeded` — 128 стін, 32+32 зони, 64 вершини, 8 плит, 32 джерела, 64 точки, 64 входи; `notReady` — `getLoadState()!=2` (напр. `getRoomPresetText` повертає порожній рядок, `PluginProcessor.cpp:1213`); `ioError` — запис/читання файлу.

**Правило ресинхронізації.** Клієнт тримає `lastRevision`. (1) Команда з `baseRevision != поточна` → `error.staleRevision` з `resync:true`; (2) `patch.fromRevision != lastRevision` → відкинути патч; (3) після асинхронного завантаження, відновлення стану хоста, undo або структурної зміни індексів C++ надсилає `event resync.required{reason}` і повний `snapshot`; (4) у разі `resync` клієнт скидає локальну модель, незавершені транзакції C++ скасовує. Індекси поверхонь **нестабільні** після delete/sanitize, тому посилання — `{kind,index}` лише разом з ревізією. Фікстура `error-resync.json` показує послідовність.

**Ліміти payload** (нові, захисні): 1 MiB для документа, 16 KiB для drag-оновлення, 4 KiB для `viewState`, 128 символів для імен (збігається з `setInputRestrictions(128)` у `SceneOutliner.h:129`). Повторні `requestId` ідемпотентні (add/delete/export не виконуються двічі). Частоти: параметри до 60 Hz на активний контрол з гарантованим кінцевим значенням; структурні drag-оновлення до 20 Hz з coalescing і final flush; rebuild-сеттери (`setSources`, `setOutputCfg`) — на commit або debounce 150–250 ms (план §3.2).

## 4. Редакторний стан: де живе зараз і що втрачається

Перевірено за кодом (**не відтворено запуском**). Є **дві різні речі, які в UI називаються «lock/hide»**:

1. **Позначки окремої поверхні** (кнопки `H`/`L` в Outliner) — зберігаються в процесорі.
2. **Маски шарів** Walls/Floors/Ceilings/Plates (цикл shown → locked → hidden) — зберігаються **лише в редакторі**.

| Стан | Де зараз | Закриття/відкриття вікна | Проєкт | `.verbroom` | Рядки |
|---|---|---|---|---|---|
| `locked`/`hidden`/`name`/`group` поверхні (теж Link) | `Scene.editor` у `display` процесора; серіалізується в `sceneText` коментарями `#@verbroom-editor-v1 kind index "name" "group" hidden locked` | **зберігається** (редактор лише читає `getDisplayScene()`) | так (`sceneText`, `PluginProcessor.cpp:385`) | так (`getRoomText()` → `:1218`) | `scene.hpp:150-153,363-373,479-488,670-680`; `PluginEditor.cpp:77-97` |
| Маски шарів `layerMode[4]` | **(виправлено в нативному редакторі)** `Scene.layerModes` у `display` процесора; редактор лише кешує їх (`VerbEditor::syncLayers`) | **зберігається** | так (у `sceneText`) | так (у `sceneText`) | рядок `#@verbroom-editor-v2 layers W F C P` (пишеться, лише коли не всі нулі); `scene.hpp`, `PluginProcessor::get/setLayerMode` |
| Вибір (`surfaceSelection`, `selWalls`, `selKind`, `selected`, `selWall`) | члени редактора | втрачається | ні | ні | `PluginEditor.h:84,146,206,333-335` |
| Згорнуті групи Outliner (`collapsed`) | `SceneOutliner` | втрачається | ні | ні | `SceneOutliner.h:78` |
| Auto-link кутів | **перемикача немає**: спільні кути завжди рухаються разом; множина `dragEnds` — вершини pickable-стін, що збігаються з перетягуваною в межах 1e-4 | — | — | — | `PluginEditor.cpp:2216-2219` |
| Lock pan (`navLocked`), Outliner відкритий, Compact/Normal, input meters, Keep I/O | атомики процесора | зберігається | так | пишеться, але при імпорті пресета лишається **локальне** значення | `PluginProcessor.cpp:387-391,468-472,1297-1298` |
| Стек undo (до 60 знімків) | член редактора | втрачається (і при завантаженні кімнати) | ні | ні | `PluginEditor.h:347`, `.cpp:1024,1252-1255` |
| Вигляд: площина, 2D-центр/масштаб (`vCx,vCy,vScale`), auto-fit, `faceUp`, `viewPsi`, камера 3D (`camYaw,camPitch,camZoom`) | члени редактора | втрачається | ні | ні | `PluginEditor.h:123-126,210,248-249` |
| Energy: опції, результат, stale, прогрес | `EnergyMapView` усередині редактора; деструктор чекає воркера | втрачається (результат і налаштування) | ні | ні | `EnergyMapView.h:33-47`, `.cpp:44` |
| Видимість 7 кривих Decay, вкладка, інструмент, Show clutter, Show mic, target RT60 текст | редактор | втрачається | ні | ні | `TailDecayView.h:109`, `PluginEditor.h:279,289,332` |
| Експорт IR, RT60 target | процесор (`exportState`, `targetState`) | зберігається під час сесії | ні | ні | `PluginProcessor.h:111,125` |

**Висновок щодо повідомлення користувача.** Згідно з кодом, блокування окремих поверхонь **мають** переживати закриття вікна, потрапляють у стан проєкту й у `.verbroom` (існує тест перевірки round trip для `hidden`: `plugin/PluginTest.cpp:441-445`; у `docs/Spatial_editor_0.1.12_uk.md` це описано). Найімовірніше пояснення скарги — **маски шарів** («Walls (locked)» тощо), які справді не зберігаються ніде. Другий, ймовірний, але не відтворений механізм — вікно втрати оновлення: `loadSceneText` копіює текст сцени в запит (`PluginProcessor.cpp:521-531`), а завантажувач після побудови рушія виконує `display = sc` із цього старого тексту (`:1112`); якщо в цей проміжок (повна побудова може тривати секунди, напр. після `setSources`/`setOutputCfg`/`prepareToPlay`) виконано `editSceneMetadata` (`:607-615`), `display` повертається до старого стану, а `sceneText` лишається новим — наступний `editScene` перезапише `sceneText` із `display` й позначка зникне. Не відтворено. **Що перевірити (для виконавця):** (а) відтворити цей сценарій вручну з Outliner-lock і з layer-lock; (б) перевірити версію встановленої збірки (якщо її зібрано до 0.1.12, per-surface lock не зберігався — не перевірялося, `publish/` не відкривався).

### 4.1 Куди має переїхати кожен прапорець (специфікація, не реалізація)

| Прапорець | Володар | Зберігається |
|---|---|---|
| per-surface `locked`/`hidden`/`name`/`group` | процесор, `Scene.editor` (як зараз) | проєкт + `.verbroom` (як зараз, **без змін формату**) |
| маски шарів `layerModes{walls,floors,ceilings,plates}` ∈ 0 shown/1 locked/2 hidden | процесор (project state) | **лише проєкт**, не `.verbroom` (це налаштування вигляду, як Lock pan; при імпорті лишається локальне) |
| вибір, згорнуті групи, камера/2D-вигляд, прапорець `autoLink` | `EditorSession` у процесорі (пам'ять), JS шле `session.setViewState` | **лише сесія**, не зберігається (переживає закриття вікна, не перезавантаження проєкту) |
| undo-стек | процесор (command service) | сесія |
| Energy-налаштування й результат | `EnergyAnalysisService` у процесорі | сесія (результат не зберігається в проєкті) |

### 4.2 Сумісне розширення формату

> **Реалізовано (нативний редактор):** маски шарів зберігаються як `Scene.layerModes` і серіалізуються рядком `#@verbroom-editor-v2 layers W F C P` (0 shown, 1 locked, 2 hidden; рядок відсутній, якщо всі нулі). Парсер 0.1.15 бачить його як звичайний коментар (перевірено збіркою проти `archive/pre-stage2-2026-11-10/src/scene.hpp`). Окремої властивості `editorLayers` у `VerbState` немає: шари їдуть у `sceneText` проєкту й `.verbroom`. Гонку «`display = sc` із застарілого тексту» усунено для перебудов тієї ж кімнати (`VerbProcessor::queueScene`).

1. **Стан проєкту** (`VerbState`): нова необов'язкова властивість `editorLayers` = `"w,f,c,p"` (цілі 0…2). Відсутня або невалідна → `0,0,0,0` (усе видиме й не заблоковане — як зараз). Старі збірки ігнорують невідому властивість `ValueTree`. Додати ключ у цикл локальних ключів `loadRoomPresetText` (`PluginProcessor.cpp:1297`), щоб імпорт чужого `.verbroom` не змінював локальні шари; бажано також не писати його в `.verbroom`.
2. **Формат `.verbroom` і `.txt` не змінюється** (`formatVersion` лишається 2).
3. **Небезпека: простір імен `#@verbroom-editor-v1` закритий для нових `kind`.** Чинний парсер кидає виняток на невідомий `kind` або невалідний індекс (`scene.hpp:479-487`) і на нерозбірливий рядок (`:368-369`) — стара збірка **відхилила б усю сцену**. Будь-які нові персистентні метадані (напр. колір матеріалу, походження пресета) мають іти під **новим префіксом** (`#@verbroom-editor-v2 …`): старий парсер вважає його коментарем (`:374-375`) і ігнорує.
4. **minDist clutter** (`ClutterCfg.minDist`, за замовчуванням 0.5): `parseScene`/`serializeScene` його не читають/не пишуть (`scene.hpp:418-427,649-651`), тому зміна губиться при перезавантаженні. Сумісне розширення: додати **9-й позиційний токен** після `hMax` у рядку `clutter` — старий парсер читає токени послідовно (`:419-427`) і не перевіряє залишок рядка, тож ігнорує зайвий. До реалізації `room.clutterMinDist` = read-only 0.5.
5. **Міграція:** стара сцена/пресет/стан без цих полів завантажується *точно як раніше*: усе розблоковано й видимо (теги відсутні → `SurfaceTag{}`), `editorLayers` відсутнє → нулі. Нових відхилень немає.
6. **Тести, які має додати виконавець (T3/T9):** (а) `createEditor()` → встановити per-surface lock/hide → знищити редактор → створити новий → стан збережено; (б) те саме для `editorLayers` (шар locked → закрити/відкрити → locked); (в) `getStateInformation`/`setStateInformation` round trip для `editorLayers` і tags; (г) `.verbroom` round trip tags (розширити наявний `PluginTest.cpp:441-445` на `locked` і `group`); (д) завантаження legacy `plugin/fixtures/stage2/legacy.verbroom`/`.state` без полів → усе видиме й розблоковане; (е) старий парсер відхиляє `editor-v1 layer …` (негативний тест, доводить, чому потрібен v2); (ж) гонка: `editSceneMetadata` під час очікуваного/виконуваного `rebuild()` не губить позначку; (з) імпорт чужого `.verbroom` не змінює локальні `editorLayers`; (и) 9-й токен `clutter` читається новою збіркою і ігнорується старою.

## 5. Навігація у 3D-вигляді

> **Реалізовано в нативному редакторі** (`plugin/Camera3D.h`, `PluginEditor.cpp`): ліва кнопка — орбіта; середня кнопка або Shift + ліва — pan (поважає Lock pan; орбіта й zoom лишаються); колесо — zoom до курсора, межі 0.25…64 від масштабу «показати все» (верхня межа розширена для «показати вибране»); `F` або кнопка Frame all — усе; `Shift+F` або Frame sel. — вибраний об'єкт/поверхні; подвійний клік на джерелі, слухачі, стіні чи зоні — центр орбіти на ньому, на порожньому місці — Frame all (кут огляду зберігається); `Home` — повне скидання камери. Камера лише в сесії редактора, не зберігається; нова кімната повертає камеру до «показати все». Текст нижче — початкова специфікація.

Чинна поведінка (`PluginEditor.cpp`): проекція перспективна з центром у bbox стін, зон і позицій джерел/слухача: `radius = 0.5·√(Δx²+Δy²+H²)+0.5`, `D = 3.2·radius` (`:2496-2517`); лише **орбіта** (`camYaw`, `camPitch∈[−0.2, 1.5]`, `mouseDrag` `:2666-2671`); колесо змінює `camZoom∈[0.25, 8]` *до центру сцени* (`:2646`); подвійний клік скидає камеру (`camYaw=−0.75, camPitch=0.5, camZoom=1`, `:2660`); початкові значення `PluginEditor.h:210`. **Зсуву (pan) у 3D немає**; нативний pan діє лише в 2D (`navLocked` блокує перетягування лівою/середньою кнопкою, `:2067,2233,2396,2678`).

Контракт (усе нижче — **чистий стан фронтенду**, команд до C++ немає; стан камери шле `session.setViewState`, бо браузер вивантажується при приховуванні вікна):

| Дія | Специфікація |
|---|---|
| Орбіта | як зараз (yaw безмежний, pitch −0.2…1.5), обертання навколо **точки орбіти** (target), не лише bbox-центру |
| Pan | середня кнопка або Shift+перетягування; зсуває target у площині екрана; **поважає Lock pan** (`navLocked`), орбіта й zoom лишаються доступні (`docs/Spatial_editor_0.1.12_uk.md`) |
| Zoom до курсора | колесо зменшує/збільшує відстань і зміщує target так, щоб точка сцени під курсором лишалась на місці; межі масштабу як зараз 0.25…8 (відносно «показати все») |
| Frame all | target = центр bbox, масштаб за формулою `radius` вище |
| Frame selection | target = центр bbox вибраних поверхонь/об'єкта, масштаб за його bbox |
| Подвійний клік | центрувати орбіту на об'єкті/поверхні під курсором; якщо під курсором нічого немає — Frame all (змінює нативну семантику «скинути камеру»: рекомендація, див. питання 14) |
| Підказка | у куті вікна перегляду: рядок із керуванням |

Вхідні дані для камери (bbox, висота) фронтенд бере зі snapshot і значень параметрів; розширений діапазон слайдерів X/Y — лише підказка «bbox стін ± 20 м» (`PluginProcessor.cpp:572-583`), а не межа; розміщення поза контуром дозволене.

## 6. Дані високої частоти

Принцип: **аудіопотік нічого не знає про JSON чи WebView**. Усі вибірки роблять на message thread (таймер), без утримання `engineLock` довше за копію `shared_ptr`.

> **Застереження про `engineLock`.** `processBlock` бере його лише `try-lock` і **обнуляє буфер**, якщо не вдалося (`PluginProcessor.cpp:159-163`). Лок мають також `getTailDiagnostics` (`:810-813`), `isTailCurrent` (`:815-819`), `getEnergyScene` (`:1152`), `targetScene` (`:824`). Кожен виклик із UI — це шанс пропущеного блоку. Тому: кешувати результат, викликати `isTailCurrent`/`getTailDiagnostics` не частіше ~5 Hz, серіалізувати лише після відпускання лока.

| Дані | Джерело | Транспорт, частота, розмір | Потік |
|---|---|---|---|
| Вхідні meters | `inputPeak(ch)`: peak **останнього блоку** до source gain, `inPeaks` перезаписується кожним блоком (`PluginProcessor.cpp:157-158`, `.h:141`) | подія `meters.input`, 20–30 Hz, масив до 64 float (≈0.4 KB JSON), ballistics і peak-hold на фронтенді (нативно: `peak=max(pk, peak·0.88)` на тик 10 Hz, шкала −60…0 dB, `SourceMixerView.h:62-64,99-103`) | message thread |
| **Вихідні meters** | **не існують** | показувати `output.meters` = unavailable, не вигадувати | — |
| Decay/tail | `getTailDiagnostics()`: імутабельний `shared_ptr<const TailDiagnostics>` (`tail.hpp:39-55`): 7×256 `decayDb` + 7×256 `physicalDecayDb`, `rt60`, `physicalRT60`, `reliableSeconds`, `active`, `multipliers`, `effectiveMultipliers`, `reshapeStatus`, per-source масиви, `duration`, `raysUsed`, `rayLimit`, `baseRays`, `shared`, `adaptive`, `needsMoreRays` | подія `diagnostics.tail` **лише при зміні вказівника** і лише якщо підписано тему `tail`; ≤2 Hz; JSON ≈ 45–60 KB (оцінка) при округленні до 0.01 dB; бюджет ≤64 KiB | message thread |
| Energy map | `EnergyMapView`/`analyzeEnergyMap` (`energy.cpp:47-102`) | див. 6.1 | воркер |
| Load/export/RT60 | `getLoadState/getStatus`, `getExportState/getExportMessage`, `getTargetRT60State/Achieved` | `status.load/status.export/status.rt60`, 5–10 Hz лише при зміні | message thread |

### 6.1 Energy

Наявні параметри нативної панелі (**усі мають бути в новому UI**, `docs/WebView_UI_decisions_uk.md`, `ui-mockup/POLISH_TODO.uk.md` п. 5):

| Контроль | Значення в коді | Рядки |
|---|---|---|
| Складові Direct/Early/Tail | бітова маска 1/2/4, за замовчуванням усі три; незалежні від Listen-solo | `EnergyMapView.cpp:34,73` |
| Джерело | −1 = всі чутні, або індекс 0…31; LFE виключено; враховуються gain, Solo/Mute | `:14,85-96`, `PluginProcessor.cpp:1161-1176` |
| Висота зонда | 0.1…30 m, крок 0.1, default 1.2 | `:28` |
| Якість (кількість променів) | 12 000 / 48 000 (default) / 192 000; рушій допускає 1000…384000 | `:18,72`, `energy.cpp:53` |
| Роздільність (довга сторона) | UI: 16, 24 (default), 32, 48, 64, 96; рушій клемпить 8…96 | `:19-20`, `energy.cpp:53` |
| Тривалість інтегрування | UI 1…20 s, default 20; рушій 0.1…20 | `:28`, `energy.cpp:54` |
| Смуга | broadband (середнє по 7) або одна з 7 | `:15-17,144-149` |
| Діапазон кольору | 0.5…120 dB, крок 0.5, default 60; **лише відображення** | `:28` |
| Auto-контраст | 5-й перцентиль освітлених клітинок вибраної смуги → діапазон; без нового трасування | `:151-163` |
| Analyze / Cancel / прогрес | кооперативне скасування; прогрес 0…1 | `:104-126` |
| Stale | сигнатура = `serializeScene(scene)` + опції (`earlyDb`,`tailDb` залежать від режиму: Physical → 0, Creative → збережені) | `:70-83,128-142` |
| Hover | позиція X/Y, рівень dB відносно піка вибраної смуги, «Outside usable height», «No energy detected» | `:223-232,235-243` |

Транспорт результату (контракт): C++ рахує на копії сцени (`getEnergyScene` на message thread; воркер працює з копією); подія `analysis.progress` ≤10 Hz, `analysis.result` з **дескриптором** (`nx,ny,x0,y0,dx,dy,heightM,raysUsed,options,validCells,blob`) і `analysis.stale`. Дані клітинок — **бінарний ресурс**, не JSON і не base64: `blob.fullUrl` = `[маска валідності, біт на клітинку LSB-first, доповнена до 4 байт][float32 LE × 21 на клітинку: direct[7], early[7], tail[7]]`, порядок рядками знизу вгору (`y*nx+x`); `blob.bandUrlTemplate` — проєкція вибраної смуги (`{band}`: `all` або 0…6) = `[маска][float32 енергія на клітинку]`, як `cellEnergy` (`:144-149`). Розмір: максимум 96×96×21×4 ≈ 756 KiB повного, ≈ 36 KiB проєкції; зміна смуги/контрасту **не** запускає трасування. У C++ зберігати кілька імутабельних результатів; одна незавершена вибірка на вигляд. Стара карта лишається зі stale-міткою. Фронтенд рахує hover/auto-contrast із проєкції.

Лише **план (XY на висоті зонда)**: Front/Side — окрема нова функція (`energy.sliceFrontSide`=unavailable). Нативний `EnergyMapView` — частина редактора й губить результат при закритті (розділ 4): винести в `EnergyAnalysisService` процесора (T7).

### 6.2 Діагностика IR/RT60

`getRT60Estimate()` → `{low, mid, high}` із фізичних `physicalRT60`: low = середнє 125–250, mid = середнє 500–2000, high = середнє 4k–8k (`PluginProcessor.cpp:844-848`); до появи діагностики — геометрична оцінка. Це **оцінка моделі, не вимір**; відкриті/незамкнені сцени не мають точного фізичного об'єму — не видавати його за такий.

## 7. Capabilities

Схема `capabilities.schema.json`. C++ повертає **усі** ключі (схема вимагає їх); недоступні мають `available:false`, `status`, `reasonCode` і **показуються з позначкою «недоступно», а не ховаються** (рішення №2). Статуси: `available`, `partial`, `readOnly`, `planned`, `unavailable`.

| Група | Ключ → статус |
|---|---|
| Рушій (є) | `mode.physicalCreative`, `absorption.scale`, `decay.bandMultipliers`, `rt60.target`, `rt60.estimate`, `tail.quality`, `tail.diagnostics`, `energy.map`, `energy.components`, `energy.planSlice`, `ir.export`, `input.meters`, `output.layouts`, `output.micPatterns`, `room.diffraction`, `room.clutter`, `room.roughness`, `sources.freePoints` — **available** |
| Пресети/файли | `presets.verbroom`, `presets.txt`, `presets.keepIO`, `library.user` — available |
| Редагування | `surface.groups`, `surface.perSurfaceLockHide` — available; `history.undo` — **partial** (`scene-snapshot-only`); `room.scaleOnce` — **partial** (`zones-not-scaled`); `colours.material` — **partial** (`local-preferences-only`) |
| Редагування (немає) | `history.redo` (`no-native-redo`) — unavailable; `surface.classEdit` — **readOnly** (рішення: клас лише для читання); `room.clutterMinDist` — readOnly (`not-persisted-in-text-format`) |
| Заплановано | `editor.layerModesPersist`, `editor.viewStateSession` — **planned** (`pending-implementation`, T3) |
| Нові можливості рушія — недоступні | `predelay`, `stereoWidth`, `freeze`, `duck`, `binaural`, `ambix`, `hybridFdn`, `directionalTail`, `lfRoomModes`, `velvetTransition`, `instrumentDirectivity` — `engine-not-implemented`; `size.continuous` — `needs-spec-owner-decision-3`; `output.ringIndependentCount` — `ring-count-follows-host`; `output.meters` — `no-output-meters-in-dsp`; `reflections.paths` — `no-public-path-api`; `energy.sliceFrontSide` — `needs-new-analysis-function`; `ir.exportCancel` — `no-public-api`; `ir.exportProgressCounters` — `message-text-only`; `ab.compare` — `pending-implementation`; `ab.persistInProject` — `owner-decision-5`; `colours.source` — `no-persistence-defined` |

Нічого з «NEW» не повинно оживати JS-формулами: Size не є параметром; Predelay/Width не існують (`source spread ≠ stereo width`, `відстань/c ≠ predelay`).

## 8. Пресети, файли, A/B, локальні налаштування

- **Файли й FileChooser — тільки C++** (`FileChooser::launchAsync`, AlertWindow для імені/перезапису/помилки); JS надсилає смислову дію, не шлях. Бібліотека користувача — `Documents/SBP/Verb Room/Rooms`, фільтр `*.txt;*.verbroom` (`PluginEditor.cpp:990-1002`); у списку — opaque id. Збереження завжди пише `<ім'я>.verbroom`, перезапис вимагає підтвердження, помилка запису — діалог (`:1047-1079`). `getRoomPresetText()` повертає порожній рядок, якщо не `Ready` (`:1213`) → `notReady`.
- **Вбудовані кімнати:** `verbp::builtinRooms()` — 17 (10 простих + 2 авто + 1 chamber + 2 symphony + 2 VSL; `RoomLibrary.h:492-565`); без `setup` вантажаться як `.txt` (`loadSceneText(...,true)`, режим інстансу зберігається), із `setup` — `loadRoomWithSetup`. Не копіювати демонстраційні PRESETS макета.
- **`.txt`** → `loadSceneText`; **`.verbroom`** формат 1/2 → `loadRoomPresetText` (перед зміною стану валідує: формат, наявність Sources/Output, ≤32 джерел, ≤64 точок, ≤128 стін, повний список параметрів; `:1228-1292`). Невідомий формат або неповний payload відхиляється до зміни.
- **Keep I/O:** локальні Sources/Output лишаються, кімната, позиції й mix імпортуються за індексом; **mode, absorption і decay імпортуються разом із кімнатою** (`docs/Physical_creative_stage2_uk.md`). Локальними лишаються `keepRouting, inputMeters, mixerCompact, navLocked, showOutliner` (`:1297-1298`). Не дублювати в JS.
- **Міграції стану:** відсутній `mode` → Creative; відсутні `absorption/decay*` → 1; відсутні `mode` і `early` → `early=−9 dB`; `spatialDry` відсутній → 0 (старий bypass); відсутні mute/solo/soloX → 0 (`PluginProcessor.cpp:473-498`; для пресета формату 1 — `:1273-1280`).
- **Export IR:** `startExportIR(dir)` → один багатоканальний WAV на просторову точку + текст кімнати; стани 0 idle/1 running/2 done/3 error; прогрес лише текстом `"Exporting impulse responses: i / N ..."` (`:1387`) — окремих лічильників **немає**; публічного cancel **немає** (`exportCancel` приватний). Закриття UI не скасовує експорт. Експорт відтворює живе звучання поточного режиму (Early/Tail/diffusion/Doppler/absorption/decay) — навмисна зміна stage 2. Налаштування під час вибору теки не зафіксовані (воркер читає APVTS у момент `:1353`).
- **A/B — нова C++-функція**, не JSON у `localStorage`. Рішення №5: спочатку лише в межах сесії, без збереження в проєкті; повний акустичний стан двох слотів (mode, усі параметри, сцена, routing/output/quality), транзакційно. У v1 `ab.compare=unavailable`.
- **Локальні налаштування (не в проєкті й не в пресеті):** тема, кольори матеріалів (рішення №7: локально; `.txt`/`.verbroom` не змінюються), кольори ролей/палітри Energy, ширина док-панелі (макет 150–310 px; нативна — фіксована, **не верифіковано**), розмір вікна. Зберігати через C++ (наприклад `juce::PropertiesFile` у профілі користувача), не `localStorage`: UDF ефемерний. Колір матеріалу — ключ за `name` і `classId`; **не** класти колір у `Material.alpha/tau/cls`. Пріоритет: явний колір матеріалу > тема > стандартний колір класу (`scene.hpp:61-68`).
- **Розмір вікна:** рішення №6 — за замовчуванням **1280×800**, мінімум **900×600**. Чинний код: `setSize(1160,760)`, `setResizeLimits(900,600,2600,1800)` (`PluginEditor.cpp:65-67`) — розбіжність із рішенням для default (розділ 10, D14).

## 9. Відкриті питання та рекомендовані відповіді

| № | Питання | Рекомендація |
|---|---|---|
| 1 | Маски шарів — у проєкті чи в `.verbroom`? | Рішення власника: **і там, і там**, а також у тексті сцени (`#@verbroom-editor-v2`); завантаження кімнати з бібліотеки або геометричного `.txt` без цього рядка скидає шари до «shown» |
| 2 | Що саме втрачав користувач — per-surface lock чи маску шару? | Відтворити обидва; код свідчить про маску шару (розділ 4) і про можливу гонку `display`/`sceneText` |
| 3 | Піки входів: `inPeaks` — peak лише останнього блоку | Накопичувати max із скиданням при читанні (зміна в `processBlock`, T7), інакше семплування 30 Hz пропускає піки |
| 4 | `diffusion`: engine застосовує в обох режимах, макет блокує | Показувати активною в обох; рішення агента рушія зафіксувати. Не симулювати material-derived lock у JS |
| 5 | Ring: показувати лише фактичну кількість | Read-only `min(hostOutputs,16)`; незалежний count — окремий формат-пакет |
| 6 | `scaleRoom` не масштабує зони й плити | Зробити коректну команду (зони, висоти плит, offsets/spread) у command service; задокументувати як зміну поведінки; `room.scaleOnce` лишається `partial` до цього |
| 7 | Undo — знімки всієї сцени, охоплює не все | v1 = паритет; рішення щодо розширення (air/clutter/джерела/вихід) — окремо |
| 8 | `minDist` не зберігається | 9-й токен `clutter` (4.2, п. 4); до того — read-only 0.5 |
| 9 | `sceneSerial` пропускає live-сеттери | Нова `revision` (3.2) |
| 10 | Host dirty: `editSceneMetadata`/`setTailSettings` викликають `updateHostDisplay(nonParameterStateChanged)` (`:614,807`), а `editScene` — **ні** (`:586-604`), так само `setSources`, `setOutputCfg` | Єдина політика в command service |
| 11 | Експорт: немає cancel, прогрес лише текстом, налаштування не заморожені | Додати лічильники й cancel, заморозити snapshot (T8) |
| 12 | Немає getter застосованої absorption | Додати публічний getter або подію |
| 13 | `adaptive`: початково `true` (`PluginProcessor.h:37`), а при відновленні стану без властивості — `false` (`.cpp:465`) | Підтвердити намір; фікстура legacy має `adaptiveTail="0"` |
| 14 | Подвійний клік у 3D: «скинути камеру» (код) чи «центрувати на об'єкті» (POLISH_TODO п. 6) | Центрувати на об'єкті під курсором, інакше Frame all; кнопка Reset view окремо |
| 15 | Імена stage-2 параметрів у хості = їхні ID | Перейменувати display name («Absorption», «Decay Low»…) окремо; ID не міняти |
| 16 | Колір джерела: формули редактора й смужки мікшера відрізняються | Єдина формула в C++ (пропуск діапазону hue 0.90…0.995 є лише в редакторі) |
| 17 | Де зберігати локальні налаштування | `juce::PropertiesFile` у профілі користувача, версіонований JSON-вміст |
| 18 | Висота слайдерів позицій | Діапазон слайдера = hint з bbox; діапазон APVTS не змінювати |

## 10. Розбіжності між планом, макетом і кодом

Позначення: **П** — план, **М** — макет `ui-mockup/index.html`, **К** — код.

| № | Місце | Розбіжність (цитата) |
|---|---|---|
| D1 | `sceneSerial` | П: «сам `sceneSerial` зараз не покриває всі setters» — **підтверджено**: `setSourcePattern` (`PluginProcessor.cpp:684-690`), `setSourceSpread` (`:692-704`), `setPointOffset` (`:758-768`) не роблять `++sceneSerial`; інкремент є в `:600,612,634,787,803,1113` |
| D2 | Undo | П: «локальний `vector<Scene>` до 60» — підтверджено (`PluginEditor.cpp:1252-1255`). **Додатково (новий висновок):** знімок — уся сцена, тож undo відкочує також air/clutter/roughness/diffraction (3.3) |
| D3 | Lock pan | П (§5): «Додати відсутній Lock pan» — у **макеті** його немає, у **коді** є: `navLockBtn` (`PluginEditor.cpp:69-71`), `navLocked` у стані проєкту (`PluginProcessor.cpp:390,471`), блокує pan у `:2067,2233,2396,2678` |
| D4 | Lock/hide | `POLISH_TODO` п. 6: «Стан lock / hide має жити в процесорі» — per-surface вже живе в процесорі й у `.verbroom` (`scene.hpp:670-680`); не живуть **маски шарів** (`PluginEditor.h:230-231`) |
| D5 | Energy resolution | П (§4): «Energy long side 8–96» — рушій клемпить 8…96 (`energy.cpp:53`), але UI пропонує 16/24/32/48/64/96 (`EnergyMapView.cpp:19`) |
| D6 | TL | П: «Native opaque 60 dB» — так (`PluginEditor.cpp:53,362-365`); рушій/пресети: opaque = 99 (`scene.hpp:53,87`); на дроті — `tau` |
| D7 | TL групи | П: «native 2/2/3» — підтверджено (`PluginEditor.cpp:359`); твердження про «mock 2/3/2» **не перевірялося** |
| D8 | Tail quality | М: `rays:st.output.rays??8192`, діапазон 1024…65536 (`index.html:342,397`); К: 8000…384000, default 48000 (`PluginProcessor.cpp:798`, `.h:36`) |
| D9 | Quality-пресети | М: Draft/Normal/High/Adaptive (`index.html:397`); К: Preview 12 000 / Mix 48 000 / Render 192 000 / Custom + незалежний `adaptive` (`PluginEditor.cpp:556-557`) |
| D10 | Absorption | М: 10…500 % лог (`index.html:114`); К: 0.3…3.0 = 30…300 % (`PluginProcessor.cpp:73-76`) |
| D11 | Diffusion | М: `diffusion:{… creative:true …}` (`index.html:114`) — блок у Physical; К: `e.setEarlyDiffusion(diffusionParam->load())` без `creative?` (`PluginProcessor.cpp:196`), підказка «Active in both modes» (`PluginEditor.cpp:635`) |
| D12 | Size | М: Size 50…160 % (`index.html:114`); К: параметра немає; `scaleRoom` — множник 0.05…20 (`PluginProcessor.cpp:725`), UI 0.25…4 (`PluginEditor.cpp:457`); **виправлено:** кути зон/плит у плані масштабуються завжди, висоти плит і частково-висотних стін — разом із прапорцем «also moves sources, listener, height» (clutter-об'єкти — фізичні розміри, не масштабуються; їхні позиції генеруються в масштабованих стінах) |
| D13 | RT60 target | М: 0.2…20 s; К: розв'язувач шукає absorption у 0.3…3 (`PluginProcessor.cpp:859,877`) → «unreachable» (стан 3); введення лише цифри (`PluginEditor.cpp:603`) |
| D14 | Розмір вікна | Рішення №6: 1280×800; К: `setSize(1160, 760)` (`PluginEditor.cpp:65`), мінімум 900×600 збігається (`:67`), максимум 2600×1800 |
| D15 | Ring-канали | П: «Source Ring 1–16 у C++ (UI 2–16)» — підтверджено: `setSources` клемпить 1…16 (`PluginProcessor.cpp:628`), `chBox` 2…16 (`PluginEditor.cpp:223`) |
| D16 | Вихід Ring | К: `makeReceiverArray(lay, center, radius, min(hostCh, 16))` (`PluginProcessor.cpp:1028`); `OutputCfg` без кількості (`PluginProcessor.h:27-33`) |
| D17 | minDist | П: «parser/serializer clutter не читає/не пише minDist» — підтверджено (`scene.hpp:418-427,649-651`); `sanitize` лише клемпить (`:291`) |
| D18 | Плити | П: «9-та plate не має мовчки стати patch» — підтверджено: `if (z.z >= 0 && ++cnt > kMaxPlates) z.z = -1;` (`scene.hpp:331-335`); велика зала сидить рівно на ліміті 8 (fixture) |
| D19 | Команди | П (§3.2): `transaction.update`, `room.setBaseMaterial` — **не прийняті** (3.3) |
| D20 | Тail level | `rt_engine.hpp:68`: «−60..+12 dB» для рушія, а параметр `tail` має −48…12 (`PluginProcessor.cpp:61`) — вужчий |
| D21 | Назви stage-2 | `p.push_back(… id, id, range, 1.f)` (`PluginProcessor.cpp:75`): назва = ID |
| D22 | Колір джерела | Редактор пропускає hue 0.90…0.995 (`PluginEditor.cpp:56-60`), смужка мікшера — ні (`SourceMixerView.h:91-92`) |
| D23 | `adaptive` | Конструктор/`TailSettings` → `true` (`PluginProcessor.h:37`); відновлення стану без властивості → `false` (`PluginProcessor.cpp:465`) |
| D24 | Кількість параметрів | П: «243» — підтверджено (розділ 2) |
| D25 | Host dirty | `updateHostDisplay(nonParameterStateChanged)` є в `editSceneMetadata` (`:614`) і `setTailSettings` (`:807`), немає в `editScene` (`:586-604`), `setSources` (`:622-637`), `setOutputCfg` (`:780-790`) |
| D26 | Кімната «Vocal booth» | Спостереження (відновлено вручну, потрібен запуск): `box()` рахує `listenerYaw` з початкових рівних позицій джерела й слухача (`RoomLibrary.h:40,52`), які потім перевизначаються (`:497-499`), тож після повороту слухач дивиться від джерела (yaw −90) |

## 11. Фікстури

| Файл | Зміст | Походження |
|---|---|---|
| `parameters.default.json` | 243 дескриптори | код `makeLayout()` |
| `snapshot.default-physical.json` | новий інстанс у Physical: вбудована demo-кімната (`kDefaultScene`, `PluginProcessor.cpp:14-40`), одне джерело | код; ревізії й host — protocol-example |
| `snapshot.legacy-creative.json` | старий стан → Creative; реальні дані `legacy.verbroom`, міграції `mode/absorption/decay*` | legacy-file + міграція вручну |
| `snapshot.small-room.json` | вбудована «Vocal booth (dead)» 2.0×1.6×2.3 m | відновлено вручну (`RoomLibrary.h`) |
| `snapshot.large-hall.json` | «VSL Grand hall – audience»: 32 стереогрупи Sum на 64 входах, 16 стін, 8 плит (ліміт), 10 матеріалів | відновлено вручну (`RoomLibrary.h:277-346,441-490`) |
| `energy-result.json` | progress → result-дескриптор → stale для demo-кімнати (18×24 клітинок) | дескриптор виведено за `energy.cpp:64-67`; значень клітинок **немає** |
| `error-resync.json` | stale → resync → snapshot; `locked`; `invalid` | protocol-example |

## 12. Що не вдалося верифікувати

1. **Нічого не запускалося:** поведінка (втрата lock після перевідкриття, гонка `display`/`sceneText`, пропуск блоку через `engineLock`) — висновки з коду, не з запуску.
2. **Значення клітинок Energy**, піки/hover, точні виходи `builtinRooms()` (мала кімната, велика зала) — відновлено вручну; можливі розбіжності з реальними через `sanitizeScene`, точність серіалізації 6 знаків і квантування APVTS (float32).
3. **Нативний dock** (220+6 px з плану) і `resized()` редактора не перевірялися; ширини док-панелі в контракті немає.
4. **`publish/`, встановлені збірки й версія плагіна в рядку `JucePlugin_VersionString`** не відкривались; `0.1.15` взято з `CMakeLists.txt:2`.
5. **Твердження про групи TL «mock 2/3/2»**, структуру 16 станів макета й `qa-v6.cjs` не звірялися (макет прочитано вибірково — id-елементи, `K`, `FORMATS`, `LAYOUTS`, `MIC_NAMES`, теми).
6. **Рушій:** формули Eyring, `reshapeTailDecay`, `finalizeTail` не перевірялися — довіряю `docs/Physical_creative_stage2_uk.md` і `out/tail-fix/REPORT_UK.md` (звіт про 13/13 CTest не переперевірявся).
7. **Порядок індексів параметрів у хості:** `index` = порядок оголошення; JUCE/VST3 може відображати його інакше — не перевірялося.
8. **Протокольні ліміти** (1 MiB, 16 KiB, 4 KiB) — пропозиції, не заявлені межі рушія.
9. **JUCE WebView API** з плану (`WebSliderRelay` тощо) у цьому завданні не перевірялися повторно.


## 13. T4 / v1.1: реалізований міст (11.11.2026)

`schemaVersion:1` і `capabilities.contractVersion:1` залишаються major-версією; optional `contractMinor:1` оголошує additive розширення. Старі v1 fixtures продовжують валідуватися. `.txt`, `.verbroom` і `VerbState` не змінено. Native editor лишається default. Ранні твердження T0 «не запускалось» стосуються лише того аудиту; поточні результати — `out/T4/REPORT.uk.md`.

### Changelog

- Додано `editor.setFlags {flags:{navLocked?,outlinerVisible?,mixerCompact?,inputMeters?,keepRouting?}}`, `listener.set {xM?,yM?,zM?,yawDeg?}`, `layers.set {layer:0..3,mode:0..2}`. Шари, listener і `minDistM` делегуються чинній T3 реалізації. `editor.setFlags` не undoable, зберігається штатними project-state setters; не є local preferences.
- `room.setClutter.minDistM` тепер дозволений командою; його persistence вже реалізував T3 сумісним дев’ятим токеном. Маски шарів уже зберігає T3 у сумісних `#@verbroom-editor-v2` коментарях. Ранні v1 позначки planned/readOnly для цих можливостей застарілі.
- `history.changed` має optional `undoLabel`/`redoLabel` (≤60 символів); Redo активований лише для web session. Додано `redo` до причин resync, optional `ack.details`, `catalogs.list` і typed reply з повною `materials`, `builtinRooms[{id,name}]`, `userPresets[{id,name,format}]`. `preset.list` повертає ту саму typed event; user id — opaque filename із переліку, а не довільний шлях.
- `preset.load.id` приймає попередній string і integer. Built-in id є індексом поточного runtime каталогу; fixture demo indices до реального каталогу не прирівнюються.
- `snapshot.parameters.texts` містить C++ `getText`; effective додатково має `zM` (обмежені висотою `sNz/lz`) та `absorption.applied` (atomic applied scale; null до першого engine). Старі v1 поля залишено.
- `reflections.query` / `diagnostics.paths` описані нижче. `reflections.paths` оголошено **partial**, із причиною `static-first-order-centre`.

### Транспорт параметрів і стану

`verbMessage(envelope)` є native функцією; відповідь — впорядкований масив envelopes. Unsolicited messages йдуть через native event `verb.message`. Hello спершу чекає фактичного документа; C++ не підставляє фікстуру чи порожню недійсну сцену, поки перша кімната завантажується. JS підписується до hello, очікує перший реальний snapshot.

Для 243 параметрів обрано еквівалент relay/attachment — **окремий пакетний native канал**, а не структурні parameter.set-команди. `verbParameters([{type:"begin",id},{type:"change",id,value:<denormalised>},{type:"end",id}])`; схеми `parameters#GestureAction/GestureBatch`. Один JS turn зберігає порядок жестів в одному виклику; C++ спочатку перевіряє весь пакет, потім викликає APVTS beginChangeGesture/setValueNotifyingHost/endChangeGesture. Це масштабується на 243 параметри без 243 audio-thread listeners, алокацій чи AsyncUpdater-повідомлень із callback. Metadata генерується з фактичного APVTS, а не копіюється з фікстури.

Native event `verb.parameters {values,effective,texts}` (`parameters#Update`) містить авторитетні значення та тексти; automation ніколи не відправляється назад до host. Creative-only зміни з page відхиляються у Physical, збережені значення не стираються. При hide/load відкриті жести завершуються. Meter ticks приблизно 29.4 Hz (timer 34 ms), immutable diagnostics до 1 Hz; незмінні curves/status не повторюються. Нульові input meters надсилаються рідше, audible peaks — до 30 Hz. Worker snapshots не беруть `engineLock` у web poll; у `processBlock` додано лише lock-free atomic peak accumulation, native last-block peaks незмінні.

Після структурного ack C++ віддає canonical full snapshot; patch — допустима v1 можливість, але поточний C++ producer її не потребує. Unsolicited document/revision/load change → resync event + full snapshot. Розрив patch ordering у store автоматично запитує hello. До актуального snapshot commands вимкнені. Idempotency cache обмежений 256 успішними request IDs; replay повертає попередній ack і актуальний snapshot. Сесійний view state переживає закриття редактора через EditorSession, але не project reload.

### Jobs, files, Energy

Save / Load file / IR export використовують C++ async native FileChooser. Ack означає прийняття запиту діалогу; cancel/error приходить окремим error з requestId. Немає browser download, довільних filesystem URL або синхронного native file picker. Built-in/user load використовує штатні processor API та Keep I/O. IR export передає штатні state/message; cancel/counters не оголошені, бо public processor API їх не має. RT60 — start/cancel і штатні status/estimate.

Energy працює на власному worker з immutable `getEnergyScene(...,true)` і наявним `analyzeEnergyMap`; попередній актуальний engine model має бути готовий. Direct/Early/Tail, source index -1/all або 0..31, height, resolution, rays, duration, component mask — штатні поля. Progress ≤10 Hz; cancel cooperative. Full binary — маска LSB-first padded to four bytes, потім LE float32 direct[7],early[7],tail[7] для кожної клітинки. Band projections 0..6 та all використовують options.components; all — середнє семи смуг. Останні два результати зберігаються в bounded resource cache. Старий результат лишається доступним зі stale-позначкою; зміна стану може консервативно інвалідовувати карту. Телеметрія призупиняється при hide; background job продовжує працювати, pending result публікується після повернення. Повторний hello replay-ить останній готовий analysis.result descriptor/stale event і перевидає slow feeds, щоб WebView unload не втратив карту.

### Rays: дані, точність і межі

`reflections.query {sourceIndex,pointIndex?,maxOrder?:0|1,maxPaths?:1..256}` задає spatial point обраного source. `pointIndex` — індекс серед його **audible spatial points**, не глобальний engine point. За замовчуванням 0, maxPaths 128. Зміни позицій/сцени дають update після 150 ms спокою; update також чекає нову modeled geometry publication. Payload обмежено кількістю шляхів (producer використовує максимум 4 точки на підтриманий шлях); `truncated` повідомляє про cap/пропуск.

Відповідь `diagnostics.paths {revision,sceneSerial,sourceIndex,engineOrder:1,receiver:"listener-centre",paths,truncated,stale}`. Path: `pointsM` source→bounces→listener centre, `order`, `surfaces[{kind,index}]`, `lengthM`, геометричний `delayS=lengthM/c(tempC)`, `bandEnergy[7]` для одиничного збудження, `occluded/diffracted/transmitted`. Непрохідні шляхи рушій відкидає, тому producer не вигадує їх як audible paths; occluded у повернених audible paths false. Індекси стін стосуються **modeled scene**, яка включає seeded clutter.

**T9 (сумісні доповнення, schema `event`/`command` лише розширено):** без `pointIndex` запит повертає шляхи **від кожної** фактичної channel point джерела (Sum = одна точка центру; LFE без просторової точки виключено); кожен path має необов'язкові `pointIndex`, `channelIndex`, а відповідь — `pointCount` і `budgetExceeded`. Глобальний cap `maxPaths` лишається (`truncated`). Дифракційний пошук обмежено бюджетом (≈6 мс) і лише зупиняється раніше (`truncated:true`, `stale:true`), математика та знайдені шляхи не змінюються. Усі JSON, що йдуть у web, проходять `finiteJson` (NaN/±Inf → `0`), тому коїнцидентні точки чи приймач поза кімнатою не ламають контракт. `status.rt60.estimate` і `diagnostics.tail.rt60/physicalRT60` використовують **0 для «неможливо виміряти»** (рушій має −1, схема v1 вимагає ≥0; UI показує «—»). `surface.create` має необов'язкове `groupName` (створює/розширює metadata-групу в тому ж undo-кроці; формат пресетів не змінено).

Повторного JS геометричного алгоритму в native mode немає. C++ читає engine modeled scene (roughness/clutter/applied absorption), використовує ті самі `geometry.hpp` функції `mirror`, `crossSurfaceSoft`, `transmitLeg`, `litEdgeAttenuation`, `diffractedPath`, `matAt`, `dirGain` та air-loss. Перший порядок відповідає поточному `ec.erOrder=1`; max delay horizon .75 s. **Це read-only статичний query до центру listener, а не snapshot рухомих actual taps/мікрофонного масиву.** Він не відтворює retarded-time history, glide, Doppler strength або мінімум 2 samples у audio delay. Публічний DiffPath не має середньої вершини трьохкутового обходу: такі обходи пропускаються з `truncated:true`; 1-/2-кутові підтримуються. Щоб прибрати обмеження, потрібен read-only accessor рушія з повним polyline/history; `src/` у T4 не редагувався. Не заявляти цю частину як повне виконання вимоги actual DSP paths.

Store адаптує wire події до вже наявного T5 UI: `catalogs.list` → `telemetry['materials.catalogue']`; `diagnostics.paths` → `telemetry['rays.paths'] {points,delayMs,levels[7],...}`. Levels — dB відносно direct band energy; якщо direct відсутній, використано абсолютну норму одиничного збудження. Native renderer успішно отримав цей feed у live WebView2 тесті.

### Preferences та межі frontend інтеграції

`web-preferences.json` лежить поруч із T1 per-user `editor.ini`, не у plugin directory. Theme, roles, materialColours, layoutChoice та windowWidth/windowHeight зберігаються C++; sessionViewState з EditorSession додається до snapshot. Native switch є через `bridge.useNativeUi()` / `preferences.set {patch:{ui:"native"}}`, editor.ini зберігає вибір; явний `VERB_UI` має пріоритет при наступному відкритті.

Власник frontend має завершити запити в `web-ui/INTEGRATION_REQUESTS.md`: dynamic built-in/user selector, кнопка Native UI, authoritative text display/Undo/Redo menu labels, plugin-specific footer/help замість MOCK текстів. Файли components/pages/styles/viewport/index layout і app.js цей агент не змінював. Наявні запити не прирівнюються до реалізованих кнопок.


## 14. E1 — contract v1.2 (пріоритет над історичними NEW у §§2/7)

Changelog: п'ять E1 capabilities `size.continuous`, `predelay`, `stereoWidth`, `freeze`, `duck` тепер **available**, contractMinor=2, schemaVersion/contractVersion лишаються 1. Назва `size.continuous` збережена для сумісності ключа; її note явно вказує debounced rebuild, а не continuous morph. Деталі DSP та обмежень: [Engine E1](Engine_E1_size_predelay_width_freeze_duck_uk.md).

APVTS додає шість IDs наприкінці (243…248): `size` 50…160 % default100, `predelay` 0…250 ms default0, `stereoWidth` 0…200 % default100, `freeze` bool default0, `duck` 0…24 dB default0, `duckRelease` 50…2000 ms default300. Float step .001, host-automatable; `size` modeClass always, інші creativeOnly з physicalValue 0/100/0/0/300. Старі 243 descriptors не змінені. Нові packets мають 249 values; схеми приймають попередні 243-field packets, додані effective fields необов'язкові для старих fixtures/clients.

`parameters.effective`: `sizePercent` = worker-applied %, `sizeRequested`, `predelayMs`, `stereoWidthPercent`, `widthAvailable` (false для mono), `freeze` bool, `duckDb`, `duckReleaseMs`, `duckReductionDb` (реальна позитивна gain reduction із audio atomic), `lockReason` (Physical explanation / empty). `predelay/width/freeze/duck/release` нейтральні у Physical, stored values в `values` зберігаються. Meter передається через наявний parameter update канал; аудіопотік не будує JSON. Geometry/tail currentness враховує Size, статус Rebuilding вже існує.

`document.scene`, source spread/offsets і APVTS poses лишаються **авторськими**. Main viewport створює лише drawing copy за applied Size; Edit — unscaled із явною приміткою. Diagnostics Energy/rays/clutter використовують ефективну engine scene. Mic physical spacing не масштабується. Native editor не одержав нових елементів.

Persistence: `VerbState` і format2 `.verbroom` зберігають Size/Predelay/Width/Duck/Release як PARAM; Freeze записується/завантажується Off. Missing IDs явно отримують neutral defaults. Keep I/O імпортує нові room controls, лишає Sources/Output локальними. `.txt` не одержує нових команд. Старі format2 parsers ігнорують додаткові PARAM IDs.
