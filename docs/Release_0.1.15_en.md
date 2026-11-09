# Verb Room 0.1.15

- Added 48 / 64 / 96-cell resolution options to the Energy analyzer. These calculate additional physical probe positions rather than enlarging the existing map image.
- The default remains 24 cells on the longest side. Larger grids take longer to analyze; audio processing is unchanged.
- Added a tooltip explaining the resolution/quality tradeoff. Late-field detail also depends on ray count and spatial averaging.
- Extended Colour range to 0.5–120 dB, with fractional legend labels for narrow ranges. This makes subtle late-field variation visible.
- Added **Auto** contrast based on the current band's lit-cell energy distribution. Band and contrast changes require no retracing.

See the [analyzer guide](Energy_analyzer_0.1.14_en.md). Preset and project formats are unchanged.
