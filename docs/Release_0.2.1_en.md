# Verb Room 0.2.1

Verb Room 0.2.1 is a maintenance and performance release over 0.2.0, addressing tester feedback on path baking, multi-source CPU load, and the web interface. Packages: Windows (x64) and macOS (universal: Apple silicon and Intel). A Linux package is not provided in this release.

Thanks to everyone who tested 0.2.0 on Windows and macOS and reported audio, performance and workflow issues.

## What is fixed and new

### Sound / engine
- **No crackle on baked paths:** replaced the frequency-domain baked-path equaliser with a time-domain 7-band one-pole filter bank with per-sample gain interpolation, removing block-boundary clicks on bass-heavy material.
- **More accurate, smoother path baking:** the bake estimator now selects one consistent ray budget across the path, uses 4 time-aligned convolution windows (`0–75`, `75–149`, `149–405`, `405+ ms`) with effective-sample-size regularisation to prevent late-tail level overshoot when moving between rooms, and removes step discontinuities between stations. Bake progress advances monotonically.
- **No doorway dip on curved paths:** smoothed listener/source motion now projects onto the movement polyline so `AlongPath` orientation stays aligned with position, and brief projection misses hold the previous band gains instead of dropping to `0 dB`.

### Interface
- **Mouse wheel safety:** added **Mouse wheel changes values** in Theme / Settings (off by default, so scrolling panels with a Magic Mouse or trackpad no longer changes knobs or number fields accidentally; `Alt+wheel` always adjusts values).
- **Timeline Hold on automatic keys:** **Hold here (pause)** now works on keys whose timing has never been moved manually (assigns an explicit time and inserts the pause in one undo step), or appears disabled with an explanation when a pause cannot be placed.
- **Undo while drawing walls:** `Ctrl+Z` / `Cmd+Z` during wall drawing now removes only the last wall segment and draft point rather than cancelling the entire chain; the automatic ceiling follows.
- **Inspector numeric entry:** fixed horizontal scrolling inside the Main Inspector when typing into numeric fields in narrow windows.
- **Edit room 3D drag and ghosts:** dragging walls, floor/ceiling zones and plates in 3D now projects onto the horizontal plane at the clicked height (eliminating parallax drift), and shows a unified dashed ghost for the whole linked group or zone while originals stay dimmed until release.
- **Rebuilding banner:** added a visible amber **Rebuilding…** banner at the top centre of the viewport during background rebuilds.
- **Source level meters:** source strip meters on the **Sources** tab now display post-gain levels (following source gain, Mute and Solo) with a clickable **Peak / Smooth** ballistics toggle in the column header.
- **Redraw path ghost:** while redrawing an existing movement path, the previous path is drawn as a dimmed, dashed, non-clickable ghost until you press **Finish** (or **Esc** to restore it).
- **Per-source colour:** added an optional colour swatch per source on the **Sources** tab (Auto, 12-colour palette, custom `#rrggbb` or colour picker), plus a small interface fix to the popover's **Auto** label. Colours appear on mixer strips, meters, inspector headers, 2D/3D viewport markers and Timeline rows without affecting audio or triggering an engine rebuild.
- **Default surface material:** walls, rooms and plates created in the web viewport now default to Plaster (and floor zones to Carpet), matching the classic interface.

### Performance
- **Much lower CPU load with many sources:** when moving the listener in large sessions (e.g. 20 orchestral sources), the background worker pool now respects CPU affinity and limits itself to `cores / 2` lower-priority threads, stops cancelling and restarting legacy rebuilds during continuous motion, caches static wall geometry and stationary sources, vectorises early-reflection folding and tap interpolation with AVX2 on x64 builds, and uses round-robin early-reflection updates for scenes with more than 4 sources.

### Presets and compatibility
- **Baked paths saved in projects and presets:** baked path tables (`VRBK v2`) are now stored inside saved host project states and `.verbroom` presets and restored automatically on reload, as well as across Undo / Redo via an in-memory cache.
- Existing `.verbroom` presets, `.txt` scenes and project states from 0.2.0 and 0.1.x continue to load. Sources left on **Auto** colour omit the colour attribute so saved files remain compatible with older builds.

## Known limitations

- **Tables baked in 0.2.0 must be re-baked once:** because the bake estimator and cache format changed to fix late-tail overshoot, baked tables saved in 0.2.0 are cleanly rejected on load (`notBaked`) and need one fresh Bake.
- **Round-robin early reflections (> 4 sources):** scenes with 4 or fewer sources solve every source on every 128-sample control period as before; scenes with more than 4 sources update early reflections in round-robin batches (`4–6` periods) with smooth linear gliding (~`10.7 ms` at `48 kHz`) during motion.
- **macOS verification:** 0.2.1 has not yet been tested on a real Mac or macOS DAW host. The 0.2.0 macOS limits still apply (unused keyboard shortcuts are not forwarded to the host; the bundle is ad-hoc signed, not notarized).
- **Verification scope:** multi-source CPU benchmarks and UI/audio checks were run in automated test harnesses (including a 4-core CPU affinity test on Windows); live REAPER listening and testing on older 4-core hardware are still up to users and testers.
- For installation and switching between the new and classic interfaces, see [Verb Room 0.2.0 release notes](Release_0.2.0_en.md).
