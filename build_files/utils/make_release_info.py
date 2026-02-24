#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2021-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

__all__ = (
    "main",
)

import json
import make_utils
import platform
import re
import sys
from pathlib import Path


class BlenderVersion:
    def __init__(self):
        self.version = make_utils.parse_blender_version()
        self.parse_source()

    def parse_source(self):
        blender_srcdir = Path(__file__).absolute().parent.parent.parent
        version_path = blender_srcdir / "source/blender/blenkernel/BKE_blender_version.h"

        content = version_path.read_text(encoding="utf-8")

        # Detect LTS
        suffix_match = re.search(
            r'#define\s+BLENDER_VERSION_SUFFIX\s+([A-Za-z0-9_"]+)',
            content
        )
        if suffix_match:
            suffix_value = suffix_match.group(1).strip('"')
            self.is_lts = suffix_value == "LTS"
        else:
            self.is_lts = False

        # Extract log line
        logline_match = re.search(
            r'#define\s+BLENDER_VERSION_LOGLINE\s+"([^"]*)"',
            content
        )
        if logline_match:
            self.log_line = logline_match.group(1)
        else:
            self.log_line = ""

    @property
    def major(self):
        return self.version.version // 100

    @property
    def minor(self):
        return self.version.version % 100

    @property
    def patch(self):
        return self.version.patch

    @property
    def cycle(self):
        return self.version.cycle

    def __str__(self):
        return f"{self.major}.{self.minor}.{self.patch}"


def get_platform_suffix(platform_name: str) -> str:
    lookup = {
        "win32": "windows",
        "darwin": "macos",
        "linux": "linux",
    }
    return lookup.get(platform_name)


def get_platform_file_format(platform_name: str) -> str:
    lookup = {
        "win32": "msi",
        "darwin": "dmg",
        "linux": "tar.xz",
    }
    return lookup.get(platform_name)


def get_composed_platform(platform_name: str, architecture: str) -> str:
    """Return a combination of the platform and the architecture (e.g., macos-arm64)"""
    platform_suffix = get_platform_suffix(platform_name)
    return f"{platform_suffix}-{architecture}"


def get_download_filepath(version: BlenderVersion, platform_name: str, architecture: str) -> str:
    file_format = get_platform_file_format(platform_name)
    composed_platform = get_composed_platform(platform_name, architecture)
    return f"blender-{version.major}.{version.minor}.{version.patch}-{composed_platform}.{file_format}/"


def get_json_filepath(version: BlenderVersion, platform_name: str, architecture: str) -> str:
    composed_platform = get_composed_platform(platform_name, architecture)
    return f"blender-{version.major}.{version.minor}.{version.patch}-{composed_platform}.json"


def get_download_url(version: BlenderVersion, platform_name: str, architecture: str) -> str:
    """Returns the complete download URL

    This is the URL that will lead you to the mirrored page and then to the Thank you page."""

    file_path = get_download_filepath(version, platform_name, architecture)
    download_url = f"https://www.blender.org/download/release/Blender{version.major}.{version.minor}/{file_path}"
    return download_url


def get_release_notes_url(version: BlenderVersion) -> str:
    """Return the final release notes website

    Note: until this release is out blender.org servers will redirect this URL
    to the Release Notes on the Blender Developer Documentation instead."""

    release_notes = f"https://www.blender.org/download/releases/{version.major}-{version.minor}/"
    return release_notes


def validate_description(description: str) -> None:
    """Make sure release has a valid description"""
    if not description:
        print("Error: No description found for this release.")
        sys.exit(1)

    if not description.endswith("."):
        print("Error: Description should always end with \".\"")
        sys.exit(1)


def main() -> None:
    blender_version = BlenderVersion()

    platform_name = sys.platform
    architecture = platform.uname().machine
    description = blender_version.log_line

    validate_description(description)

    data = {
        "download_url": get_download_url(blender_version, platform_name, architecture),
        "release_notes": get_release_notes_url(blender_version),
        "platform": get_composed_platform(platform_name, architecture),
        "version": str(blender_version),
        "is_lts": blender_version.is_lts,
        "description": description,
        "cycle": blender_version.cycle,
    }

    json_filepath = get_json_filepath(blender_version, platform_name, architecture)

    print(f"Release Info saved to: {json_filepath}")
    print(json.dumps(data, indent=2, ensure_ascii=False))

    with open(json_filepath, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False)


if __name__ == "__main__":
    import doctest

    if doctest.testmod().failed:
        raise SystemExit("ERROR: Self-test failed, refusing to run")

    main()
