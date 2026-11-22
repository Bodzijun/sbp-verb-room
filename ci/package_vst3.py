"""Package a CI-built VST3, preserving bundle permissions and recording its source."""
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import tarfile
from pathlib import Path
from binary_readme import write_binary_readme


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--platform", choices=("win64", "macos-universal", "linux-x86_64"), required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent.parent
    expected_os = {"win64": "Windows", "macos-universal": "Darwin", "linux-x86_64": "Linux"}[args.platform]
    windows = expected_os == "Windows"
    if platform.system() != expected_os:
        raise RuntimeError(f"Package {args.platform} on {expected_os}, not {platform.system()}")
    version = re.search(r"project\(verb VERSION ([\d.]+)", (source / "CMakeLists.txt").read_text()).group(1)
    bundle = args.build_dir.resolve() / "plugin/VerbPlugin_artefacts/Release/VST3/Verb Room.vst3"
    if not bundle.is_dir():
        raise FileNotFoundError(bundle)
    cache = (args.build_dir.resolve() / "CMakeCache.txt").read_text()
    avx2 = re.search(r"^VERB_AVX2:BOOL=(.+)$", cache, re.MULTILINE)
    if windows and (not avx2 or avx2.group(1).strip().upper() not in ("ON", "TRUE", "1", "YES")):
        raise RuntimeError("The Windows x64 package requires a build configured with VERB_AVX2=ON")
    if not windows and (not avx2 or avx2.group(1).strip().upper() not in ("OFF", "FALSE", "0", "NO")):
        raise RuntimeError("These portable packages require a build configured with VERB_AVX2=OFF")
    commit = os.environ.get("GITHUB_SHA")
    if not windows and not commit:
        raise RuntimeError("Set GITHUB_SHA to the source revision for BUILDINFO.json")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    name = f"VerbRoom-{version}-{args.platform}"
    # A fresh stage prevents stale files from a previous package entering the archive.
    stage = args.build_dir.resolve() / "packages" / name
    stage.parent.mkdir(parents=True, exist_ok=True)
    if stage.exists():
        raise FileExistsError(f"Package stage already exists: {stage}")
    stage.mkdir()
    shutil.copytree(bundle, stage / bundle.name, symlinks=True,
                    ignore=shutil.ignore_patterns("*.map") if windows else None)
    shutil.copy2(source / "LICENSE", stage / "LICENSE")
    shutil.copy2(source / "THIRD_PARTY_DIRECTIVITY.md", stage / "THIRD_PARTY_DIRECTIVITY.md")
    shutil.copytree(source / "docs", stage / "docs")
    shutil.copytree(source / "presets", stage / "Presets")
    web_on = re.search(r"^VERB_WEB_UI:BOOL=(.+)$", cache, re.MULTILINE)
    web_on = bool(web_on) and web_on.group(1).strip().upper() in ("ON", "TRUE", "1", "YES")
    # Web UI backend compiled in: system WKWebView on macOS; Linux is built without a web UI (classic interface only).
    web_backend = "WKWebView" if (expected_os == "Darwin" and web_on) else "none"
    info = {"version": version, "platform": args.platform, "commit": commit, "web": web_backend,
            "avx2_required": False, "apple_notarized": False if expected_os == "Darwin" else None,
            "apple_signing": "ad-hoc" if expected_os == "Darwin" else None}
    if not windows:
        (stage / "BUILDINFO.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    if windows:
        instructions = (
            f"Verb Room {version} - Windows x64 (AVX2/FMA)\n"
            'Close your DAW and copy the whole "Verb Room.vst3" folder to\n'
            "C:\\Program Files\\Common Files\\VST3\\.\n"
            "Requirements: Windows 10/11 64-bit with AVX2/FMA.\n"
            "The web interface requires the Microsoft WebView2 Runtime. Without it the classic interface opens.\n"
            "To use the classic interface: open Theme -> Classic UI or set VERB_UI=classic.\n"
            f"See docs/Release_{version}_en.md for changes and compatibility notes.\n"
        )
        archive = output / f"{name}.zip"
    elif expected_os == "Darwin":
        instructions = (
            "macOS Universal (arm64 + x86_64), deployment target macOS 11.\n"
            "Close your DAW and copy the whole Verb Room.vst3 bundle to\n"
            "~/Library/Audio/Plug-Ins/VST3/ or /Library/Audio/Plug-Ins/VST3/.\n"
            "This build is ad-hoc signed, not Developer ID signed or Apple-notarized.\n"
            "macOS may block a downloaded plug-in. To allow it, right-click the .vst3 in Finder and choose Open,\n"
            "or run in Terminal (adjust the path):\n"
            "  xattr -dr com.apple.quarantine \"/Library/Audio/Plug-Ins/VST3/Verb Room.vst3\"\n"
            + ("The new web interface (system WKWebView) opens by default.\n" if web_backend != "none" else "The classic interface is used.\n") +
            "To use the classic interface: open the Theme menu and press 'Classic UI' (remembered),\n"
            "or start the host with the environment variable VERB_UI=classic.\n"
            "Preference file: ~/Library/SBP/VerbRoom/editor.ini (delete it to restore the default).\n"
            "The web interface on macOS is a first release; please report macOS version, Mac model and host.\n"
        )
        archive = output / f"{name}.zip"
    else:
        instructions = (
            "Linux x86_64, built on Ubuntu 24.04 without requiring AVX2/FMA.\n"
            "Requires compatible system libraries (glibc 2.39 or newer); older distributions are not targeted.\n"
            "Close your DAW and copy the whole Verb Room.vst3 directory to ~/.vst3/.\n"
            "Extract the tar.gz archive to preserve executable permissions.\n"
        )
        archive = output / f"{name}.tar.gz"
    instructions += (
        "Reopen the DAW and rescan plug-ins if needed. Actual host compatibility requires testing.\n"
        "Presets are in Presets/; see docs/Presets_0.1.10_en.md for import and Keep I/O.\n"
        "This package does not contain orchestral instruments or samples.\n"
    )
    (stage / "INSTALL.txt").write_text(instructions, encoding="utf-8")
    write_binary_readme(stage, version, args.platform)
    if windows:
        shutil.make_archive(str(output / name), "zip", root_dir=stage.parent, base_dir=name)
    elif expected_os == "Darwin":
        subprocess.run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", str(stage), str(archive)], check=True)
    else:
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(stage, arcname=name)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (output / f"{archive.name}.sha256").write_text(f"{digest}  {archive.name}\n", encoding="ascii")
    print(f"Packaged {archive.name}" + (f" from {commit}" if commit else ""))


if __name__ == "__main__":
    main()
