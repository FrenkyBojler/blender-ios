# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import hashlib
from pathlib import Path

from . import types

# Chunk size of the hashing process, in bytes.
HASH_BLOCK_SIZE = 1024 * 1024

# Hashes that have not been 'used' in this many days are removed from the database.
# A 'use' means actually storing/updating the hash itself, or seeing that the
# stats (file size & mtime) still match the file on disk.
HASH_RETAIN_AGE_DAYS = 180


class DiskFileHashService:
    backend: types.DiskFileHashBackend

    def __init__(self, backend: types.DiskFileHashBackend) -> None:
        self.backend = backend
        self._is_open = False

    def open(self) -> None:
        """Prepare the service for use."""
        self.backend.open()
        self._is_open = True

    def close(self) -> None:
        """Close the service."""

        if not self._is_open:
            # Support closing of a never-opened service.
            return

        # Remove (potentially) outdated hashes. This is done on close, and not
        # on open, to give Blender the time to query files it needs.
        #
        # TODO: as a future improvement, we could investigate (instead of
        # delete) hashes that are older than X days. If they reference files
        # that still exist on disk, for which the cached entry is still valid
        # (given size in bytes & mtime), the cache entry could be marked as
        # 'freshly checked' instead of removed.
        self.backend.remove_older_than(days=HASH_RETAIN_AGE_DAYS)
        self.backend.close()

        self._is_open = False

    def get_hash(self, filepath: Path, hash_algorithm: str) -> str:
        """Return the hash of a file on disk."""
        cached_info = self.backend.fetch_hash(filepath, hash_algorithm)
        if cached_info:
            if self._file_stat_matches(filepath, cached_info.file_size_bytes, cached_info.file_stat_mtime):
                # Cached hash is still fresh.
                self.backend.mark_hash_as_fresh(filepath, hash_algorithm)
                return cached_info.hexhash

        # Hash the actual file on disk & store in the back-end.
        fresh_info = self._hash_file(filepath, hash_algorithm)
        self.backend.store_hash(filepath, hash_algorithm, fresh_info)
        return fresh_info.hexhash

    def store_hash(self, filepath: Path, hash_algorithm: str, hexhash: str) -> None:
        """Store a pre-computed hash for the given file path.

        The file should exist on disk, to obtain its size and last-modified
        timestamp. Its contents are not accessed. The caller is trusted to
        provide the correct hash.
        """
        stat = filepath.stat()
        hash_info = types.FileHashInfo(
            hexhash=hexhash,
            file_size_bytes=stat.st_size,
            file_stat_mtime=stat.st_mtime,
        )
        self.backend.store_hash(filepath, hash_algorithm, hash_info)

    def file_matches(self, filepath: Path, hash_algorithm: str, hexhash: str, size_in_byes: int) -> bool:
        """Check the file on disk, to see if it matches the given properties."""

        # Check the file size first, if it doesn't match we don't have to bother with the hash.
        stat = filepath.stat()
        if stat.st_size != size_in_byes:
            return False

        actual_hash = self.get_hash(filepath, hash_algorithm)
        return actual_hash == hexhash

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

    def _get_hasher(self, algorithm: str) -> hashlib._Hash:
        """Construct a hasher for the given hash algorithm.

        The algorithm should be chosen from hashlib.algorithms_available.
        """
        if algorithm not in hashlib.algorithms_available:
            available = ", ".join(sorted(hashlib.algorithms_available))
            raise ValueError("Hash algorithm {!r} not available ({!r})".format(
                algorithm, available))

        return hashlib.new(algorithm, usedforsecurity=False)
