# Verb Room 0.1.12: positioned dry and scene organization

## Positioned dry

Mix now includes **Positioned dry**. The direct sound keeps its source position, distance, propagation delay, directivity and microphone cues. At 0% Dry/Wet, only that direct field is audible. Raising Wet adds early reflections and the tail, reaching the full room at 100%. The direct sound is not duplicated, avoiding a correlated level increase at the midpoint.

Every configured source, including groups on additional inputs, is rendered through its spatial points. Gain, Solo/Mute, summing and free channel points are respected; LFE keeps its separate direct route. This is the acoustic model's direct path, rather than just panning: distance and obstacles can affect its level and spectrum.

Turn Positioned dry off for the original crossfade with unprocessed input channels. New instances default to Positioned dry; older projects and format 1 presets retain their original behaviour. Direct / Early / Tail Solo bypasses Dry/Wet to audition components at their configured levels.

## Lock pan

**Lock pan** prevents both left-button and middle-button map panning. Missing a surface no longer moves the grid. Zoom, selection and object editing remain available. Camera orbit in 3D is a separate action.

## Outliner and Link

**Outliner** lists walls, the main floor and ceiling, zones and raised plates.

- Click to select; Shift/Ctrl adds or removes items from the selection.
- Select at least two walls/zones and click **Link** to create a named construction with child surfaces. The main infinite floor and ceiling cannot be members of these constructions.
- Drag a linked member in plan view to move the whole construction while preserving relative geometry. A locked member prevents partial movement of the group.
- **Unlink** removes the relationship without changing geometry. Hold Alt to select an individual member, and keep Alt held when dragging it separately on the map.
- **H** hides a surface or group on the map. Hidden surfaces cannot be picked or snapped to, but **remain acoustically active**.
- **L** locks map picking and dragging. Select the surface in the outliner to inspect or unlock it. Locked selections cannot be deleted or have their materials changed.
- Double-click a name to rename a surface or group. Use `+`/`-` to collapse or expand a construction.
- Ctrl+Z undoes grouping, metadata changes, movement and deletion. Linking does not merge surfaces into one acoustic plane.

Material changes apply to the whole selection. Wall/plate heights are edited with one surface selected. Room scale continues to scale the whole room.

## Persistence and compatibility

Names, groups, visibility and locking are stored as `#@verbroom-editor-v1` scene comments. Existing text scenes remain readable; older plug-ins ignore these comments and use the same acoustic geometry.

New `.verbroom` files use **format 2**, requiring **Verb Room 0.1.12 or a later version supporting that format**. Format 1 presets from 0.1.10/0.1.11 continue to load. This prevents older plug-ins from silently discarding Positioned dry.

Surface organization and the dry mode are saved in projects and complete presets. Lock pan and Outliner visibility are saved in projects but remain local preferences when importing another user's preset.

## 3D view and layers (0.1.16)

- **3D navigation:** drag to orbit; pan with middle-button drag or Shift + left-drag; the wheel zooms towards the cursor. **F** fits the whole scene, **Shift+F** fits the selection (also the Frame buttons), **Home** resets the camera. Double-click an object to centre the orbit on it, or double-click empty space to fit the scene. Right-drag still rotates the listener or source. The camera is not saved.
- **Layer states:** the Walls / Floors / Ceilings / Plates states (shown, locked, hidden) are kept when the editor is closed and reopened, and are saved in projects, complete `.verbroom` presets and scene text as `#@verbroom-editor-v2 layers`. Library rooms and geometry-only `.txt` scenes reset them to shown.
- **Scale room** scales zones together with the walls; with objects included it also scales plate heights.
