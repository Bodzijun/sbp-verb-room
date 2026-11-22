# Third-Party Directivity Datasets & Attributions

This document lists the third-party sound-source directivity datasets and first-principles acoustic models embedded in **Verb Room** (`src/directivity_builtins.inc` and `src/directivity.hpp`), together with their required Creative Commons Attribution 4.0 International (`CC BY 4.0`) notices and modification statements.

---

## 1. Embedded Measured Datasets (`CC BY 4.0`)

### 1.1 Human Speech Directivity — Sentence Average (`th_koeln_voice_sentence`)
- **Authors:** Christoph Pörschmann (TH Köln — University of Applied Sciences)
- **Title:** *Supplementary material for "A database for the comparison of measured datasets of human voice directivity"*
- **Year:** 2023
- **Source / URI:** https://doi.org/10.5281/zenodo.7834211 (`Voice_Directivity_sentence.zip`, sparse 32-microphone pentakis-dodecahedron array, 13 subjects)
- **Related Publication:** Pörschmann, C., & Arend, J. M. (2023), *"A database for the comparison of measured datasets of human voice directivity"*, Proceedings of Forum Acusticum 2023.
- **Licence:** Creative Commons Attribution 4.0 International (**CC BY 4.0** — https://creativecommons.org/licenses/by/4.0/)
- **SOFA Global `License` Attribute Verified:** `"CC BY 4.0"`
- **Modifications Made:** Only the authors' own `Voice_Directivity_sentence.zip` measurements were used (`Database.zip` containing third-party comparison studies was excluded). Complex transfer functions (`GeneralTF`) were integrated over the 7 octave bands (`125, 250, 500, 1000, 2000, 4000, 8000 Hz`), unit-power normalized per subject, averaged across all 13 talkers without subject anthropometry, projected onto 4π-orthonormal real spherical harmonics of order $N = 4$, and normalized to unit mean spherical power per octave band. Provided without warranty of any kind.

### 1.2 Human Speech Directivity — 8 Phonemes Average (`th_koeln_voice_phonemes`)
- **Authors:** Christoph Pörschmann and Johannes M. Arend (TH Köln — University of Applied Sciences)
- **Title:** *Supplementary material for "Investigating phoneme-dependencies of spherical voice directivity patterns"*
- **Year:** 2021
- **Source / URI:** https://doi.org/10.5281/zenodo.4739183 (`Voice_Directivities_Sparse.zip`, sparse 32-microphone pentakis-dodecahedron array, 13 subjects × 2 repetitions × 8 phonemes `[a, e, i, o, u, f, s, sch]`)
- **Related Publication:** Pörschmann, C., & Arend, J. M. (2021), *"Investigating phoneme-dependencies of spherical voice directivity patterns"*, The Journal of the Acoustical Society of America, 149(6), 4553–4564 (https://doi.org/10.1121/10.0005401).
- **Licence:** Creative Commons Attribution 4.0 International (**CC BY 4.0** — https://creativecommons.org/licenses/by/4.0/)
- **SOFA Global `License` Attribute Verified:** `"CC BY 4.0"`
- **Modifications Made:** Complex transfer functions (`GeneralTF`) were integrated over the 7 octave bands (`125..8000 Hz`), unit-power normalized per phoneme and talker, averaged across all 8 phonemes and 26 subject/repetition recordings without personal anthropometry, projected onto 4π-orthonormal real spherical harmonics of order $N = 4$, and normalized to unit mean spherical power per octave band. Provided without warranty of any kind.

### 1.3 Studio Monitor — Genelec 8030C (`genelec_8030c`)
- **Authors:** Felix Stärz, Armin Budnik, and Matthias Blau (Institut für Hörtechnik und Audiologie, Jade Hochschule Oldenburg)
- **Title:** *Loudspeaker Directivity Dataset Genelec 8030C*
- **Year:** 2026
- **Source / URI:** https://doi.org/10.5281/zenodo.21486919 (`Genelec_8030C_SourceDirectivity_minPhase.sofa`, 5° × 5° spherical grid, 2664 impulse responses at 44.1 kHz)
- **Licence:** Creative Commons Attribution 4.0 International (**CC BY 4.0** — https://creativecommons.org/licenses/by/4.0/)
- **SOFA Global `License` Attribute Verified:** `"CC BY 4.0"`
- **Modifications Made:** Impulse responses (`GeneralFIR`) were transformed via FFT and integrated over the 7 octave bands (`125..8000 Hz`), with the `125 Hz` band blended 50 % toward omnidirectional below the 160 Hz anechoic chamber cutoff, projected onto 4π-orthonormal real spherical harmonics of order $N = 4$ with $\cos(\text{elevation})$ quadrature weighting, and normalized to unit mean spherical power per octave band. Provided without warranty of any kind.

---

## 2. First-Principles Analytic Models (`(model)`)

The following profiles in `src/directivity.hpp` are computed directly from textbook acoustic radiation theory (Morse & Ingard, *Theoretical Acoustics*, §7.2: vibrating circular cap on a rigid sphere) and contain no third-party measurement data:
- `voice_mouth_sphere_model` — **Voice — mouth on a sphere (model)** ($R = 8.75\text{ cm}$ head sphere, $a = 1.5\text{ cm}$ mouth aperture)
- `brass_trumpet_bell_model` — **Brass — trumpet bell, r = 6.2 cm (model)**
- `brass_trombone_bell_model` — **Brass — trombone bell, r = 10.8 cm (model)**
- `brass_horn_bell_model` — **Brass — horn bell, r = 15.0 cm (model)**
- `brass_tuba_bell_model` — **Brass — tuba bell, r = 20.0 cm (model)**

---

## 3. Datasets Inspected and Excluded Due to Licence Restrictions

Per project policy and theE13 brief ("If a permitted set cannot be downloaded or its licence text on the download page differs from R3, do not embed it — report instead"):
- **Aalto Four Loudspeakers (`Genelec 8030B`, `Genelec 8331A`, `OmniSource`, Zenodo `10.5281/zenodo.10255555`):** Although the Zenodo landing page metadata states `cc-by-4.0`, inspecting the global HDF5/SOFA `License` attribute embedded inside `LS_directivity_Calibrated_GENELEC_8030B.sofa` and `LS_directivity_Calibrated_GENELEC_8331A.sofa` revealed the literal string:
  > `License = "No license provided, ask the author for permission"`
  Because the internal SOFA licence attribute contradicts the Zenodo wrapper metadata, these files are **not embedded** in the shipped plugin. Users who download them directly may convert them offline with `verbdir_tool`.
- **TU Berlin Musical Instruments (`10.14279/depositonce-19858`):** Excluded (`CC BY-NC-SA 4.0`).
- **BYU Musical Instrument Directivity:** Excluded (no formal open licence).
- **SONICOM Databases #29 / #80:** Excluded (`CC BY-SA 4.0` ShareAlike copyleft).
- **Ahrens / Chalmers (`10.5281/zenodo.3707708`):** Excluded (`CC BY-NC-SA 4.0` in repository README and contains NC upstream data).
