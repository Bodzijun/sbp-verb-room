"""Package a CI-built VST3, preserving bundle permissions and recording its source."""
import argparse
import hashlib
import json
import platform
import re
import shutil
import subprocess
import tarfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--platform", choices=("macos-universal", "linux-x86_64"), required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent.parent
    expected_os = "Darwin" if args.platform == "macos-universal" else "Linux"
    if platform.system() != expected_os:
        raise RuntimeError(f"Package {args.platform} on {expected_os}, not {platform.system()}")
    version = re.search(r"project\(verb VERSION ([\d.]+)", (source / "CMakeLists.txt").read_text()).group(1)
    bundle = args.build_dir.resolve() / "plugin/VerbPlugin_artefacts/Release/VST3/Verb Room.vst3"
    if not bundle.is_dir():
        raise FileNotFoundError(bundle)
    cache = (args.build_dir.resolve() / "CMakeCache.txt").read_text()
    avx2 = re.search(r"^VERB_AVX2:BOOL=(.+)$", cache, re.MULTILINE)
    if not avx2 or avx2.group(1).strip().upper() not in ("OFF", "FALSE", "0", "NO"):
        raise RuntimeError("These portable packages require a build configured with VERB_AVX2=OFF")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    name = f"VerbRoom-{version}-{args.platform}"
    # A fresh stage prevents stale files from a previous package entering the archive.
    stage = args.build_dir.resolve() / "packages" / name
    stage.parent.mkdir(parents=True, exist_ok=True)
    if stage.exists():
        raise FileExistsError(f"Package stage already exists: {stage}")
    stage.mkdir()
    shutil.copytree(bundle, stage / bundle.name, symlinks=True)
    for filename in ("LICENSE", "README.md"):
        shutil.copy2(source / filename, stage / filename)
    shutil.copytree(source / "docs", stage / "docs")
    shutil.copytree(source / "presets", stage / "Presets")
    info = {"version": version, "platform": args.platform, "commit": commit,
            "avx2_required": False, "apple_notarized": False if expected_os == "Darwin" else None}
    (stage / "BUILDINFO.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    if expected_os == "Darwin":
        instructions = (
            "macOS Universal (arm64 + x86_64), deployment target macOS 11.\n"
            "Close your DAW and copy the whole Verb Room.vst3 bundle to\n"
            "~/Library/Audio/Plug-Ins/VST3/ or /Library/Audio/Plug-Ins/VST3/.\n"
            "This test build is ad-hoc signed, not Developer ID signed or Apple-notarized.\n"
            "macOS may block a downloaded plug-in. This package is for compatibility testing.\n"
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
    if expected_os == "Darwin":
        subprocess.run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", str(stage), str(archive)], check=True)
    else:
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(stage, arcname=name)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (output / f"{archive.name}.sha256").write_text(f"{digest}  {archive.name}\n", encoding="ascii")
    print(f"Packaged {archive.name} from {commit}")


if __name__ == "__main__":
    main()
