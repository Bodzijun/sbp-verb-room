# Verb Room 0.2.0

Verb Room is a physically based room reverb (VST3). You draw a room, place sound sources and a listener in it, and the
plug-in simulates how that room sounds at that spot.

0.2.0 is a large step from 0.1.16. It adds a completely new interface, movement paths, Ambisonic output and a number of
engine fixes. Packages: Windows (x64), macOS (universal: Apple silicon and Intel) and Linux (classic interface only).

**Pre-release status.** The new interface on macOS is a first release: it has been built, but not yet tested on many
Macs and hosts. If something goes wrong, switch to the classic interface (see "Switching interfaces").

## What is new since 0.1.16

### New interface
- A resizable window with a 3D room viewport, a Main page and an **Edit room** page (walls, floor and ceiling zones,
  plates, materials with a catalogue, dimensions and scale), outliner, inspector, CPU load meter.
- Sources, listener and movement paths are edited directly in the viewport and in the inspector.
- Decay curves per band, energy map of the room, reflection paths in the 3D view.
- Undo / redo for room edits (Ctrl+Z / Ctrl+Y on Windows, Cmd+Z / Cmd+Shift+Z on macOS).
- The window size and view settings are remembered.
- The classic interface is still included and can be selected at any time.

### Physical / Creative modes and controls
- **Physical** plays the room as modelled; **Creative** unlocks Size, Pre-delay, Stereo width, Ducking and a **Freeze**
  that really holds the tail, in addition to decay per band and early/tail level.
- Absorption scale and an RT60 target assistant.

### Movement paths
- Sources and the listener can follow **paths** drawn in the viewport (points, closed paths with laps).
- **Path timing:** Start % / End %, a Time % value per point and key easing, so that objects of one group can move at
  different times along the same path.
- **Orientation** per object: Manual, **Along path** or **Look at** another object. Look at also works between
  members of the same group (including moving targets, with any timing).
- **Groups and coupling:** several objects can share one path position and move together.
- **Timeline panel:** shows and edits the position of every object on the Position axis (0-100 %).
- **Path baking:** the late tail along a path can be pre-computed in the background (Bake path / Cancel / Bake again,
  progress in the status bar) for several independent sources, for coupled groups and for the recorded pass of the
  listener, so that long movements do not need a re-trace at every step. Baking runs at lower priority and is
  cancelled by a real scene change.
- Position automation also works through silence if **Keep processing during silence** is switched on in Settings
  (see known limits).

### Output formats
- **AmbiX 1st, 2nd and 3rd order** (4, 9 and 16 channels, ACN order, SN3D normalisation) as additional listener
  formats at the end of the list. Existing presets and projects keep their format indices.

### Directivity and profiles
- Directivity profiles for sound sources and instruments. The **Profiles** folder is the default location and the
  last used folder is remembered; **Profile aim** controls where a profile points.
- Experimental option to keep the directional relations of the tail (Settings).

### Engine
- A sealed room is really sealed: no sound leaking through closed corners or through walls into the tail.
- Correct tail level near walls; bounded real-time cost of the diffraction search.
- Smoother late tail. The energy map and the bake share a thread pool and no longer block the audio thread.
- Output meters measure the final output (after gain and make-up).

Existing `.verbroom` presets, `.txt` scenes and saved projects load as before.

## Installation

### Windows
Copy `Verb Room.vst3` (the folder) to `C:\Program Files\Common Files\VST3\` and rescan plug-ins in your host.
The new interface uses the Microsoft **WebView2 Runtime**, which is normally already present on Windows 10 and 11.
If it is missing, the plug-in falls back to the classic interface and says why in its diagnostics.

### macOS
Copy `Verb Room.vst3` to `/Library/Audio/Plug-Ins/VST3/` (all users) or `~/Library/Audio/Plug-Ins/VST3/` (you only).
The package is **ad-hoc signed, not notarized** (there is no Apple Developer ID). macOS may refuse to load a downloaded
copy. To allow it, either right-click the file in Finder and choose **Open**, or remove the quarantine flag in Terminal:

```
xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/Verb Room.vst3"
```

The new interface uses the system WKWebView (no extra installation). Keep macOS up to date: the interface uses
current web features (macOS 11 needs the latest Safari/WebKit security update, 12 or newer is recommended).

## Switching interfaces

- **From the new interface to the classic one:** open the **Theme** menu and press **Classic UI**. The choice is
  remembered. If the new interface cannot start, the plug-in opens the classic one by itself, and a window that
  stopped responding offers **Use classic UI** and **Retry web UI**.
- **From the classic interface to the new one:** press **New UI** in the classic window (shown only when the new
  interface can run on your system).
- **By environment variable:** `VERB_UI=classic` (or `native`) forces the classic interface, `VERB_UI=web` the new one.
- **Default:** the new interface opens first on Windows and macOS (a choice you made earlier is remembered). If it
  cannot start, the classic interface opens instead.
  The preference is stored in `editor.ini` in `%LOCALAPPDATA%\SBP\VerbRoom` (Windows) or
  `~/Library/SBP/VerbRoom` (macOS). Deleting it restores the default.

## Known limits

- **macOS web interface is a first release.** Untested on many Macs and hosts. Keyboard shortcuts that the page does
  not use are **not forwarded to the host** on macOS (on Windows they are); click outside the plug-in window to use
  host shortcuts. Please report problems with your macOS version, Mac model and host.
- **AmbiX** needs a track with an even number of channels in REAPER (4, 9 and 16 channels need 4, 10 and 16 track
  channels respectively).
- **Keep processing during silence** is off by default and uses more CPU. REAPER stops calling a plug-in when its input
  is silent after the reported tail; either switch the option on or enable REAPER's
  *Compatibility settings > Ignore plug-in reported tail size*.
- Geometry is 2.5D (vertical walls, horizontal floors, ceilings, plates); there are no low-frequency room modes;
  the tail is a statistical estimate. Large scenes are heavy on the CPU.
- Linux: classic interface only.
