# Verb Room 0.1.10 — mixer updates, bigger orchestra presets and preset sharing

Hi everyone! Here is an update covering the recent Verb Room changes.

- **A more compact source mixer:** switch between Compact and Normal channel strips, with automatable Solo/Mute for each source. Multiple solos are supported. Muting stops new input from that source while the existing reverb continues to decay naturally.
- **Two larger orchestral presets:** VSL Grand Hall — Audience and Conductor, with 32 stereo groups across 64 inputs. The editable hall has a faceted, asymmetric shape, stage risers, balconies and overhead reflectors. It is designed for dry orchestral libraries; VSL instruments and samples are not included.
- **Complete preset sharing:** Save room now creates a `.verbroom` file containing the scene, source/input setup, output configuration, mixer settings and tail quality. You can send that file to another user and open it through Load from file. Older `.txt` scenes still work.
- **Keep I/O:** preserve your current source setup and output configuration when trying another room, or turn it off to load the full setup from the preset.
- **Hall correction:** both rear bevels in VSL Grand Hall now use the same absorbing material as the central rear wall.

The earlier long-tail fix is included too: Cathedral Long should no longer stop after only a few seconds because of the initial tail length reported to the host. The engine now supports room-dependent tails up to 20 seconds.

Close your DAW before updating. Existing projects keep their saved geometry, so reselect VSL Grand Hall to get the material correction. The 32-group presets need 64 host input channels; DAW/Vienna routing is configured separately.

Source: https://github.com/Bodzijun/sbp-verb-room

Feedback and shared room presets are welcome!
