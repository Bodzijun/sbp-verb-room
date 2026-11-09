# Verb Room 0.1.14: sound energy coverage

The **Energy** tab maps modeled sound energy on a horizontal room slice. Press **Analyze** to calculate it on a background worker. Analysis does not change the scene, routing or audio. **Cancel** preserves the previous map. Closing the editor cancels its worker; maps are not saved in presets.

## Controls

- **Excitation:** all audible source groups or one group. Live positions, rotation, gain, Solo/Mute, layouts and free channel points apply. LFE is excluded.
- **Octave band:** 125–8000 Hz, or Broadband, the mean energy of seven equally excited bands.
- **Probe height:** absolute height above the base floor, 0.1–30 m. Cells below raised floors or above the closed main ceiling are excluded.
- **Direct / Early / Tail:** choose components and Analyze again. Direct includes modeled transmission/diffraction; Early uses first-order reflections and the audio Early level; Tail uses the audio Tail level.
- **Total tail rays:** 12k / 48k / 192k total rays, shared across the selected spatial source points. Increase the budget for large orchestras or noisy maps. Disabling Tail skips tracing.
- **Map resolution:** 16 / 24 / 32 cells on the longest side; 0.1.15 also offers 48 / 64 / 96. The other side preserves physical proportions. The default remains 24. Larger grids increase analysis time and map memory use without changing the audio engine. Use 48–64 for most detailed checks, or 96 for a closer view. Late-field detail also depends on receiver-sphere averaging and the ray budget.
- **Tail integration:** integrate late energy over up to 20 seconds. Shortening this excludes later arrivals from analysis without changing the audio tail.
- **Colour range:** from 0.1.15, 0.5–120 dB below the map peak (previously 20–80). Smaller ranges reveal subtle differences; try 1–5 dB for a uniform tail. **Auto** fits the scale from the peak to the fifth percentile of lit-cell energy in the current band, covering approximately 95% of its values. This changes display contrast only. Bands and colour range can be changed without retracing.

The pink marker follows the listener's current XY position. Probe height sets the map's Z. Hover for cell coordinates and level. Wall bounds define the map rectangle; a wall-free scene uses an area around the sources. +X points right and +Y up, independently of editor/listener rotation.

## Interpretation

The colour scale's 0 dB is the loudest cell in the displayed band. The numeric peak below the map is referenced to unit excitation at 1 m, without calibrated dB SPL. Each map/band normalizes its colour scale separately; use the numeric peak when comparing them.

Each spatial point receives unit excitation, and independent path/source energies are added. A summed group is analyzed as one point. This does not measure live music, sample spectra or channel correlation. Probes are omnidirectional, independent of output microphone layout, Dry/Wet, Output gain and audio component Solo. The Energy component switches affect analysis only.

The map uses the engine's actual geometry, including seeded clutter and roughness. Editor-hidden surfaces remain acoustically active. Early paths use the engine's 0.75 s delay horizon. Late energy is accumulated by ray tracing without histogram extrapolation. The late field is spatially averaged over the tracer's receiver spheres; smaller grid cells do not imply equally fine physical tail resolution.

This is a geometric-acoustics energy model, without wave modes or phase-interference nulls. Use the plug-in's audio output for listening and final tail assessment.

Changes to sources, materials or analysis settings mark the previous result **OUTDATED**. Press Analyze to update it. Moving the listener only moves its marker because the map already samples fixed spatial positions.

The diffuse late field can be nearly uniform, producing similar orange/red colours on a wide scale. A narrow scale reveals differences but also magnifies Monte Carlo noise. Increase Total tail rays and repeat analysis to assess texture stability; check the numeric dB values in the legend and hover readout.
