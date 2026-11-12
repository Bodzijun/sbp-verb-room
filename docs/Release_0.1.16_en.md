# Verb Room 0.1.16

## Sound

- **Smoother start of the late tail.** The part of the tail between roughly 80 and 400 ms used to sound loose and uneven: individual rays produced bumps and gaps that were shared by all bands and by both ears. The tail envelope in that zone is now smoothed according to how many rays actually contribute, so it follows the expected build-up and decay. Strong late reflections (flutter echoes between parallel walls) are protected, the direct sound, early reflections and the first 5 ms of the tail are untouched, and the late energy per octave band (within 0.2 dB) and the RT60 (within 3 %) are preserved. Existing projects keep their settings but sound slightly different in that one zone; this is an intentional bug fix.
- **Decay multipliers are numerically safe.** Decay Low / Mid / High above 1 could previously produce enormous levels, Inf or NaN on short or very long rooms. The tail energy is no longer raised above the band's original peak, and when a requested decay does not fit into the 20 s tail limit the effective multiplier is clamped and shown in the tooltip.
- **IR export matches what you hear.** The exported impulse response now applies the same Early / Tail level, diffusion, Doppler, absorption and decay as the live sound in the current mode.

## Physical / Creative modes

- A new instance starts in **Physical**: Early 0 dB, Tail 0 dB, Doppler 1.0 and Decay 1.0, so the room sounds as modelled. **Creative** applies the stored Early, Tail, Doppler and Decay Low / Mid / High overrides. Switching is smooth and never erases the stored values.
- New **Absorption** scale (0.3–3.0) for both modes, and an **RT60 target** assistant that adjusts absorption to reach a requested mid-band RT60 (with Cancel and an "unreachable" message).
- Old projects and `.verbroom` presets without a mode load as **Creative**, with all their saved values (including Early -9 dB), so they sound as before. Built-in rooms and geometry-only `.txt` scenes keep the instance's current mode.
- Loading a room or changing absorption no longer blocks the host for seconds: the tail length is cached and calculated in the background.

## Interface

- **Layer locks are remembered.** The Walls / Floors / Ceilings / Plates state (shown, locked or hidden) survives closing and reopening the editor, and is saved in projects, `.verbroom` presets and scene text. A lock or hide changed while a room is rebuilding is no longer lost.
- **Scale room** now scales zones (and plate heights with objects) together with the walls.
- **3D view navigation:** pan with middle-button drag or Shift + left-drag, zoom towards the cursor, **F** fits the whole scene, **Shift+F** fits the selection, double-click centres the orbit on an object (or fits the scene on empty space), **Home** resets.

## Compatibility

- Old projects, `.verbroom` presets (format 1 and 2) and `.txt` scenes load as before. New files stay format 2; additional parameters are ignored by older format-2 readers, which cannot reproduce Physical mode or absorption/decay.
- Layer state is written as `#@verbroom-editor-v2 layers ...`, a plain comment for 0.1.15 and earlier.

See the [editor guide](Spatial_editor_0.1.12_en.md), the [scene authoring guide](Scene_authoring_agents_en.md) and the [preset exchange guide](Presets_0.1.10_en.md). The Ukrainian technical description of the modes and the tail fix is in [Physical_creative_stage2_uk.md](Physical_creative_stage2_uk.md).
