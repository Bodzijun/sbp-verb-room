Бодя, це опис збірки й роботи оболонки WebView-редактора (завдання **T1** з [плану](WebView_UI_plan_uk.md), розділи 2, 3 «Вбудований frontend», 7, 8). Оболонка — окремий опційний редактор у VST3 для Windows: сторінка зі `plugin/webshell/`, вбудована в плагін, показується в WebView2. Звичайний (native) редактор залишається типовим і працює так само, як раніше. Справжнього мосту (параметри, команди, сцени) ще немає — це T4.

# WebView-редактор: збірка, перемикання, відмови

## 1. Що це і чого це не робить

- `plugin/VerbWebEditor.h/.cpp` — редактор `VerbWebEditor` (оболонка), `plugin/WebResources.h/.cpp` — вбудовані файли й провайдер ресурсів, `plugin/WebRuntimeProbe.cpp` — перевірка наявності WebView2 Runtime, `plugin/webshell/` — сторінка (HTML/JS/CSS без мережі; `host-keys.js` — передача клавіш хосту, §11), `plugin/GenerateEmbedded.cmake` — генератор вбудованих байтів.
- Оболонка доводить механіку: офлайн-ресурси з власного origin, рукостискання `ready`, round trip `ping`, повідомлення про розмір вікна, відмова на native. Вона **не** змінює звук, стан, параметри чи пресети.
- `src/` (DSP), `web-ui/`, `ui-mockup/`, `publish/`, CI не змінювалися. З боку `PluginProcessor.cpp` змінено лише `createEditor()` (див. §4).

## 2. Збірка

### Опції CMake (у `plugin/CMakeLists.txt`)

| Опція | Типово | Значення |
|---|---|---|
| `VERB_WEB_UI` | `ON` | Будувати код WebView-редактора в `VerbPlugin` (лише Windows). `OFF` — збірка така сама, як до T1: без нових файлів, залежностей, макросів; `JUCE_WEB_BROWSER=0`, як було. |
| `VERB_WEBVIEW2_SDK_DIR` | `third_party/webview2` | Каталог, у якому лежить розпакований пакет `Microsoft.Web.WebView2.<версія>`. |
| `VERB_WEB_DIR` | `plugin/webshell` | Каталог, який вбудовується як веб-інтерфейс. Це єдине, що змінить наступне завдання (§8). |

Якщо `VERB_WEB_UI=ON`, але SDK не знайдено, CMake друкує **WARNING** з потрібним шляхом і будує плагін без веб-редактора (без помилки). Тест `web_shell` у такому разі не створюється.

`COPY_PLUGIN_AFTER_BUILD` залишається `FALSE`: збірка нічого не копіює за межі `build-msvc`.

### Залежність: WebView2 SDK (NuGet) — що саме і як отримано

Потрібен пакет **Microsoft.Web.WebView2 1.0.3485.44** (саме цю версію підказує `FindWebView2.cmake` у JUCE). Нічого не встановлюється в систему; пакет лежить у проєкті, `nuget.exe` і Visual Studio не потрібні. Фактично виконано (PowerShell):

```powershell
cd E:\Claude\verb\third_party\webview2
Invoke-WebRequest -UseBasicParsing -OutFile wv2.zip `
  -Uri https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.3485.44/microsoft.web.webview2.1.0.3485.44.nupkg
