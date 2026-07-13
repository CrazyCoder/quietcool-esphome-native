#!/usr/bin/env python3
"""Generate the ESPHome managed-update manifest for the staged dist image."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
VERSION_RE = re.compile(r"[0-9A-Za-z][0-9A-Za-z.+_-]{0,31}")
MD5_RE = re.compile(r"[0-9a-f]{32}")


def read_scalar(path: Path) -> str:
    value = path.read_text(encoding="utf-8").strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        value = value[1:-1]
    return value


def generate(
    version_path: Path,
    md5_path: Path,
    output_path: Path,
    previous_path: Path | None = None,
) -> None:
    version = read_scalar(version_path)
    if VERSION_RE.fullmatch(version) is None:
        raise ValueError(f"invalid firmware version in {version_path}: {version!r}")

    md5 = md5_path.read_text(encoding="ascii").strip().lower()
    if MD5_RE.fullmatch(md5) is None:
        raise ValueError(f"invalid firmware MD5 in {md5_path}: {md5!r}")

    manifest = {
        "name": "QuietCool Attic Fan Firmware",
        "version": version,
        "builds": [
            {
                "chipFamily": "ESP32",
                "ota": {
                    "path": "firmware.ota.bin",
                    "md5": md5,
                    "summary": "QuietCool replacement firmware update. See the release page for details.",
                    "release_url": "https://github.com/CrazyCoder/quietcool-esphome-native/releases/latest",
                },
            }
        ],
    }

    if previous_path is not None:
        previous = json.loads(previous_path.read_text(encoding="utf-8"))
        previous_version = previous.get("version")
        try:
            previous_md5 = previous["builds"][0]["ota"]["md5"]
        except (KeyError, IndexError, TypeError):
            previous_md5 = None
        if (
            previous_version == version
            and previous_md5 is not None
            and previous_md5.lower() != md5
        ):
            raise ValueError(
                "firmware bytes changed without a firmware-version.yaml bump: "
                f"published version is still {version}"
            )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=True) + "\n", encoding="ascii"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--version-file", type=Path, default=ROOT / "firmware-version.yaml"
    )
    parser.add_argument(
        "--md5-file",
        type=Path,
        default=ROOT / "web-installer" / "firmware.ota.bin.md5",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "web-installer" / "manifest.json",
    )
    parser.add_argument(
        "--previous-manifest",
        type=Path,
        help="reject changed firmware bytes when the published version was not bumped",
    )
    args = parser.parse_args()
    generate(
        args.version_file, args.md5_file, args.output, args.previous_manifest
    )


if __name__ == "__main__":
    main()
