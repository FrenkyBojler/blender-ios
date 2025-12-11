# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Protocol


class DiskFileHashService:
    backend: DiskFileHashBackend

    def __init__(self, backend: DiskFileHashBackend) -> None:
        self.backend = backend

    def _get_hasher(self, algorithm: str) -> hashlib._Hash:
        if algorithm not in hashlib.algorithms_available:
            available = ", ".join(sorted(hashlib.algorithms_available))
            raise ValueError("Hash algorithm {!r} not available ({!r})".format(
                algorithm, available))

        return hashlib.new(algorithm, usedforsecurity=False)


class DiskFileHashBackend(Protocol):
    def open(self) -> None:
        """Prepare the back-end for use."""
        pass

    def close(self) -> None:
        """Close the back-end.

        After calling this, the back-end is not expected to work any more.
        """
        pass

    def get_hash(self, filepath: Path, hash_algorithm: str) -> str:
        """Return the hash of the given file, as hex string."""
        return ""

    def store_hash(self, filepath: Path, hash_algorithm: str, hexhash: str) -> None:
        """Store a pre-computed hash for the given file path."""

    def file_matches(self, filepath: Path, hash_algorithm: str, hexhash: str, size_in_byes: int) -> bool:
        """Check the file on disk, to see if it matches the given properties."""
        return False
