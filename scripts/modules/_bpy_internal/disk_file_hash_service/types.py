# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
from typing import Protocol
import dataclasses


__all__ = (
    'DiskFileHashBackend',
    'FileHashInfo',
)


@dataclasses.dataclass
class FileHashInfo:
    hexhash: str
    file_size_bytes: int
    file_stat_mtime: float


class DiskFileHashBackend(Protocol):
    def open(self) -> None:
        """Prepare the back-end for use."""

    def close(self) -> None:
        """Close the back-end.

        After calling this, the back-end is not expected to work any more.
        """

    def fetch_hash(self, filepath: Path, hash_algorithm: str) -> FileHashInfo | None:
        """Return the cached hash info of a given file.

        If no info is cached for this path/algorithm combo, returns None.
        """

    def store_hash(self, filepath: Path, hash_algorithm: str, hash_info: FileHashInfo) -> None:
        """Store a pre-computed hash for the given file path."""

    def remove_file(self, filepath: Path) -> None:
        """Remove all information about this file."""
