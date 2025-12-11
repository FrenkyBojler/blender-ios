# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import hashlib
from pathlib import Path

from . import types

# Chunk size of the hashing process, in bytes.
HASH_BLOCK_SIZE = 1024 * 1024


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

    def open(self) -> None:
        """Prepare the service for use."""
        self.backend.open()

    def close(self) -> None:
        """Close the service."""
        self.backend.close()

    def get_hash(self, filepath: Path, hash_algorithm: str) -> str:
        """Return the cached hash info of a given file."""
        cached_info = self.backend.fetch_hash(filepath, hash_algorithm)
        if cached_info:
            if self._file_stat_matches(filepath, cached_info.file_size_bytes, cached_info.file_stat_mtime):
                return cached_info.hexhash

        fresh_info = self._hash_file(filepath, hash_algorithm)
        self.backend.store_hash(filepath, hash_algorithm, fresh_info)
        return fresh_info.hexhash

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

    def _hash_file(self, filepath: Path, hash_algorithm: str) -> types.FileHashInfo:
        stat = filepath.stat()

        hasher = self._get_hasher(hash_algorithm)
        with filepath.open(mode="rb") as infile:
            while block := infile.read(HASH_BLOCK_SIZE):
                hasher.update(block)

        return types.FileHashInfo(
            hexhash=hasher.hexdigest(),
            file_size_bytes=stat.st_size,
            file_stat_mtime=stat.st_mtime,
        )
