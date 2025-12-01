# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import typing
from pathlib import Path

if typing.TYPE_CHECKING:
    from _bpy_internal.assets.remote_library_listing.blender_asset_library_openapi import URLWithHashV1 as _URLWithHashV1
else:
    _URLWithHashV1 = object


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


def url(url_with_hash: _URLWithHashV1) -> str:
    """Return the url, with the hash on the query string.

    >>> url(URLWithHashV1(url="http://localhost/", hash="sha256:the-hash"))
    'http://localhost/?hash=the-hash'
    """

    import urllib.parse

    url = url_with_hash.url
    hash_with_type = url_with_hash.hash

    if not hash_with_type:
        return url

    try:
        _, hash_value = hash_with_type.split(':', 1)
    except ValueError:
        # This means the hash is not in the form '{TYPE}:{HASH}'; just use it as-is.
        hash_value = hash_with_type

    sep = '&' if '?' in url else '?'
    return url + sep + 'hash=' + urllib.parse.quote(hash_value)
