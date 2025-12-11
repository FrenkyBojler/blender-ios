# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

"""
blender -b --factory-startup -P tests/python/bl_disk_file_hash_service_test.py  -- --verbose
"""

__all__ = (
    "main",
)

import unittest
from pathlib import Path

from _bpy_internal.disk_file_hash_service import backend_sqlite

scratch_dir: Path


class SQLiteBackendTest(unittest.TestCase):
    storagepath: Path
    backend: backend_sqlite.SQLiteBackend

    def setUp(self) -> None:
        self.storagepath = scratch_dir / "database"
        self.backend = backend_sqlite.SQLiteBackend(self.storagepath)

        # Delete the database between each test.
        self.backend.dbfile_path.unlink(missing_ok=True)

        self.backend.open()

    def tearDown(self) -> None:
        self.backend.close()

    def test_store_fetch_update_hash(self) -> None:
        # This function should trust the provided hash, and not access the file itself.
        filepath = Path("path-does-not-matter.blend")
        fake_hash = "Monkeys are not a hash, what is this?"
        fake_filesize = 100
        fake_mtime = 47.327
        self.backend.store_hash(filepath, "sha256", fake_hash, fake_filesize, fake_mtime)
        hash_info = self.backend.fetch_hash(filepath, "sha256")
        assert hash_info is not None
        (hash, filesize, mtime) = hash_info
        self.assertEqual(fake_hash, hash)
        self.assertEqual(fake_filesize, filesize)
        self.assertEqual(fake_mtime, mtime)

        # The info should be updatable.
        new_hash = "A new hash is new"
        new_filesize = 42
        new_mtime = 100.43
        self.backend.store_hash(filepath, "sha256", new_hash, new_filesize, new_mtime)
        hash_info = self.backend.fetch_hash(filepath, "sha256")
        assert hash_info is not None
        (hash, filesize, mtime) = hash_info
        self.assertEqual(new_hash, hash)
        self.assertEqual(new_filesize, filesize)
        self.assertEqual(new_mtime, mtime)

    def test_remove_file(self) -> None:
        filepath = Path("path-does-not-matter.blend")

        # Create an entry.
        self.backend.store_hash(filepath, "sha256", "hash-does-not-matter", 100, 47.327)
        hash_info = self.backend.fetch_hash(filepath, "sha256")
        self.assertIsNotNone(hash_info)

        # Delete the entry.
        self.backend.remove_file(filepath)

        # Check it does not exist any more.
        hash_info = self.backend.fetch_hash(filepath, "sha256")
        self.assertIsNone(hash_info)

    def test_fetch_nonexistent_file(self) -> None:
        hash_info = self.backend.fetch_hash(Path("path-does-not-matter.blend"), "sha256")
        self.assertIsNone(hash_info, "A non-existent entry should be handled gracefully")


def main() -> None:
    global scratch_dir

    import sys
    import tempfile

    argv = [sys.argv[0]]
    if '--' in sys.argv:
        argv.extend(sys.argv[sys.argv.index('--') + 1:])

    # with tempfile.TemporaryDirectory() as temp_dir:
    #     scratch_dir = Path(temp_dir)
    #     unittest.main(argv=argv)
    scratch_dir = Path("/tmp/blendtest")
    scratch_dir.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=argv)


if __name__ == "__main__":
    main()
