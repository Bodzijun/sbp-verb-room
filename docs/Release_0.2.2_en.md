# Verb Room 0.2.2

Verb Room 0.2.2 is a maintenance release over 0.2.1. It fixes path baking after room edits and during manual orientation, and improves wall editing and the Sources panel. Release targets are Windows x64 and macOS Universal (Apple silicon and Intel). There is no Linux release package.

## What is fixed and new

### Sound / engine

- **Bake after room edits:** Bake can finish after you edit or replace the room with the host transport stopped. The status banner shows what Bake is waiting for.
- **Manual orientation during movement:** the baked tail stays active while you change the manual azimuth of a moving source or listener. This applies to linked path groups and recorded passes. Bakes from recorded passes no longer depend on the host buffer size.

### Interface

- **Auto-link corners:** with Auto-link enabled, dragging an entire wall also moves the joined ends of neighbouring walls. The preview shows which ends will move, and one Undo restores the full edit. The viewport sends the switch state through `surface.translate.autoLink`, which defaults to off if omitted by an older caller.
- **Clutter:** use **Randomize** next to Seed to select a different layout.
- **Path tooltips:** **Bake path**, **Record pass**, **Clear bake**, and **Manual** orientation explain their actions. Press **Bake path** after recording a pass to compute its baked table.
- **Sources:** the per-source **Σ** button sums the channels of a multichannel source into one point. **All formats…** sets formats and renumbers inputs for sources that fit the available budget. **Sum for all sources** toggles Sum for multichannel sources. Each bulk action takes one Undo. The list keeps its scroll position after edits.

### Presets and compatibility

- Existing `.verbroom` presets, `.txt` scenes, and saved projects continue to load. Moving Manual objects with tables baked in an earlier build may need one fresh **Bake path**.

## Known limitations

- **Tables baked in 0.2.0 must be re-baked once.** The estimator and cache changes in 0.2.1 mean that these tables load as `notBaked`.
- **Tail direction:** baked late tails retain the azimuth at bake time. Source directivity profiles affect direct sound and early reflections. The late tail treats each source as omnidirectional.
- **AmbiX level:** the output uses ACN/SN3D with an unattenuated W channel. It is louder than stereo by design. Normalization and gain options are planned.
- **Round-robin early reflections:** up to four sources are solved on each 128-sample control period. With more than four, early reflections update in batches over 4–6 periods and glide linearly during motion (about 10.7 ms at 48 kHz).
- **Host verification:** Windows is the platform tested in REAPER. Automated performance checks cover multi-source scenes. Live listening and performance on older 4-core systems remain open. This release has yet to be tested on a real Mac or macOS DAW. The macOS bundle is ad-hoc signed, lacks notarization, and the web editor does not forward unused keys to the host.

For installation and switching between the new and classic interfaces, see the [0.2.0 release notes](Release_0.2.0_en.md).
