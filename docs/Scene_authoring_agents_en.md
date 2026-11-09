# Verb Room: scene-authoring guide for agents

[Ukrainian version](Scene_authoring_agents_uk.md)

This document describes the current 0.1.10 implementation. Before changing the format, check the code in `src/scene.hpp`, `src/rt_engine.cpp`, `plugin/SourceModel.h`, `plugin/RoomLibrary.h`, and `plugin/PluginProcessor.cpp`. Do not modify the user's open DAW or Vienna project unless the task asks for it.

## 1. Choose the format

- **`.txt`** is a geometric scene description for authoring, the CLI, and importing into the plug-in. It does not describe routing or mixer settings.
- **`.verbroom`** is a complete preset for sharing between users. Configure the scene in the plug-in and save it with Save room or `VerbProcessor::getRoomPresetText()`. Do not invent XML parameters manually: the `VerbRoomPreset formatVersion="1"` container holds a complete `VerbState`. See the [preset exchange and Keep I/O guide](Presets_0.1.10_en.md).
- Add built-in presets with multichannel sources through `BuiltinRoom.setup` and `loadRoomWithSetup`, rather than supplying scene text alone. See `orchestraRoom()` and `vslGrandHall()` for examples.

## 2. Coordinates and geometry

Units are metres, degrees, seconds, and decibels. X/Y define the floor plan; Z is height. Azimuth 0° points along +X, and 90° points along +Y. Free channel points use the source's local frame: X forward, Y left, Z up.

The model supports vertical wall segments, horizontal floors and ceilings, and horizontal raised platforms. Domes, sloped planes, and arbitrary 3D meshes are not supported as true surfaces. Use vertical walls to build a faceted floor plan. Do not describe a horizontal reflector as a sloped ceiling.

For an enclosed hall:

1. Define a simple outline without self-intersections, duplicate edges, or zero-length edges.
2. The end of each exterior wall must meet the start of the next, including the last/first pair. Define internal partitions separately.
3. Sources, all their spatial points, and the listener must be inside and clear of surfaces. Check clearances in metres.
4. Sources and the listener must be above the highest platform beneath them and below the ceiling or canopy. Define a raised floor as an `at` platform, rather than just a material patch.
5. When changing coordinate frames, transform walls, zones, sources, receivers, and the listener together. Built-in constructors may shift the scene; do not apply that shift twice.

The plug-in may reorient the scene for a listener-facing display. Check the saved result as well as the constructor's initial coordinates.

## 3. `.txt` syntax

Text after `#` is a comment. Use a decimal point. Material and source names are single tokens without spaces; use `_` instead of spaces. Declare a material before referencing it.

```text
sr 48000
length 3.0
height 8.0
temp 20
humidity 50
order 2
rays 48000
seed 17
detect_radius 0.5
material Oak 0.10 0.08 0.08 0.10 0.12 0.15 0.18 0.25 0.65
floor Oak
ceiling Oak
wall 0 0 12 0 Oak
wall 12 0 12 20 Oak
wall 12 20 0 20 Oak
wall 0 20 0 0 Oak
listener 6 6 1.2 90
source Violins 4 15 1.4 -90 0 0 0
receiver L 5.91 6 1.2 120 0.5 0 0
receiver R 6.09 6 1.2 60 0.5 0 0
```

Complete minimal example: [Agent_Hall.txt](examples/Agent_Hall.txt). A scene requires at least a declared floor material, a `floor` line, one `source`, and one `receiver`. An unspecified ceiling means an open top; always specify `ceiling` for an enclosed hall.

Main additional commands:

```text
# Absorption: seven energy coefficients, then LF/HF scattering.
material NAME a125 a250 a500 a1000 a2000 a4000 a8000 scatterLF scatterHF
# Transmission loss: seven values in dB. Without transmit, the material is opaque.
transmit NAME TL125 TL250 TL500 TL1000 TL2000 TL4000 TL8000
# A full-height wall, or one with explicit bottom/top Z limits.
wall ax ay bx by NAME
wall ax ay bx by NAME z0 z1
# A material patch on the main floor or ceiling.
zone floor NAME x1 y1 x2 y2 x3 y3
zone ceiling NAME x1 y1 x2 y2 x3 y3
# A platform or horizontal canopy at absolute height Z.
zone floor NAME at Z x1 y1 x2 y2 x3 y3
zone ceiling NAME at Z x1 y1 x2 y2 x3 y3
# An open top, either for the whole scene or a zone.
ceiling open
zone ceiling open x1 y1 x2 y2 x3 y3
# Other settings.
roughness 0.1 123
diffraction 1
clutter 12 0.4 1.6 2 10 123 0.5 1.8
```

`source` and `receiver` take `NAME x y z azimuth pattern gainDb elevationDeg`; the final four numbers are optional. `pattern`: 0 = omni, 0.5 = cardioid, 1 = figure-8. `listener` takes `x y z yaw`. In the plug-in, output receivers are generated from the Output layout and listener position. The file's `receiver` coordinates are also used by the CLI and do not replace the plug-in's Output configuration.

