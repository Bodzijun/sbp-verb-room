# Verb Room

Version 0.1.5: selectable/adaptive ray budgets, seven-band decay diagnostics with estimated RT60, and combinable Direct/Early/Tail Solo. Quality updates crossfade the tail without resetting audio history. See [the guide (Ukrainian)](docs/Quality_decay_solo_0.1.5_uk.md). Includes the [0.1.4 tail fixes](docs/Tail_fix_0.1.4_uk.md).

A physically based room reverb for **REAPER and other VST3 hosts** (Windows x64), built for scenes where
sources and the listener *move*: cars, fly-bys, games-style sound design, virtual rooms with many sources.

Instead of a fixed impulse response, Verb Room traces the sound through a room you draw (wall segments with
materials, floor/ceiling zones, lowered ceilings, doorways) and re-solves the geometry as the sources and the
listener move. Position and orientation of every source and of the listener are **automatable parameters**.

> Status: working alpha (0.1). Developed and tested on Windows 11 with REAPER. Only the VST3 format on
> Windows x64 is built and tested. An AVX2/FMA CPU is required.

## What it does

- **Hybrid engine**: image-source early reflections plus a ray-traced late tail rendered by a partitioned FFT
  convolution (zero added latency), computed in 7 octave bands with air absorption.
- **Rooms from wall segments (2.5D)**: any polygon or open layout; each wall has its own material, a bottom and top
  (low walls, windows, doorways), pass-through (transmission loss per band) and roughness.
- **Floor and ceiling zones** with their own materials, openings in the ceiling, and **plates** — horizontal surfaces at
  their own height (bonnet, mezzanine, platform) that reflect from both sides and can let sound through.
- **Diffraction** around wall ends (also for walls that only attenuate, such as doors and glass), soft reflection edges,
  so the level changes smoothly when a source or the listener passes a corner.
- **Moving sources and listener** with Doppler (strength adjustable). Positions are smoothed between host blocks so
  irregular automation does not cause pitch wobble.
- **Many sources**: up to 32 sources and 64 engine points on up to 64 input channels. A source can be mono, stereo,
  LCR, quad, 5.0, 5.1, 7.1 or a ring of N channels, with a free position for each channel point.
- **Listener**: output layouts Mono, Stereo (ORTF / XY), LCR, Quad, 5.1, 7.1, 7.1.4 and a ring of N channels;
  microphone patterns (omni … figure-8) and a **human-ears** pattern with head shadow (mic radius = ear spacing).
- **Mix controls**: Dry/Wet (equal power), output gain, early-reflection level, diffusion, Doppler strength,
  **tail level**, and automatable, combinable **Direct / Early / Tail Solo** with smooth switching.
- **Tail quality**: Preview / Mix / Render profiles, manual ray budgets and bounded adaptive refinement, updated in the background without restarting the engine.
- **Decay tab**: seven octave-band energy-decay curves, estimated RT60 from T20, and an indication of extrapolated regions. Individual curves can be hidden for comparison.
- **Editor**: plan, front, side and 3D views; wall/zone/box-select tools; undo; a room library with 10 starter rooms and
  two MotorSynth car cabins; layers (show / lock / hide walls, floors, ceilings, plates); repeated click steps through
  surfaces that overlap; optional input channel meters (numbered as in the host).
- **Tail length that fits the room**: the late tail is made as long as the room needs (up to 10 s) and fades out smoothly, so long decays are not cut off.
- **Orchestra halls**: a chamber hall and a symphony hall (listener in the audience or on the conductor's podium) with the instrument groups at their real places and heights on stage risers; see `docs/Orchestra_presets.md`.
- **IR export**: renders one 32-bit float WAV per engine point plus a text description of the room.

## Install (binary)

Copy `Verb Room.vst3` to `C:\Program Files\Common Files\VST3\` and rescan plug-ins. If REAPER is running it keeps the old
DLL in memory: restart it after replacing the file. A room is saved with the project; choose a library room again
after updating if you want the new defaults.

## Using it in REAPER

Insert one instance on a bus and send your sources to it. Each source takes the input channel(s) you set in
*Objects → First input channel*. With 30 sources of stereo pairs, put `Wind` on channels 21/22 in the send, and so on.
The built-in "MotorSynth" rooms expect the layout of the SBP MotorSynth2 routing scheme (see `docs/`).

## Build

Requirements: CMake ≥ 3.16, MSVC 2019+ (Windows), Git. JUCE 8.0.15 is a submodule.

```
git clone --recurse-submodules https://github.com/<you>/sbp-verb-room.git
cd sbp-verb-room
cmake -S . -B build -DVERB_BUILD_PLUGIN=ON
cmake --build build --config Release
```

The plug-in is produced in `build/plugin/VerbPlugin_artefacts/Release/VST3/`.

Tests (each prints `all ... passed`; the paths depend on the CMake generator):

```
build\Release\rt_test.exe scenes                                  # engine: reflections, tail, diffraction, zones, plates...
build\Release\tail_regression.exe                                # sparse tails, orchestra presets, rates, block boundaries
build\Release\quality_test.exe                                   # RT60, adaptive budgets, Solo summation and ramps
build\plugin\VerbPluginTest_artefacts\Release\VerbPluginTest.exe  # plug-in, editor, state, car presets, channel order
build\plugin\VerbPluginStress_artefacts\Release\VerbPluginStress.exe
```

The timing checks in `rt_test` depend on how busy the machine is and can fail on a loaded system.

## Limits worth knowing

| | |
|---|---|
| Walls | 128 (reflection order 1, as the plug-in uses); 64 / 24 / 12 for orders 2 / 3 / 4 |
| Zones | up to 32 floor and 32 ceiling zones, 3–64 corners each; 8 plates in total |
| Sources / points / inputs / outputs | 32 / 64 / 64 / 16 |
| Diffraction | around vertical wall ends, up to three ends in a row |
| Human ears | head shadow and ear spacing only, no pinna filtering |
| IR export | static positions, no LFE |

Note for REAPER users with many input channels: the plug-in compensates for the channel reordering that JUCE applies
when a host describes a long bus as a run of VST3 speaker bits, so "input N" really is track channel N.

## License

Copyright (C) 2026 SBP. Licensed under the **GNU Affero General Public License v3.0** — see `LICENSE`.
Verb Room is built with [JUCE](https://juce.com) 8, used under its AGPLv3 option. If you distribute a build you must
make the corresponding source available under the same license.