Expand-Archive wv2.zip -DestinationPath Microsoft.Web.WebView2.1.0.3485.44
Remove-Item wv2.zip
```

SHA-256 завантаженого `.nupkg`: `BC09150B179246AC90189649B13BE8E6B11B3AC200E817E18DF106E1F3CF489E` (8 912 787 байтів). Далі `JUCE_WEBVIEW2_PACKAGE_LOCATION` (задається у `plugin/CMakeLists.txt`) вказує на `third_party/webview2`, а JUCE бере `build/native/include/WebView2.h` і **статичний** `build/native/x64/WebView2LoaderStatic.lib` (`NEEDS_WEBVIEW2 TRUE`, `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1`). Тому `WebView2Loader.dll` не постачається. Ліцензія пакета — `LICENSE.txt`/`NOTICE.txt` в його корені; їх треба додати до пакета, коли оболонка вийде за межі розробки.

**Runtime — окрема вимога.** Статичний loader не замінює **WebView2 Runtime** (Evergreen). На цій машині встановлено 157.0.4341.42. На чистій машині без нього плагін не ламається: відкривається native редактор (§5). Офлайн-інсталятор Runtime — окремо від VST3 (див. розділ 7 плану).

### Команди

```powershell
cd E:\Claude\verb
.\out\tail-fix\msvc.cmd cmake -S . -B build-msvc            # перконфігурація (ON — типово)
.\out\tail-fix\msvc.cmd cmake --build build-msvc --config Release -j 6
ctest --test-dir build-msvc -C Release -R "^(spatial_editor|editor_commands|web_shell)$"   # ctest напряму: обгортка ламає "|"
.\out\tail-fix\msvc.cmd cmake -S . -B build-nowebui -DVERB_WEB_UI=OFF -DVERB_BUILD_PLUGIN=ON  # збірка без веб-редактора
```

Тестові виконувані файли `VerbPluginTest`, `VerbPluginStress`, `VerbEditorCommandTest` залишаються без браузера (`JUCE_WEB_BROWSER=0`, без `VERB_WEB_UI_ENABLED`): їхній `createEditor()` — це завжди native `VerbEditor`, тому старі перевірки не пропускаються тихо (план, «Збереження тестів без тихого пропуску»). Веб-код тестує окремий `VerbWebShellTest` (CTest `web_shell`).

## 3. Перемикач під час роботи (runtime)

`createEditor()` обирає редактор так (перший збіг):

1. змінна середовища **`VERB_UI=web`** → веб-редактор; **`VERB_UI=native`** (або будь-яке інше непорожнє значення) → native;
2. збережена локальна настройка: файл `%LOCALAPPDATA%\SBP\VerbRoom\editor.ini` з рядком `ui=web` (або `ui=native`);
3. типово — **native**.

Як побачити оболонку в хості: закрити хост, у середовищі, з якого він стартує, виставити `VERB_UI=web` (наприклад `set VERB_UI=web` у `cmd` і запустити `reaper.exe` з нього) **або** створити `editor.ini` з одним рядком `ui=web` і відкрити плагін. Повернутися до класичного вікна: видалити `editor.ini` / написати `ui=native` / прибрати змінну; крім того, кнопка **«Use classic UI»** на самій сторінці запише `ui=native` і замінить вікно на native. Якщо змінна `VERB_UI` задана, вона важливіша за файл.

Редактор вибирається при відкритті вікна; відкрите вікно не міняється під час перемикання настройки.

## 4. Як влаштовано вибір і підміна

- `VerbProcessor::createEditor()` (у збірці з `VERB_WEB_UI_ENABLED`) викликає `verbweb::createEditorFor()`. Якщо обрано native, або перевірка перед створенням провалилася (немає Runtime, не створюється/не пишеться папка даних), повертається **той самий `new VerbEditor(*this)`, що й раніше** — без обгортки. Причину записано в `verbweb::diagnosticsLine()`.
- Якщо обрано веб і перевірка пройшла, повертається `VerbWebEditor`: розмір за замовчуванням **1280×800**, мінімум **900×600**, максимум 2600×1800 (як у native), зі зміною розміру. Браузер створюється асинхронно; `createEditor()` не чекає на нього (вимірювання в §9).
- Після відмови (§5) всередині `VerbWebEditor` створюється справжній `VerbEditor` як дочірній компонент (його власний кутик зміни розміру вимкнено — розмір задає зовнішнє вікно). Документ і процесор не залежать від того, який редактор показано.

## 5. Правила відмови (fallback)

| Ситуація | Що відбувається |
|---|---|
| WebView2 Runtime відсутній (`GetAvailableCoreWebView2BrowserVersionString` не вдалося) | `createEditor()` одразу повертає native; причина — у діагностиці. Жодного мовчазного переходу на IE: JUCE-варіант «IE» не використовується, бекенд задається явно `webview2`. |
| Папку даних користувача (UDF) не можна створити/записати | native, причина «user-data folder …». |
| Сторінка не надіслала `ready` протягом **10 с** після того, як вікно стало видимим | вікно замінюється на native (всередині того ж вікна), причина в діагностиці. Відлік іде лише поки вікно видиме; час не блокує потік повідомлень (таймер 200 мс). |
| Сторінку не вдалося завантажити (`pageLoadHadNetworkError`) | під час завантаження — native; на відкритій сторінці — повідомлення (нижче). |
| Браузер «завис»/процес браузера впав, поки вікно відкрите | JUCE 8.0.15 не виставляє подію `ProcessFailed` WebView2, тому застосовано **heartbeat**: сторінка кожні 2 с викликає `ping`; якщо видиме вікно **15 с** не бачило жодного `ping`, веб-вигляд прибирається й показується звичайне повідомлення з кнопкою **«Use classic UI»** (перехід на native лише для цього вікна, без запису в настройки). Це не заміна події `ProcessFailed`, а побічна ознака; обмеження — у §10. |
| Користувач натиснув «Use classic UI» на сторінці | `ui=native` записується в `editor.ini`, вікно замінюється на native. |

Заміна editor-у ніколи не виконується всередині виклику браузера: усе робиться з таймера або через `MessageManager::callAsync` із `SafePointer`. Для звуку нічого з цього не має значення: аудіопотік веб-редактор не торкається.

## 6. Ресурси, origin, безпека

- Сторінка відкривається за `WebBrowserComponent::getResourceProviderRoot()` (на Windows `https://juce.backend/`) — це локальна видача JUCE, а не HTTP-сервер, `file://` не застосовується. Мережі немає: провайдер повертає лише вбудовані файли.
- Провайдер **узагальнений**: `WebResources.cpp` віддає будь-який вбудований файл за шляхом, MIME за розширенням (html, js, mjs, css, json, svg, png, jpg, gif, webp, ico, woff, woff2, ttf, otf, wasm, txt; інше — `application/octet-stream`). Шлях нормалізується (`?`/`#` відкидаються, `%`-коди розкодовуються, `..`, `\`, `:`, порожні сегменти, NUL — відмова). JUCE API не вміє повертати код 404, тому на відсутній шлях провайдер віддає тіло `404 Not Found` як `text/plain` (порожня відповідь пустила б запит у мережу). Перевірено тестом.
- Навігація: `pageAboutToLoad` пропускає лише `about:blank` і URL, що починається з `https://juce.backend/` (схожий хост `https://juce.backend.evil…/` не проходить); нові вікна відхиляються. Лічильник відмов — у тестовій статистиці.
- На сторінці є `Content-Security-Policy` у `<meta>` (`default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'`). Скрипти JUCE-інтеграції впроваджуються самим WebView2 і політикою не блокуються (у тесті міст працює).
- Сторінка бере `index.js` та `check_native_interop.js` з `modules/juce_gui_extra/native/javascript/` без змін (`plugin/webshell/juce/`, ліцензійний заголовок у файлах збережено). Назви: нативні функції `ready`, `ping`, `useClassicUi`, `hostKey` (§11); події C++ → JS `verb.hello`, `verb.pingRequest`; JS → C++ `verb.pingDone`, `verb.pageSize`. Привʼязка до `evaluateJavascript` із підстановкою даних не використовується.

### Рукостискання `ready` (мінімум T1)

Сторінка реєструє слухачів, потім викликає `ready({pageVersion, supportedSchemaVersions})`. C++ відповідає `{ok:true}` і подією `verb.hello` з полями `schemaVersion`, `pluginVersion`, `revision` (рядок із `getRevision()`, десяткове uint64), `capabilities` (заглушка: `bridge:"t1-shell"`, `commands:false`, `parameters:false`), `heartbeatMs`. Повторний `ready` (наприклад, після прихованого/показаного вікна, коли JUCE вивантажує сторінку) обробляється так само. Це не контракт `hello/snapshot` v1 — він з'явиться в T4.

## 7. Папка даних користувача (UDF)

`%LOCALAPPDATA%\SBP\VerbRoom\WebView\<pid хоста>\<токен>` — не поряд із бінарником плагіна й не roaming.

- **Токен** привʼязаний до екземпляра процесора на час життя хоста: повторне відкриття редактора того самого екземпляра використовує ту саму папку; різні екземпляри й різні хости мають різні папки (план, розділ 2; спільна папка не вводилась).
- Поки редактор використовує папку, в ній тримається відкритий без спільного доступу файл `verb.lock`. Прибирання застарілих папок (фоновий потік, не частіше разу на хвилину, при створенні редактора) видаляє лише папки, де `verb.lock` вільний (немає живого редактора в жодному хості), яких немає серед поточних процесора цього процесу й до яких не торкались ≥ 5 хв. Помилки видалення ігноруються (браузерні процеси можуть ще завершуватись) — повтор при наступному запуску.
- Для тестів можна підмінити корінь і файл настройки (`verbweb::settings().udfRootOverride/prefFileOverride`).

## 8. Як наступне завдання замінить оболонку на зібраний `web-ui/`

1. Зібрати `web-ui` у каталог (наприклад `build/web-ui-dist`) з `index.html` у корені; в ньому має бути `juce/index.js` (або власна копія хелпера) — шляхи довільні.
2. Налаштувати `-DVERB_WEB_DIR=<каталог>`: `GenerateEmbedded.cmake` вбудує **весь каталог** (рекурсивно; без `.md`, `.map`, `.cmake`, прихованих файлів), CMake перегенерує файл при зміні будь-якого вхідного файлу. Код C++ не змінюється: провайдер віддає шляхи й MIME універсально. Колізії імен у різних підкаталогах допустимі (на відміну від `juce_add_binary_data`).
3. Прибрати/замінити `ready`/`ping` на міст із T4 у `VerbWebEditor.cpp::createBrowser()`; тест `web_shell` перевіряє вбудовані файли за `plugin/webshell/` — його перевірку «байти = файли на диску» слід перенацілити на новий каталог.
4. Обмеження: файли в таблиці — це статичні масиви; для великих ресурсів (десятки МБ) краще інший формат (`juce_add_binary_data` з маніфестом). Для оболонки й типового `web-ui` ≈ сотні КБ це не проблема.

## 9. Виміри (цей комп'ютер, Windows 11, WebView2 157.0.4341.42, Release)

Заповнено фактичними запусками; детально — у звіті T1.

| Показник | Значення |
|---|---|
| Час від `createEditor()` до `ready`, холодний старт (перший редактор у процесі) | 1517 мс |
| Те саме, наступні відкриття одного процесора (середнє з 5) | 202 мс |
| 4 одночасні екземпляри (4 процесори, 4 UDF) — усі `ready` | 3621 мс |
| 20 циклів відкрити/закрити (з них 5 з очікуванням `ready`) | 3525 мс |
| Розмір `Verb Room.vst3` | 5 897 216 байтів (було 5 621 760 до T1): **+275 456 байтів, +4,9 %** |
| Тест `web_shell` | 71 перевірка; сам exe ≈ 21 с, під `ctest` ≈ 29 с (запускає 8 процесорів і до 4 вікон браузера одночасно) |
| Залежності VST3 (`dumpbin /dependents`) | без `WebView2Loader.dll` (статичний loader), без нових DLL |

Розмір порівнюється з VST3, що лежав у `build-msvc` до змін (зібраний того ж дня без T1); інші зміни паралельних агентів у цей приріст не розділено. Оболонка вбудовує 5 файлів ≈ 27 КБ.

## 9a. Перевірка зібраного VST3 як хоста (опційно)

`VerbWebVst3Smoke` (не входить у «all» і CTest) завантажує **зібраний** `Verb Room.vst3` через VST3-хостинг JUCE, з `VERB_UI=web` відкриває редактор і дивиться на пікселі вікна, доки не зʼявиться вигляд «ready» (темний фон сторінки + смарагдовий статус):

```powershell
.\out	ail-fix\msvc.cmd cmake --build build-msvc --config Release --target VerbWebVst3Smoke
.uild-msvc\plugin\VerbWebVst3Smoke_artefacts\Release\VerbWebVst3Smoke.exe "build-msvc\plugin\VerbPlugin_artefacts\Release\VST3\Verb Room.vst3" web
```

Результат на цій машині: редактор 1280×800, вигляд «ready» за ≈ 0,8 с після показу вікна (фон сторінки 87,7 % пікселів); контроль із `native` — редактор 1160×760, вигляд сторінки не зʼявляється. Це перевірка не в реальному DAW. Smoke створює справжню папку даних у `%LOCALAPPDATA%\SBP\VerbRoom\WebView\<pid>` (прибирається автоматично пізніше або вручну).

## 10. Що не перевірено (ризики на хостах) і ручні перевірки

- **Події `ProcessFailed` WebView2 немає в API JUCE 8.0.15** — падіння процесу браузера виявляється лише через heartbeat (§5). Реальний обрив `msedgewebview2.exe` у хості не відтворювався (лише змодельований збій і «замовкла» сторінка).
- **DPI** (100/125/150/200 %, перенесення між моніторами під час зміни розміру): перевірено тільки масштаб 100 % (CSS-ширина = фізичній).
- **Фокус клавіатури** у REAPER (floating/FX chain/embedded), Tab, повернення фокуса — не перевірялось (план, «Focus, DPI та відомі host-ризики»). Передача клавіш хосту (Space тощо) реалізована (§11): автоматично перевірено лише на stub-вікні хоста, поведінку в REAPER треба перевірити вручну (чеклист у §11).
- Інші хости (Live тощо), відсутність інтернету як окремий сценарій, окремий процес ізоляції хоста — не перевірялись. Тест працює без мережі (сторінка не звертається до неї), але прямо «відключену мережу» не емулювали.
- Багато редакторів (8/32), RAM і CPU браузерних процесів — вимірювались лише для 4 екземплярів як перевірка життєвого циклу, не як профіль.
- macOS/WKWebView — з 0.2.0 код веб-редактора компілюється на macOS (CMake: `APPLE`, без WebView2 SDK); збирається лише в CI, на реальному Mac ще не перевірено. Linux лишається native (за рішенням власника).
- Поведінка при прихованому/показаному вікні перевірена тільки в тесті (`setVisible`), не в хості.
- Редактор у хості, який повертає `createEditor()` повторно поки попередній ще живий (деякі хости): папка даних спільна для процесора й коректно ділиться (lease), у тесті — послідовні/одночасні екземпляри різних процесорів, не двох редакторів одного процесора одночасно.

## 11. Клавіші для хоста (Space, undo, Ctrl+S…)

### Проблема і як це працює в native

У native-редакторі JUCE VST3-wrapper створює вікно редактора як **дочірнє `HWND` від вікна, яке дав хост** (`IPlugView::attached`). Клавіша потрапляє в чергу повідомлень потоку хоста; хук JUCE (`WH_GETMESSAGE`) пропонує її компоненту у фокусі. Якщо жоден `keyPressed` її не взяв, JUCE у `HWNDComponentPeer` робить `PostMessage(GetParent(hwnd), WM_KEYDOWN/UP/SYSKEYDOWN/UP, wParam, lParam)` — у вікно хоста, і хост обробляє її як свою гарячу клавішу (REAPER: Space = play/stop).

З WebView2 у фокусі клавіші потрапляють у вікно браузера (`Chrome_RenderWidgetHostHWND`, його обробляє процес браузера). Потік хоста їх **не бачить**: ні хук JUCE, ні `forwardMessageToParent` не спрацьовують. `AcceleratorKeyPressed` контролера WebView2 у JUCE 8.0.15 недоступний (`webViewController` — приватний член, JUCE не патчимо). Тому сторінка сама віддає клавіші, яких не використала.

### Механізм

1. `plugin/webshell/host-keys.js` — самодостатній ES-модуль (без імпортів): слухає `keydown` (bubble, на `window`, тобто після обробників сторінки) і `keyup` (capture). Для клавіші, яку **сторінка не обробила** і яка **не в текстовому полі**, він викликає нативну функцію `hostKey({phase, vk, code, key, ctrl, shift, alt, meta, repeat})`.
2. C++ (`VerbWebEditor::handleHostKey`, потік повідомлень) перевіряє payload і робить `PostMessage` у **батьківське вікно редактора** — той самий маршрут, яким JUCE віддає необроблену клавішу native-редактора. `wParam` — віртуальний код клавіші; `lParam` будується як у ОС (лічильник повторів, скан-код, extended, контекст Alt, previous-state, transition). Alt-комбінації й F10 йдуть як `WM_SYSKEYDOWN/UP`, решта — `WM_KEYDOWN/UP`. `WM_CHAR` не надсилається.
3. Аудіопотік не задіяний. Виклик після знищення редактора нічого не робить (`SafePointer`; `PostMessage` асинхронний, зворотних викликів немає). Якщо батьківського вікна немає (редактор без хоста-батька), клавіша тихо відкидається.

Запасний варіант «повернути фокус хосту / розповсюдити `KeyPress` по ланцюжку компонентів JUCE» не використано: у веб-редакторі жоден JUCE-компонент не обробляє клавіші, а перенесення фокуса під час натискання ламало б введення.

### Правило: що пересилається

«Сторінка не обробила, і фокус не в текстовому полі → віддати хосту».

| Ситуація | Поведінка |
|---|---|
| Фон сторінки, кнопка після кліку мишею: Space, стрілки, Home/End, літери, Ctrl+Z, Ctrl+S… | пересилається; на сторінці для такої клавіші викликається `preventDefault()` (рівно один ефект — хоста: без прокрутки від Space, без F5/Ctrl+R/Ctrl+P браузера) |
| `input` (крім button-подібних), `textarea`, `select`, `contenteditable`, ролі `textbox/combobox/slider/…`, піддерево з `data-host-keys="off"` | **не** пересилається, друк працює звичайно (у полі Space друкує пробіл) |
| Сфокусована кнопка/посилання/checkbox/radio/`summary` | Space і Enter лишаються елементу (не пересилаються). Після кліку **мишею** кнопка віддає фокус (`blurAfterClick`), тож наступний Space іде хосту. Tab до кнопки + Space активує її |
| Tab, Meta (Win), ContextMenu, введення через IME (`isComposing`, `keyCode 229`) | ніколи не пересилаються |
| Сторінка обробила: `event.defaultPrevented`, `hostKeys.markHandled(event)`, `options.isHandled(event)` або `stopPropagation()` | не пересилається |
| Повтор (`repeat`) | пересилається з бітом previous-state **лише** якщо першу клавішу вже було переслано (як потік повторів native); `forwardRepeats:false` вимикає |
| `keyup` | пересилається тоді й лише тоді, коли відповідний `keydown` був пересланий (незалежно від того, де тепер фокус): хост не бачить «завислих» клавіш |
| Втрата фокуса вікном, `visibilitychange` → hidden, перехід на native, збій веб-вигляду, закриття редактора | для всіх утримуваних клавіш хосту надсилається відпускання |

Чому `keydown` слухається на `window` у фазі bubble, а не capture: питання «чи обробила сторінка» має сенс лише **після** її обробників (`defaultPrevented`), і `preventDefault()` на пересланій клавіші ще встигає зупинити стандартну дію. `keyup` слухається в capture, щоб відпускання не загубилося, якщо сторінка зупинила його поширення. Обмеження: обробник, зареєстрований на `window` **після** `installHostKeys`, спрацює вже після пересилання; такі обробники ставте на `document`/елементи або викликайте `markHandled`.

Перемикач: змінна середовища `VERB_HOST_KEYS=off` вимикає пересилання; у `hello.capabilities.hostKeys` плагін повідомляє сторінці, чи воно ввімкнене.

### Що має містити справжній frontend

1. Скопіювати `host-keys.js` без змін у збірку (будь-який каталог) і підключити:

```js
import { getNativeFunction } from "./juce/index.js";
import { installHostKeys } from "./host-keys.js";
const hostKey = getNativeFunction("hostKey");
const hostKeys = installHostKeys({ send: (payload) => hostKey(payload) });
// після рукостискання: hostKeys.setEnabled(hello.capabilities.hostKeys !== false);
```

2. У власних гарячих клавішах сторінки (undo/redo, Delete тощо) викликати `event.preventDefault()` або `hostKeys.markHandled(event)` — інакше клавіша піде ще й хосту. Обробники ставити на `document`/елементи. Якщо сторінка має панелі, що прокручуються стрілками, вона мусить позначати ці клавіші обробленими.
3. Поля введення — звичайні `input/textarea/contenteditable`: розпізнаються автоматично. Нетекстові віджети, яким потрібні клавіші (власний слайдер, список), — `role="slider"/"listbox"/…` або `data-host-keys="off"`.
4. Нативна функція `hostKey` зареєстрована в C++ (`createBrowser()`). У dev-сервері в звичайному браузері (`window.__JUCE__` немає) передайте `send: () => {}`.
5. Модуль не перевіряє `isTrusted` (тест подає синтетичні події).

### Перевірено автоматично (тест `web_shell`, частини A і D)

- Кодування `lParam` і типу повідомлення (Space, extended, Alt, F10, AltGr); відхилення невалідних payload.
- На живій сторінці в WebView2 всередині stub-вікна хоста (редактор — його дочірнє вікно, як у VST3): Space down/up — рівно по одному `WM_KEYDOWN`/`WM_KEYUP` з правильними VK/scan/бітами; Ctrl+Z (модифікатор і літера, стан модифікаторів у журналі пересилача); Alt+F як `WM_SYSKEYDOWN` з контекстним бітом; повтори стрілки (previous-state, extended); у текстовому полі нічого не пересилається й `preventDefault` не викликається; `keyup` без пересланого `keydown` не йде; `preventDefault`/`markHandled`, Tab, Meta, IME не йдуть; Space на кнопці не йде; після кліку мишею фокус знімається й Space іде хосту; blur вікна відпускає клавішу, blur елемента — ні; перемикач вимикає; після виходу з веб-режиму й після знищення редактора нічого не надсилається; кожному пересланому натисканню відповідає відпускання.
- **Не перевірено автоматично:** реальні фізичні клавіші через ОС → WebView2 (тест подає синтетичні `KeyboardEvent` у сторінку), поведінка REAPER (як він обробляє `WM_KEYDOWN` у батьківському вікні, режим «Send all keyboard input to plug-in», `GetKeyState` модифікаторів з його потоку), інші хости.

### Ручна перевірка в REAPER (для Боді)

Підготовка: `VERB_UI=web` (або `editor.ini` з `ui=web`), VST3 встановлено власноруч, плагін відкрито у вікні FX, транспорт зупинено. На сторінці рядок «Last key sent to host» показує, що саме пішло хосту.

1. **Фон сторінки.** Клікнути порожнє місце → Space: транспорт стартує; ще раз Space — зупиняється. Один раз на натискання (якщо транспорт «блимає» чи Space «не працює» — див. п. 7).
2. **Після кнопки.** Клікнути «Ping» → Space: транспорт перемикається (лічильник Ping не росте).
3. **Текстове поле.** Клікнути в поле «Text field» → набрати текст із пробілами: усе друкується, транспорт **не** реагує, «Last key sent to host» не змінюється. Потім клікнути на фон → Space: транспорт реагує.
4. **Стрілки, Home/End, літери, Ctrl+Z, Ctrl+S** на фоні: працюють як хост-команди (Ctrl+S — збереження проєкту, Ctrl+Z — undo REAPER). Ctrl+Y сторінка «використовує» сама («Ctrl+Y used by the page») — хосту не йде. Утримання Space/стрілки: REAPER має поводитись так само, як із native-редактором.
5. **Tab / Shift+Tab** переходить між кнопками й полем сторінки, хост не реагує.
6. Усе повторити з опцією REAPER **«Send all keyboard input to plug-in»** вимкненою й **увімкненою**; порівняти з native-редактором (`VERB_UI=native`). Окремо: плаваюче вікно FX, FX chain, вбудований (embedded) вигляд.
7. Якщо Space перемикає транспорт **двічі** (ефекту немає), запустити REAPER із `VERB_HOST_KEYS=off`: якщо Space тепер працює один раз — хост бачить клавішу сам, пересилання для цього хоста треба обмежити. Якщо Space не працює взагалі, а «Last key sent to host» змінюється — REAPER у цьому режимі ігнорує повідомлення в батьківське вікно; повідомте, спробуємо інший адресат.


### T9: остаточна таблиця клавіш справжнього редактора (Verb Room web UI)

Справжній редактор вмикає `installHostKeys({ editorOwnsKeys:true, … })` (`web-ui/src/bridge/juce.js`). Ці клавіші **ніколи** не йдуть хосту, навіть коли фокус на фоні сторінки; сторінка або виконує дію, або гасить клавішу. Вимірювано на справжньому parent HWND у власному test host (`web-ui/tests/live-t9-keys.mjs`, WM_KEYDOWN/UP без самих модифікаторів):

| Клавіша | Edit, є виділення | Edit, нічого не виділено | Main | Хост отримує |
|---|---|---|---|---|
| Delete / Backspace | видаляє виділені поверхні й джерела **одним** undo-кроком (хоча б один source лишається) | гаситься | гаситься | 0 |
| Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y | undo / redo сторінки | undo / redo | undo / redo | 0 |
| Ctrl+A | виділяє всі доступні (не приховані, не заблоковані) стіни/зони | те саме | гаситься | 0 |
| Ctrl+X / C / V / D | гаситься (поки не має власної дії) | гаситься | гаситься | 0 |
| Escape | скасовує жест / чернетку, скидає виділення | те саме | скидає виділення | 0 |
| Стрілки | гасяться, коли щось виділено | пересилаються | пересилаються | 0 / 2 |
| V B R W Z C P (Edit) | перемикає інструмент | те саме | — | 0 |
| Space, F-клавіші, інші літери, Home/End… | пересилаються | пересилаються | пересилаються | 2 (down+up) |
| Текстове поле (`input`, `textarea`, `select`, ролі textbox/combobox/slider…) | звичайне редагування (Backspace, Delete, Ctrl+A/X/C/V) | те саме | те саме | 0 |

Самі модифікатори (Ctrl/Shift/Alt) пересилаються окремо як пара down/up — як у native-редакторі; без «літери» вони хосту нічого не роблять. Транспортні клавіші (Space тощо) залишаються хосту. Захист на рівні `host-keys.js` діє незалежно від того, чи встигла сторінка зареєструвати власний обробник (збій рендера не може «відпустити» Delete у DAW).


## 12. T4: справжній frontend і bridge v1.1

Бодя, production default `VERB_WEB_DIR` тепер `${CMAKE_SOURCE_DIR}/web-ui`. Для існуючого build tree cache треба явно оновити:

```powershell
.\out\tail-fix\msvc.cmd cmake -S . -B build-msvc -DVERB_WEB_DIR=E:/Claude/verb/web-ui
.\out\tail-fix\msvc.cmd cmake --build build-msvc --config Release -j 6
ctest --test-dir build-msvc -C Release -R "^(spatial_editor|editor_commands|web_shell|web_bridge|portable_presets)$" --output-on-failure
Push-Location web-ui
npm test
Pop-Location
```

Embed whitelist: index.html, src/, assets/, schema/ (runtime validator). Fixtures/tests/shots/node_modules/docs не вбудовуються. Вхідна URL додає `?bridge=juce&dev=0`; host-keys/ready/heartbeat підключає native bridge. T1 `web_shell` навмисно має окремий незмінений resource table `plugin/webshell`, щоб зберегти кожну стару перевірку. T4 `web_bridge` додатково відкриває саме production page у WebView2 і перевіряє full render, heartbeat, відсутність dev strip та ENGINE RAYS.

`VerbWebBridgeTest` пише live snapshot, JSON message capture, rays fixture і metrics у out/T4. Fixture після перевірки скопійована до web-ui/fixtures/rays.vocal-booth.json. Результати/невиконані ручні перевірки — out/T4/REPORT.uk.md; runtime контракт — розділ 13 docs/WebView_contract_v1.md. `COPY_PLUGIN_AFTER_BUILD` залишається FALSE; жодного встановлення поза build-msvc немає.

### Ручна перевірка в REAPER для T4

1. Залишити чинні незбережені проєкти відкритими. Для перевірки використати окремий тестовий/portable REAPER з VST search path `E:\Claude\verb\build-msvc\plugin\VerbPlugin_artefacts\Release\VST3`. Переконатися, що він завантажує саме цю development DLL. Не підміняти встановлений плагін і не збирати поверх development DLL, поки її тримає цей хост.
2. Запустити цей тестовий REAPER з process env `VERB_UI=web` або з per-user `%LOCALAPPDATA%\SBP\VerbRoom\editor.ini` `ui=web`. Без env/preference — native editor. Приховати/повернути FX, перевірити повний стан і відсутність повторних/залиплих жестів; resize мінімум 900×600, закрити/відкрити й перевірити відновлений розмір.
3. Крутити mix/gain/джерело з page, записати automation у REAPER і відтворити. Положення має приходити назад без дубльованих automation points. Physical блокує creative-only controls; Creative відновлює збережені значення. Diffusion активна в обох режимах.
4. Змінити room height/material/source/output, перевірити Undo/Redo після появи відповідних frontend controls. Зробити host project save/reload і переконатися, що документ відновився. Змінювати built-in rooms лише після підключення dynamic catalog selector із INTEGRATION_REQUESTS: старі чотири demo labels не відповідають runtime indices.
5. Save → native `.verbroom` chooser, потім Load file у чистий instance з Keep I/O off; порівняти setup/mix/output. Повторити Keep I/O on з іншими локальними routes. Cancel кожного діалогу не має змінювати сцену. Export IR → вибрати development output directory; перевірити status, WAV та імпульс. Ці OS dialogs/експорт у REAPER автоматично не виконувались.
6. Energy: Direct/Early/Tail, source/all, height/quality/duration, Analyze/Cancel/progress; перемістити джерело та перевірити stale. Зміна band/contrast не запускає новий worker. Rays показує ENGINE RAYS; source selection оновлює feed. Обмеження static first-order query викладені в контракті; не прирівнювати його до moving actual taps.
7. Theme/material colour — змінити, закрити/відкрити, перевірити persistence. Native switch після додавання frontend кнопки з INTEGRATION_REQUESTS; до того — `editor.ini ui=native`, прибрати явний VERB_UI=web, відкрити редактор знову. Space після кліку на canvas керує транспортом; у текстових полях набирається текст. Тестувати обидва режими REAPER «Send all keyboard input to plug-in»; при дублюванні використати `VERB_HOST_KEYS=off`.
