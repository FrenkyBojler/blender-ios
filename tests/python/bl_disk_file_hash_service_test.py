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
    filepath: Path
    storagepath: Path

    def setUp(self) -> None:
        # Create a file to test with. It's recreated for every test to allow
        # modification.
        self.filepath = scratch_dir / "testfile.txt"
        self.filepath.write_text("😻 Laksa & Quercus 😻")

        self.storagepath = scratch_dir / "database"

    def tearDown(self) -> None:
        self.filepath.unlink(missing_ok=True)

    def test_store_get_hash(self) -> None:
        backend = backend_sqlite.SQLiteBackend(self.storagepath)
        backend.open()
        try:
            # This function should trust the provided hash.
            fake_hash = "Monkeys are not a hash, what is this?"
            backend.store_hash(self.filepath, "sha256", fake_hash)
            hash_from_service = backend.get_hash(self.filepath, "sha256")
            self.assertEqual(fake_hash, hash_from_service, "The fake hash should have been trusted")

            # It should be repeatable
            fake_hash = "A new hash is new"
            backend.store_hash(self.filepath, "sha256", fake_hash)
            hash_from_service = backend.get_hash(self.filepath, "sha256")
            self.assertEqual(fake_hash, hash_from_service, "The fake hash should have been trusted")
        finally:
            backend.close()


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
