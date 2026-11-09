# Verb Room 0.1.10: sharing presets

[Ukrainian version](Presets_0.1.10_uk.md)

## Save and share

1. Wait for the room to finish loading, then configure it.
2. Click **Save room**, enter a name, and click **Save**.
3. In the room menu, choose **Open my library folder**. Send the `YourRoom.verbroom` file to another user.
4. The recipient opens it through **Load from file ...**. Alternatively, they can place the file in their own library and reopen the plug-in editor.

Library location: the system Documents folder → `SBP/Verb Room/Rooms`.
`.verbroom` requires **Verb Room 0.1.10 or a later version supporting format 1**.

## What the file contains

- Geometry, materials, zones, raised platforms, air settings, clutter, and roughness.
- Sources and listener: positions, orientations, names, and gains.
- Source formats, input assignments, summing, spread, directivity, and free channel positions.
- Output and microphone configuration.
- Mixer settings, Dry/Wet, output gain, Early/Tail levels, and Solo/Mute.
- Tail ray budget and Adaptive.

This is a self-contained XML file. It does not include VSL samples, other VST plug-ins, MIDI, tracks, or external DAW/Vienna connections. Impulse responses are recalculated after loading.

## Keep I/O

**Off:** the source configurations, input assignments, and output configuration are imported from the preset. For VSL Grand Hall, this means 32 stereo groups on inputs 1–64.

**On:** the current source list and configuration are retained, including names, formats, inputs, and channel points, together with the output configuration. The room, positions, and mixer parameters are imported from the preset by source index. To reproduce another user's setup, turn Keep I/O off before loading. To try another hall with your existing routing, leave it on.

The Keep I/O switch itself, the Compact/Normal view, and input-meter visibility remain the recipient's local preferences.

The plug-in does not create DAW sends or change host pin mapping: track channel counts and connections must be configured in the host. The 32 stereo groups require 64 available inputs.

## Compatibility and validation

Older `.txt` files still load as geometric scenes. They do not contain the complete routing or mixer setup. To upgrade one, load it, configure its inputs and outputs, and use **Save room**.

Overwriting an existing `.verbroom` file requires confirmation. Write failures appear in a dialog. Unknown format versions, damaged scenes, and incomplete parameter lists are rejected before changing the current state.

Solo/Mute is saved too: before sharing a preset, check that no groups were accidentally left muted or that Solo Tail was not left enabled.

## Changes in 0.1.10

- VSL Grand Hall: both rear bevels and the central rear wall use `Rear_absorbing_wall`; the side walls retain their wood material.
- Added complete `.verbroom` saving and loading through the library menu and Load from file.
- Keep I/O also works with complete presets.
- The `.txt` scene description accompanying an IR export now includes the current source gains.
- Added a [scene-authoring guide for agents](Scene_authoring_agents_en.md) and a [minimal example](examples/Agent_Hall.txt).

The corrected materials take effect when you reselect the built-in VSL Grand Hall or load a new `.verbroom` file. Previously saved projects retain the geometry stored in them.
