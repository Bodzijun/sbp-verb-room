"""Generate the English README shipped inside a Verb Room binary package."""
import argparse
from pathlib import Path


def write_binary_readme(stage: Path, version: str, target: str) -> None:
    instructions = {
        "win64": (
            "Windows x64",
            "Copy the entire `Verb Room.vst3` folder to "
            "`C:\\Program Files\\Common Files\\VST3\\`.\n\n"
            "The Windows build requires an AVX2/FMA-capable CPU.",
        ),
        "macos-universal": (
            "macOS Universal (Apple Silicon and Intel)",
            "Copy the entire `Verb Room.vst3` bundle to "
            "`~/Library/Audio/Plug-Ins/VST3/` or `/Library/Audio/Plug-Ins/VST3/`.\n\n"
            "See INSTALL.txt for this build's macOS requirements and signing status.",
        ),
        "linux-x86_64": (
            "Linux x86_64",
            "Extract the tar.gz archive to preserve file permissions, then copy the entire "
            "`Verb Room.vst3` directory to `~/.vst3/`.\n\n"
            "See INSTALL.txt for this build's system-library requirements.",
        ),
    }
    platform_name, installation = instructions[target]
    guides = [
        ("Release notes", f"docs/Release_{version}_en.md"),
        ("Energy analyzer", "docs/Energy_analyzer_0.1.14_en.md"),
        ("Positioned dry and scene organization", "docs/Spatial_editor_0.1.12_en.md"),
        ("Preset exchange and Keep I/O", "docs/Presets_0.1.10_en.md"),
        ("Cross-platform builds", "docs/Cross_platform_builds_en.md"),
    ]
    links = [f"- [{label}]({path})" for label, path in guides if (stage / path).is_file()]
    text = (
        f"# Verb Room {version}\n\n"
        f"VST3 audio effect for **{platform_name}**.\n\n"
        "## Installation\n\n"
        "1. Save your project and close REAPER or your other DAW.\n"
        "2. Install the complete plug-in folder as described below. "
        "Do not copy only the executable inside it.\n"
        "3. Reopen your DAW and rescan VST3 plug-ins if necessary.\n\n"
        f"{installation}\n\n"
        "## Getting started\n\n"
        "Insert Verb Room on an audio track or bus. Select a room and match the host's "
        "track channels, sends and plug-in input pins to the sources configured in Verb Room. "
        "A room preset does not create DAW tracks or sends.\n\n"
        "Instrument libraries, orchestral samples and Vienna instruments are not included.\n"
    )
    if links:
        text += "\n## English guides\n\n" + "\n".join(links) + "\n"
    if (stage / "INSTALL.txt").is_file():
        text += "\nRead INSTALL.txt for package-specific instructions and requirements.\n"
    if any((stage / "docs").glob("*_uk.md")) or any(stage.glob("*_uk.md")):
        text += "\nFiles ending in `_uk.md` are optional Ukrainian versions of the documentation.\n"
    license_name = "LICENSE.txt" if (stage / "LICENSE.txt").is_file() else "LICENSE"
    text += f"\n## License\n\nGNU Affero General Public License v3.0. See {license_name}.\n"
    (stage / "README.md").write_text(text, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", choices=("win64", "macos-universal", "linux-x86_64"), required=True)
    args = parser.parse_args()
    if not args.stage.is_dir():
        parser.error("stage must be an existing package directory")
    write_binary_readme(args.stage, args.version, args.platform)
