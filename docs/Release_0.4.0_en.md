# Verb Room 0.4.0

[Українською](Release_0.4.0_uk.md) · [User manual](Manual_en.md)

## What's new

- **Motion** is a separate switch beside **Physical / Creative**. It adapts the late reverb in real time as sources and the listener move, with no Bake step. Turning Motion off restores access to existing baked tables.
- **Paths** have eight shapes (Circle, Ellipse, Figure-8, Square, Rectangle, Triangle, Star and Spiral). Edit shape parameters live, compare with the previous shape ghost, and set the Spiral's Start and End Z separately. Collapse the Points list when drawing. **Endless** runs closed paths from the host transport, with a live position marker.
- **Path wander** adds a shared random displacement. **Link to path** makes followers use the same path with local forward, left and up offsets. Disable **Follow wander** on individual followers. **Unlink** converts the offset curve to an independent path.
- **Trigger zones** provide up to 16 box or polygon volumes with soft edges and hysteresis. **In zone**, **Zone index** and **Distance to zone** drive the modulation matrix and **Mod Out 1–16**. A visibility toggle controls the viewport overlay. The classic editor has a zone list.
- **MIDI out** sends note gates, note triggers, CC and keyswitches from modulation routes in Creative while the transport plays. Verb Room declares a VST3 MIDI output and does not pass incoming MIDI through. The compact dock MIDI selector stays in the dock.
- **Stems workflow** explains how to render groups in separate passes with the Source Solo buttons. The four-source static regression covers Physical, Creative, Positioned dry and Motion.

## Compatibility

New settings use optional project state and preset fields. Existing projects and presets open with the old behavior when the new settings are absent. The first 283 APVTS parameters keep their indices. **Mod Out 1–16** still occupy indices 283–298 (299 host parameters total).

With Motion off and the other new settings unused, the 0.3.1 and 0.4.0 Windows VST3 impulse renders were bit-identical for the default stereo room, legacy state, VSL Grand Hall and the path with azimuth keys (768,000 float samples total). The modulation and third-order AmbiX cases (16-in/16-out and 64-in/64-out) differed by at most 1.12 × 10⁻⁸ FS after a longer silent pre-roll. Shorter pre-rolls exposed run-to-run variation in asynchronous tail preparation.

## Known limits

- Linked followers always use the live tail. Use **Link to path** with **Motion**. For per-object Bake, draw each object's path separately.
- At cold start Motion plays direct sound and early reflections while the tail prepares (about 0.5–1.6 seconds in the measured scenes), then fades the tail in over 150 ms. With five or more sources, a soloed source behind a partition can still use the shared decay shape. Rapid simultaneous movement of one source and the listener in a large hall can reduce accuracy. In the MO5b benchmark with 16 outputs and 128-sample blocks at 48 kHz, 8 sources averaged 365 µs per audio block (p99 686 µs) and 32 sources averaged 623 µs (p99 951 µs).
- Freeze and the shared Motion tail with five or more sources can make the stem sum differ from the full mix. Keep feeding all host inputs in each pass so Duck continues to use the same detector signal.
- The classic editor has limited shape controls and a zone list. Use the web editor to edit the modulation matrix, Path LFO, azimuth keys and automation range.

## Test status

A pre-release Windows build was tested in REAPER, including MIDI from Verb Room to a sampler on another track. REAPER does not let one track both take audio and send the MIDI its effect generates to another track; the setup that worked uses the Windows built-in MIDI loopback (steps in the manual, section MIDI out). A JUCE VST3 smoke host also verified the MIDI event bus and note/CC delivery. Other hosts and real macOS and Linux systems remain unverified for 0.4.0. The release ships VST3 builds only.
