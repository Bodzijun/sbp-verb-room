# Binary package documentation

The primary README and installation instructions inside downloadable VST3 archives are English. Optional Ukrainian guides keep the `_uk.md` suffix; English guides use `_en.md`.

`ci/package_vst3.py` automatically generates the English README for macOS and Linux through `ci/binary_readme.py`. It keeps platform-specific installation requirements in `INSTALL.txt`.

When preparing a Windows package manually, generate its README after copying the plug-in, license, presets and documentation into the staging directory:

```powershell
python ci/binary_readme.py publish/release/VerbRoom-0.1.15-win64 --version 0.1.15 --platform win64
```

Use the version and staging path for the build being packaged. Write `INSTALL.txt` in English, then create the ZIP from that staging directory. Keep the entire VST3 bundle intact.

After changing documentation in an existing archive, verify that all VST3 bundle files still have the same hashes and regenerate the archive's SHA-256 checksum. Changing only documentation does not require replacing an installed plug-in.
