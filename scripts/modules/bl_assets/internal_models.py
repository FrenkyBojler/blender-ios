# SPDX-FileCopyrightText: 2011-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Data models internally used by Blender.

These are "internal" in contrast to the API models in
`blender_asset_library_openapi.py`.
"""

import hashlib
from pathlib import Path

import pydantic


class BlendFile(pydantic.BaseModel):
    filepath: Path
    size: int
    sha256: str

    def __init__(self, filepath: Path) -> None:
        super().__init__(
            filepath=filepath,
            # This can cause an OSError, which the caller should know about.
            size=self.filepath.stat().st_size,
            sha256=self.get_sha256(),
        )

    def get_sha256(self) -> str:
        """Computes and returns the SHA256 hash of the file."""
        sha256_hash = hashlib.sha256()

        with open(self.filepath, "rb") as f:
            for byte_block in iter(lambda: f.read(4096), b""):
                sha256_hash.update(byte_block)
        return sha256_hash.hexdigest()
