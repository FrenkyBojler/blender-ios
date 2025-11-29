# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path


def hash_file(filepath: Path) -> str:
    """Computes and returns the hash of the file.

    The returned string is prefixed with the hash type, like "{TYPE}:{HASH}".
    """
    return 'SHA256:' + _sha256_file(filepath)


def _sha256_file(filepath: Path) -> str:
    """Computes and returns the SHA256 hash of the file."""
    import hashlib

    sha256_hash = hashlib.sha256()

    file_size_bytes = 0
    with open(filepath, "rb") as f:
        for byte_block in iter(lambda: f.read(4096), b""):
            file_size_bytes += len(byte_block)
            sha256_hash.update(byte_block)
    return sha256_hash.hexdigest()
