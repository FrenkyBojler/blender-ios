# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import hashlib
from pathlib import Path

from . import types


class DiskFileHashService:
    backend: types.DiskFileHashBackend

    def __init__(self, backend: types.DiskFileHashBackend) -> None:
        self.backend = backend

    def _get_hasher(self, algorithm: str) -> hashlib._Hash:
        if algorithm not in hashlib.algorithms_available:
            available = ", ".join(sorted(hashlib.algorithms_available))
            raise ValueError("Hash algorithm {!r} not available ({!r})".format(
                algorithm, available))

        return hashlib.new(algorithm, usedforsecurity=False)

    def get_hash(self, filepath: Path, hash_algorithm: str) -> str:
        """Return the cached hash info of a given file."""
        cached_info = self.backend.fetch_hash(filepath, hash_algorithm)
        if cached_info:
            pass
        raise NotImplementedError()

    def store_hash(self, filepath: Path, hash_algorithm: str, hexhash: str) -> None:
        """Store a pre-computed hash for the given file path."""
        stat = filepath.stat()
        raise NotImplementedError()

    def file_matches(self, filepath: Path, hash_algorithm: str, hexhash: str, size_in_byes: int) -> bool:
        """Check the file on disk, to see if it matches the given properties."""
        raise NotImplementedError()

    def _file_stat_matches(self, filepath: Path, size_in_bytes: int, file_stat_mtime: float) -> bool:
        """Check whether the file on disk matches this size & timestamp."""
        try:
            stat = filepath.stat()
        except FileNotFoundError:
            return False
        return stat.st_size == size_in_bytes and stat.st_mtime == file_stat_mtime
