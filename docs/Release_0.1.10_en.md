# Verb Room 0.1.10

Source mixer improvements, a larger orchestra setup, and complete presets you can share.

Documentation: [preset sharing and Keep I/O](Presets_0.1.10_en.md) · [scene authoring for agents](Scene_authoring_agents_en.md).

## Changes since 0.1.7

### 0.1.10 — Shareable presets and hall correction

- **Complete `.verbroom` presets.** Save room now saves the room geometry and materials, source and listener positions, source names and gains, input formats and channel assignments, summing, spread, directivity, free channel positions, output configuration, mix settings, Solo/Mute, and tail quality.
- **Preset exchange.** Share a single `.verbroom` file and load it through **Room library → Load from file ...**, or place it in the user's room library. Two ready-to-load VSL Grand Hall files are included.
- **Keep I/O support when importing.** With Keep I/O enabled, the existing source setup and output configuration stay in place. Room geometry, positions and mix parameters load from the preset by source index. With Keep I/O disabled, the preset restores its complete setup. Keep I/O, meter visibility and Compact/Normal remain local preferences.
- **Safer loading and saving.** Unsupported format versions, invalid scene data, incomplete parameter lists, invalid input ranges and excessive spatial-point counts are rejected before applying the preset. Replacing an existing file asks for confirmation; write failures are reported.
- **Legacy support.** Existing `.txt` scenes still load. They describe the scene rather than a complete plug-in setup; load and configure one, then use Save room to create a full preset.
- **VSL Grand Hall material fix.** Both rear bevels now use the same absorbing material as the central rear wall. The side walls retain their reflective wood material. The hall's intentional geometric asymmetry is preserved.
- The scene description accompanying an IR export now includes the current source gains.
- Added a scene-authoring guide for agents and contributors, a minimal scene example, and preset regression tests.

### 0.1.9 — VSL Grand Hall

- Added **VSL Grand Hall — Audience** and **VSL Grand Hall — Conductor**, with **32 stereo instrument groups on inputs 1–64**.
- Added an editable, asymmetric hall with a 16-sided outline, a raised stage, three riser levels, two audience balconies and overhead reflectors.
- Each stereo group is averaged into one spatial source by default, letting Verb Room place the instrument in the room. The two presets offer audience and conductor listening positions.
- Intended for VSL Studio and other dry orchestral libraries. Instruments and sample content are loaded separately; this is an original modeled hall, not a measured recreation of a named venue.

### 0.1.8 — Compact mixer and source Solo/Mute

- Added **Compact / Normal** mixer modes, with 92 px and 160 px channel strips. Compact is the default; the selection is saved with the project.
- Added automatable **Solo and Mute for each source**. Multiple sources can be soloed together, and Mute takes priority over Solo.
- Source switching smoothly changes new input to the room, while reverb already in progress continues to decay. It does not rebuild or retrace the engine.
- Source Solo/Mute also affects the dry signal on channels assigned to those sources and works alongside Direct / Early / Tail Solo.
- Source switches are saved in projects; older projects load with them inactive. Input meters continue to show the signal before source gain and Solo/Mute.

## Also included from 0.1.6–0.1.7

- Fixed premature host-side tail cutoff, especially noticeable with Cathedral Long: longer room-dependent tails, support up to 20 seconds, and conservative tail reporting from plug-in startup.
- Added **Keep I/O** for changing rooms without replacing the source setup and output configuration.
- Added the dedicated **Mix** workspace with source strips, input meters, automatable gain faders, input/format selection, summing, spread and directivity.
- Moved Keep I/O into the input settings, widened the source faders, fixed input-range text and spacing, and removed numeric-field outlines.
- Reordered the tabs to **Objects → Surfaces → Space → Mix → Decay**.

## Updating and sharing

- **Windows x64 VST3; AVX2/FMA CPU required.** Close the DAW before replacing the entire `Verb Room.vst3` folder in `C:\Program Files\Common Files\VST3`.
- Existing projects retain their saved room geometry. Select VSL Grand Hall again, or load one of the new `.verbroom` files, to apply the corrected rear-wall materials. Enable Keep I/O first if you want to retain your current source setup.
- To apply all 32 groups from the VSL preset, disable Keep I/O and provide 64 input channels in the host. Presets do not create DAW sends, change host pin mapping, or include VSL instruments or samples.
- `.verbroom` format 1 requires Verb Room 0.1.10 or a later version supporting that format. Solo/Mute is included in the preset, so check the switches before sharing.
- Your library is in the system Documents folder under `SBP/Verb Room/Rooms`. Use **Open my library folder** to locate the saved file.

## Validation

Passed the portable-preset tests, both VSL hall configuration and input-routing tests, and all four VSL tail regression scenarios. The preset tests cover two-instance file round-trips, Unicode names, free points, mix and routing state, Keep I/O, legacy scenes and rejection of damaged data. The scene example was also parsed and rendered with the CLI.
