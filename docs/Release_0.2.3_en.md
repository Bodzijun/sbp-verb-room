# Verb Room 0.2.3

Verb Room 0.2.3 adds corner snapping and repair, Ambisonics output controls and clearer recording and baking controls. Release targets are Windows x64, macOS Universal (Apple silicon and Intel) and Linux x86_64 (classic interface only).

## What is fixed and new

### Sound / engine

- **Ambisonics output.** The web and classic editors offer **Ambisonics normalization** (SN3D by default, N3D or FuMa) and **Ambisonics level** (0, −3 or −6 dB). Keep I/O preserves both settings. WAV export uses the same channel order and level as live output. FuMa needs 4, 9 or 16 outputs for first, second or third order. A short bus silences slots whose components are missing, and the editor warns about it.
- **Measured level in the F8 test room.** With SN3D at 0 dB, W carried about 1.8 dB more energy than the sum of the stereo ORTF channels. Summing the energy across all Ambisonics channels gave +4.8, +6.5 and +7.7 dB for orders 1, 2 and 3 relative to ORTF. These figures describe one test room and measurement window.
- **Internal tests.** The full plug-in test suite runs with the standard 1 MiB stack.

### Interface

- **Corner snapping and repair.** While dragging a corner or wall, a visible stationary corner within 9 px acts as a magnet, even with **Snap** off. Hold Alt for free dragging. In **Edit room → Scene → Room**, **Open corners** counts nearby gaps and **Join corners** closes them with a 1, 5 or 10 cm tolerance. Open corners can leak sound and slow Bake. The repair covers wall endpoints. T-junctions in the middle of a wall and zone vertices need manual editing.
- **Recording and baking.** The **Path** block has two rows. **Host automation** contains **Record automation** and **Discard**, and **Bake late tail** contains **Bake path** and **Clear bake**. **Bake source** identifies the chosen input. After **Stop**, a usable recording selected for baking prompts you to press **Bake path**. The status messages distinguish an unused recording (`Recording kept, not used: …`) from live motion outside the baked recording (`Live motion is outside the baked recording - estimating the tail`). **Discard** uses the new `path.recordClear` command.
- **Small windows.** Mix and Decay scroll, including the last Decay slider and its diagnostics. The Pose column scrolls to expose path controls. **Clear selection** has room when the Outliner is open, and long selected values such as Layout, Microphones and Quality have tooltips. The minimum web content height is 560 px.

### Presets and compatibility

- Wall and zone coordinates are saved with full double precision. Older plug-in versions read these files, and older files still load. Presets and project states without Ambisonics output settings use SN3D and 0 dB.
- Moving **Manual** participants with earlier baked tables may need one fresh **Bake path**.

## Known limitations

- **Tables baked in 0.2.0 need one fresh Bake.** The estimator and cache changes in 0.2.1 mean these tables load as `notBaked`.
- **Tail direction.** Baked late tails retain their baked azimuth. Source directivity profiles act on direct sound and early reflections. The late tail treats sources as omnidirectional.
- **Round-robin early reflections.** Up to four sources are solved per 128-sample control period. With more sources, early reflections update in batches over 4–6 periods and glide linearly during motion.
- **Host verification.** Windows was tested in REAPER. A community tester tried 0.2.0 on a real Mac. Versions 0.2.1, 0.2.2 and 0.2.3 have yet to be tested on a Mac. Linux builds and passes CI tests with the classic interface and awaits testing in a real host.

For installation and switching interfaces, see the [0.2.0 release notes](Release_0.2.0_en.md).