## 4. Materials and acoustics

The band order is always **125, 250, 500, 1000, 2000, 4000, 8000 Hz**. `alpha` is the absorbed energy fraction, `tau` is the transmitted fraction, and the reflected fraction is `1-alpha-tau`. For every band, `0 <= alpha,tau <= 1` and `alpha+tau <= 1`. `transmit` is specified in dB: `tau = 10^(-TL/10)`. A value of 0 dB means full transmission, not an opaque surface.

For solid exterior walls, do not inherit nonzero `tau` from a decorative material. For a custom opaque surface, explicitly use `tau.fill(0)` in C++, or omit `transmit` in `.txt`. Do not change absorption just to change the map colour: it changes the sound.

Use `surfacePresets()` / `presetByName()` as a starting point. Explain the choices for wood, drapes, seats, stage surfaces, and canopies. Asymmetric geometry is acceptable, but explicitly check the materials of surfaces that serve corresponding functions. For rear walls and bevels, use a defined edge group or a verified inclusive coordinate boundary. Test the material of each bevel to catch `<` versus `<=` errors.

`length` is not a measured RT60 and does not guarantee the actual tail duration: the plug-in adapts the duration using its model. The Decay graph describes the model, not recorded audio or a precise measurement at a particular seat. Do not claim a match to a named real hall without measurements. Check the actual impulse response, decay by band, and late energy increases.

## 5. Implementation limits

- The plug-in supports up to **32 sources**, **64 spatial points**, **64 inputs**, and **16 outputs**. Count spatial points with `buildPoints()`; LFE is not a spatial point.
- Format IDs: Mono=0, Stereo=1, LCR=2, Quad=3, 5.0=4, 5.1=5, 7.1=6, Ring=7. `firstIn` is zero-based in C++ and displayed from 1 in the UI. For stereo group i, use `format=1`, `channels=2`, and `firstIn=2*i`.
- `sum=true` averages the channels into one point. Thirty-two summed stereo groups use 64 inputs and 32 points. Stereo without summing uses two points per group. Do not replace independent multichannel content with a mono sum without explaining the choice.
- There are at most **8 raised platforms/canopies in total**. Plain patches without `at` do not count. The sanitizer converts excess platforms into patches; check the count BEFORE sanitizing.
- The editor permits up to 128 walls, but the early-reflection wall budget depends on order: 128 / 64 / 24 / 12 walls for orders <=1 / 2 / 3 / 4. Do not add outline complexity without checking the actual early-reflection model.
- The current plug-in uses first-order early reflections (`ec.erOrder=1`); the tail forms the rest of the field. The `.txt` `order` setting is also used by the standalone renderer. Do not claim that changing this line switches the VST's early-reflection order.
- Plug-in position parameters range from -200 to 200 m for X/Y, 0 to 30 m for Z, and -180 to 180° for azimuth. The parser may accept wider limits that the UI/APVTS cannot reproduce.
- The engine's tail is limited to 20 s. The plug-in's tail ray budget is 8000–384000; Adaptive may use up to 4× the base budget. The scene's `rays` field does not replace the Tail quality saved in a complete preset.

## 6. Required checks before delivery

1. Parse → serialize → parse: geometry, materials, and source order must survive.
2. Check the outline, self-intersections, material indices, all functional wall groups, positions, and heights.
3. Check `firstIn + effectiveChannels() <= 64`, spatial points <=64, and raised platforms <=8. Input ranges may overlap only intentionally.
4. Prepare the processor, load the scene, and wait for Ready. Compare sources, points, and outputs with the expected setup.
5. Send an impulse to the first and last groups, and to both channels of the final stereo pair. Check for silence, NaN, unexpected repetitions, and abrupt tail cutoff.
6. Export `.verbroom`, load it into a fresh processor with Keep I/O off, and compare routing, mixer settings, positions, quality, and output. Separately test Keep I/O on with a different local configuration.
7. Inspect the plan and front views. Include a clear description of the design, channels, and materials. Clear accidental Solo/Mute settings before publishing.

Useful existing checks, run from the repository root after building:

```powershell
$env:VERB_PRESETS_ONLY='1'
& './build-msvc/plugin/VerbPluginTest_artefacts/Release/VerbPluginTest.exe'
Remove-Item Env:VERB_PRESETS_ONLY
$env:VERB_VSL_HALL_ONLY='1'
& './build-msvc/plugin/VerbPluginTest_artefacts/Release/VerbPluginTest.exe'
Remove-Item Env:VERB_VSL_HALL_ONLY
$env:VERB_VSL_TAIL_ONLY='1'
& './build-msvc/tail_regression.exe'
Remove-Item Env:VERB_VSL_TAIL_ONLY
```

A new hall needs its own checks; passing the VSL tests does not validate a different scene. Do not install a VST3 over a binary currently used by a DAW. Prepare and verify the package first, then wait for the host to release the file.
