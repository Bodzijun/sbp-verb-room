# Verb Room 0.3.1

Verb Room 0.3.1 is a maintenance and compatibility update on top of 0.3.0. It fixes baked late-tail persistence when an analytic source directivity pattern is fixed, widens accepted host output buses up to 64 channels while keeping unused outputs silent, fixes a worker-thread race in tail currency tracking, adds routing hints and multichannel warnings in the interface, and expands the user manual with signal-flow documentation. Release targets are Windows x64, macOS Universal (Apple silicon and Intel), and Linux x86_64 (classic interface only).

## What is new

### Engine and host routing

- **Baked late tails survive save and reload with a fixed source pattern.** Previously, changing an analytic source's **Directivity** (`pattern`) set `fixedPattern = true` live, whereas reloading a project rebuilt the engine with `fixedPattern = false` and caused `BakeCache` signature checks to reset the table to `Not baked`. `VerbProcessor::run()` now initializes sources with canonical `(double)(float)` pattern values and `fixedPattern = true` via `setSourceDirectivity()`, matching live updates. Both variants of saved caches from `0.3.0` and `0.2.x` (`fixedPattern = true` and saved `fixedPattern = false` tables) continue to restore cleanly; hand-edited `.verbroom` or XML states whose `pattern` differs from `(float)` precision are safely rejected and re-baked.
- **Tail currency thread race fix.** Changed `workerJumpSeen` in `src/rt_engine.cpp` to `std::atomic<uint64_t>` with acquire/release ordering so `Engine::tailIsCurrent()` and the background worker observe timeline jumps consistently without a data race.
- **Wide host output buses up to 64 channels.** `isBusesLayoutSupported()` now accepts host output buses from `1` to `64` channels (`kMaxInputChannels`) while the engine continues rendering up to `16` layout channels (`R`, e.g., `2 / 64 ch` for Stereo or `16 / 64 ch` for 3rd-order AmbiX / Ring on a 64-channel REAPER track with a 32-source VSL setup). Channels `R+1..nOut` are owned by the plug-in and silenced when **Positioned dry** is on, instead of passing dry inputs through unmodified. Source **Mute** and **Solo** also silence the raw dry path (`k → k`) of **Positioned dry** off for covered channels (existing behaviour, now explicitly tested and documented).
- **No change to stereo or 16-channel sound.** Stereo (`2 ch`) and 16-channel (`AmbiX 3rd order`) outputs match `0.3.0` within float rounding (`diff = 0` on Default, `legacy.state (2→2)`, `F6-AmbiX`, and `AzKey+Path`; max sample difference `≤ 7.45×10⁻⁹`, energy difference `≤ 6.26×10⁻⁷ dB`).

### Interface and documentation

- **Sources rack reminder.** A permanent hint above the **Sources** table (`M / S / Gain act on what the plug-in outputs. Channels the plug-in does not output are passed on by the host unchanged.`) and matching column tooltips on `Gain dB`, `M`, and `S` clarify channel ownership. Compact viewport media rules and splitter limits (`170 px`) keep the first mixer row visible on smaller windows (`1100×560`, `1280×720`).
- **Positioned dry off warning in Mix and Settings.** When **Positioned dry** is off and the output is Ambisonics, surround, or multi-channel `Ring` (`> 2` channels), a reactive non-blocking warning explains that raw inputs mix `k → k` and advises keeping **Positioned dry** on (hidden in mono and stereo).
- **Manual signal-flow documentation.** Added **Dry signal and input channels 1/2**, **Multichannel and Ambisonics outputs**, and the signal-flow diagram (`docs/images/manual/signal-flow.png`) to [the English manual](Manual_en.md) and [the Ukrainian manual](Manual_uk.md).

### Presets and compatibility

- Existing projects, states, `.verbroom` (format 1 and 2), `.txt` scenes, and baked caches from `0.3.0` and `0.2.x` continue to load. Only projects that had a fixed-pattern bake saved under `0.3.0` change their canonical bake signature going forward while still accepting saved `0.3.0` / `0.2.x` caches.
- Wide host output support is additive; the first 283 APVTS parameters, `Mod Out 1…16` (indices `283–298`), and the default `±200 m` / `0–30 m` automation range are unchanged.
- Compatibility vs. `0.3.0` (measured on Windows x64 VST3 across 13 scenarios: Default `2→2`, `stage2/legacy.state` `2→2` & `64→2`, `VSL_Grand_Hall_Audience` `64→2`, `VSL_Grand_Hall_Conductor` `64→2`, `F6-Default`, `F6-AmbiX`, `F6-AmbiX3-2`, `F6-AmbiX3-16`, `test azimut`, `AzKey+Path`, `Modulation` route, and wide `64→64` vs `0.3.0` `64→16` AmbiX3): all passed (`max sample diff ≤ 7.45×10⁻⁹`, `|ΔE| ≤ 6.26×10⁻⁷ dB`, `0` energy on channels `17–64`). State/preset save-reload (`AGENTS.md`) on `0.3.1` across 6 scenarios: all passed (`max sample diff ≤ 3.73×10⁻⁹`).

## Known limitations

- **Web editor vs. classic UI.** Azimuth keys, the modulation matrix, Path LFO, and **Automation range / Fit to scene** UI controls are available in the web editor only; the classic UI (including Linux) continues to track an active position automation range on its sliders.
- **Viewport Path LFO display & tempo sync.** The viewport displays the base path geometry (not the instantaneous Path LFO deflection; the Path timeline highlights the oscillation swing). Path LFO rate is specified in Hz (`0.05–8 Hz`); host-tempo sync is not implemented.
- **Hand-edited non-float pattern values.** A `.verbroom` or XML state manually edited with a non-float `double` pattern value (differing from `(float)` precision) is safely re-baked rather than restoring the mismatched table.
- **Tables baked in 0.2.0 need one fresh Bake.** Estimator and cache changes since 0.2.1 mean 0.2.0 tables load as `notBaked`.
- **Tail direction & round-robin early reflections.** Baked late tails retain their baked orientation (or keyed azimuth curve); source directivity profiles act on direct sound and early reflections while the late tail treats sources as omnidirectional. Early reflections solve up to four sources per 128-sample control period, updating in batches over 4–6 periods with linear glide when more sources are active.
- **Host verification.** Developed and tested in REAPER on Windows 11 (including a 64-channel track with the 32-source VSL setup and 64 host outputs, plus automated VST3 host and UI verification on 0.3.1). A community tester tried `0.2.0` on a real Mac; versions `0.2.1–0.3.1` have not yet been tested on real macOS hardware. Linux builds and passes CI tests with the classic interface and awaits real host testing.

For installation and switching interfaces, see the [User manual](Manual_en.md) and [0.3.0 release notes](Release_0.3.0_en.md).
