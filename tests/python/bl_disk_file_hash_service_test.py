# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

"""
blender -b --factory-startup -P tests/python/bl_disk_file_hash_service_test.py  -- --verbose
"""

__all__ = (
    "main",
)

import os
import unittest
from pathlib import Path

from _bpy_internal.disk_file_hash_service import backend_sqlite, hash_service, types

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
        fake_hash_info = types.FileHashInfo(
            hexhash="Monkeys are not a hash, what is this?",
            file_size_bytes=100,
            file_stat_mtime=47.327,
        )
        self.backend.store_hash(filepath, "sha256", fake_hash_info)
        hash_info = self.backend.fetch_hash(filepath, "sha256")
        assert hash_info is not None
        self.assertEqual(fake_hash_info, hash_info)

        # The info should be updatable.
        new_hash_info = types.FileHashInfo(
            hexhash="A new hash is new",
            file_size_bytes=42,
            file_stat_mtime=100.43,
        )

        self.backend.store_hash(filepath, "sha256", new_hash_info)
        hash_info = self.backend.fetch_hash(filepath, "sha256")
        assert hash_info is not None
        self.assertEqual(new_hash_info, hash_info)

    def test_fetch_nonexistent_file(self) -> None:
        hash_info = self.backend.fetch_hash(Path("path-does-not-matter.blend"), "sha256")
        self.assertIsNone(hash_info, "A non-existent entry should be handled gracefully")


class DiskFileHashServiceTest(unittest.TestCase):
    storagepath: Path
    filepath: Path
    backend: types.DiskFileHashBackend
    service: hash_service.DiskFileHashService

    def setUp(self) -> None:
        self.storagepath = scratch_dir / "database"

        # Create a test file to play with
        self.filepath = scratch_dir / "file-to-hash.txt"
        self.filepath.write_text("😺 Laksa & Quercus 😻")

        self.backend = backend_sqlite.SQLiteBackend(self.storagepath)

        # Delete the database between each test.
        self.backend.dbfile_path.unlink(missing_ok=True)

        self.service = hash_service.DiskFileHashService(self.backend)
        self.service.open()

    def tearDown(self) -> None:
        self.service.close()

    def test_get_file_hash(self) -> None:
        # Test the service.
        hash = self.service.get_hash(self.filepath, "sha256")
        self.assertEqual("43231d711ce5992cd9090ffa5cbb8779148e291bc1472353cdeebd040bef0b93", hash)

        # Check that the back-end now has the hash stored.
        backend_info = self.backend.fetch_hash(self.filepath, "sha256")
        assert backend_info is not None
        self.assertEqual(hash, backend_info.hexhash)

    def test_rehash_after_modification(self) -> None:
        # Tell the back-end to store a fake hash, so that we get a different
        # result (the actual file hash) when the file is re-hashed.
        stat = self.filepath.stat()
        self.backend.store_hash(self.filepath, "sha256", types.FileHashInfo(
            hexhash="fake hash", file_size_bytes=stat.st_size, file_stat_mtime=stat.st_mtime))

        # Get the hash from the service. Since the cached hash matches the
        # current size & mtime, it should just return the cached hash.
        hash = self.service.get_hash(self.filepath, "sha256")
        self.assertEqual("fake hash", hash)

        # Update the file, this should trigger a re-hashing.
        self.filepath.touch()
        updated_hash = self.service.get_hash(self.filepath, "sha256")
        self.assertEqual("43231d711ce5992cd9090ffa5cbb8779148e291bc1472353cdeebd040bef0b93", updated_hash)

        # Change the contents to something of a different length, but keep the
        # mtime the same. This also should trigger a re-hashing.
        self.filepath.write_text("New Content 😿")
        os.utime(self.filepath, (stat.st_atime, stat.st_mtime))
        updated_hash = self.service.get_hash(self.filepath, "sha256")
        self.assertEqual("49a02e79cb4c68a5f1626d34a05a021b60cbd0b22f9485dcd4026ab3e9201b5a", updated_hash)

    def test_file_matches(self) -> None:
        # Tell the back-end to store a fake hash, so that we get a different
        # result (the actual file hash) when the file is re-hashed.
        stat = self.filepath.stat()
        self.backend.store_hash(self.filepath, "sha256", types.FileHashInfo(
            hexhash="fake hash", file_size_bytes=stat.st_size, file_stat_mtime=stat.st_mtime))
        self.assertTrue(self.service.file_matches(self.filepath, "sha256", "fake hash", stat.st_size))
        self.assertFalse(self.service.file_matches(self.filepath, "sha256", "fake hash", stat.st_size + 5))

        # Touch the file to trigger a re-hash.
        self.filepath.touch()
        self.assertTrue(
            self.service.file_matches(
                self.filepath,
                "sha256",
                "43231d711ce5992cd9090ffa5cbb8779148e291bc1472353cdeebd040bef0b93",
                stat.st_size))

        # Check that the back-end now has the hash stored.
        backend_info = self.backend.fetch_hash(self.filepath, "sha256")
        assert backend_info is not None
        self.assertEqual("43231d711ce5992cd9090ffa5cbb8779148e291bc1472353cdeebd040bef0b93", backend_info.hexhash)


class DiskFileHashServiceNotOpeningTest(unittest.TestCase):
    """Contrary to the above test case, this one doesn't auto-open the service for each test."""

    storagepath: Path
    filepath: Path
    backend: types.DiskFileHashBackend
    service: hash_service.DiskFileHashService

    def setUp(self) -> None:
        self.storagepath = scratch_dir / "database"

        self.backend = backend_sqlite.SQLiteBackend(self.storagepath)

        # Delete the database between each test.
        self.backend.dbfile_path.unlink(missing_ok=True)

        self.service = hash_service.DiskFileHashService(self.backend)

    def tearDown(self) -> None:
        self.service.close()

    def test_closing_unopened_service(self) -> None:
        """A service that was never opened should still be closable."""
        self.service.close()


def main() -> None:
    global scratch_dir

    import sys
    import tempfile

    argv = [sys.argv[0]]
    if '--' in sys.argv:
        argv.extend(sys.argv[sys.argv.index('--') + 1:])

    with tempfile.TemporaryDirectory() as temp_dir:
        scratch_dir = Path(temp_dir)
        unittest.main(argv=argv)


if __name__ == "__main__":
    main()
