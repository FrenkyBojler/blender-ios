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
import datetime
import unittest
from pathlib import Path
from typing import Callable

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

    def test_store_multiple_hashes_and_sizes(self) -> None:
        """Each cached hash should store the hashed file size as well."""
        filepath = Path("path-does-not-matter.blend")
        hash_info_1 = types.FileHashInfo(
            hexhash="Hash for 100 bytes file + SHA256",
            file_size_bytes=100,
            file_stat_mtime=47,
        )
        hash_info_2 = types.FileHashInfo(
            hexhash="Hash for 42 bytes file + SHA1",
            file_size_bytes=42,
            file_stat_mtime=327,
        )

        self.backend.store_hash(filepath, "sha256", hash_info_1)
        self.backend.store_hash(filepath, "sha1", hash_info_2)

        cached_info_1 = self.backend.fetch_hash(filepath, "sha256")
        cached_info_2 = self.backend.fetch_hash(filepath, "sha1")

        assert cached_info_1 is not None
        assert cached_info_2 is not None

        self.assertEqual(hash_info_1, cached_info_1)
        self.assertEqual(hash_info_2, cached_info_2)

    def test_mark_as_fresh(self) -> None:
        # Monkeypatch the backend so that it thinks it's the past, so that the hash we store is back-dated.
        orig_now = self.backend._now
        self.backend._now = lambda: datetime.datetime(
            year=2024, month=1, day=1, hour=0, minute=0, second=0, tzinfo=datetime.timezone.utc)

        filepath = Path("old-file.blend")
        fake_hash_info = types.FileHashInfo(
            hexhash="fake hash",
            file_size_bytes=100,
            file_stat_mtime=47.327,
        )
        self.backend.store_hash(filepath, "sha256", fake_hash_info)

        # Restore the 'now' function for the backend.
        self.backend._now = orig_now

        # Mark the hash as 'fresh'.
        self.backend.mark_hash_as_fresh(filepath, "sha256")

        # Remove outdated hashes, which shouldn't do anything.
        self.backend.remove_older_than(days=5)

        # The old-but-refreshed hash should still be there.
        cached_hash_info = self.backend.fetch_hash(filepath, "sha256")
        self.assertEqual(fake_hash_info, cached_hash_info)

    def test_mark_hashes_as_fresh(self) -> None:
        """Bulk-refreshing a set of hashes should keep them from being removed."""
        # Monkeypatch the backend so that it thinks it's the past, so that the hashes we store are back-dated.
        orig_now = self.backend._now
        self.backend._now = lambda: datetime.datetime(
            year=2024, month=1, day=1, hour=0, minute=0, second=0, tzinfo=datetime.timezone.utc)

        filepath_a = Path("file-a.blend")
        filepath_b = Path("file-b.blend")
        filepath_c = Path("file-c.blend")
        fake_hash_info = types.FileHashInfo(
            hexhash="fake hash",
            file_size_bytes=100,
            file_stat_mtime=47.327,
        )
        self.backend.store_hash(filepath_a, "sha256", fake_hash_info)
        self.backend.store_hash(filepath_b, "sha1", fake_hash_info)
        # This one stays old, and is expected to be removed below.
        self.backend.store_hash(filepath_c, "sha256", fake_hash_info)

        # Restore the 'now' function for the backend.
        self.backend._now = orig_now

        # Bulk-refresh the first two hashes, including a non-existent entry that should be silently ignored.
        self.backend.mark_hashes_as_fresh([
            (filepath_a, "sha256"),
            (filepath_b, "sha1"),
            (Path("never-stored.blend"), "sha256"),
        ])

        # Remove outdated hashes; only filepath_c should be removed.
        self.backend.remove_older_than(days=5)

        self.assertEqual(fake_hash_info, self.backend.fetch_hash(filepath_a, "sha256"))
        self.assertEqual(fake_hash_info, self.backend.fetch_hash(filepath_b, "sha1"))
        self.assertIsNone(self.backend.fetch_hash(filepath_c, "sha256"))

    def test_mark_hashes_as_fresh_empty(self) -> None:
        """Passing an empty iterable should be a no-op rather than an error."""
        self.backend.mark_hashes_as_fresh([])

    def test_fetch_older_than(self) -> None:
        """Only entries older than the cutoff should be returned, with full hash info."""
        # Monkeypatch the backend so that it thinks it's the past, so that the hashes we store are back-dated.
        orig_now = self.backend._now
        self.backend._now = lambda: datetime.datetime(
            year=2024, month=1, day=1, hour=0, minute=0, second=0, tzinfo=datetime.timezone.utc)

        filepath_old = Path("old-file.blend")
        old_info_sha256 = types.FileHashInfo(
            hexhash="old sha256 hash",
            file_size_bytes=100,
            file_stat_mtime=47.327,
        )
        old_info_sha1 = types.FileHashInfo(
            hexhash="old sha1 hash",
            file_size_bytes=100,
            file_stat_mtime=47.327,
        )
        self.backend.store_hash(filepath_old, "sha256", old_info_sha256)
        self.backend.store_hash(filepath_old, "sha1", old_info_sha1)

        # Restore the 'now' function for the backend.
        self.backend._now = orig_now

        # A fresh entry should not show up.
        filepath_new = Path("new-file.blend")
        new_info = types.FileHashInfo(
            hexhash="new hash",
            file_size_bytes=42,
            file_stat_mtime=100.0,
        )
        self.backend.store_hash(filepath_new, "sha256", new_info)

        older_entries = sorted(
            self.backend.fetch_older_than(days=5),
            key=lambda entry: (str(entry[0]), entry[1]),
        )
        self.assertEqual([
            (filepath_old, "sha1", old_info_sha1),
            (filepath_old, "sha256", old_info_sha256),
        ], older_entries)

    def test_fetch_older_than_empty(self) -> None:
        """An empty database should return an empty iterable rather than raising."""
        self.assertEqual([], list(self.backend.fetch_older_than(days=5)))

    def test_remove_older_than(self) -> None:
        # Monkeypatch the backend so that it thinks it's the past, so that the hash we store is back-dated.
        orig_now = self.backend._now
        self.backend._now = lambda: datetime.datetime(
            year=2024, month=1, day=1, hour=0, minute=0, second=0, tzinfo=datetime.timezone.utc)

        filepath_old = Path("old-file.blend")
        fake_hash_info = types.FileHashInfo(
            hexhash="fake hash",
            file_size_bytes=100,
            file_stat_mtime=47.327,
        )
        self.backend.store_hash(filepath_old, "sha256", fake_hash_info)

        # Restore the 'now' function for the backend.
        self.backend._now = orig_now

        # Store another file path, which shouldn't be outdated.
        filepath_new = Path("new-file.blend")
        self.backend.store_hash(filepath_new, "sha256", fake_hash_info)

        # Remove outdated hashes.
        self.backend.remove_older_than(days=5)

        # The old hash should be gone.
        cached_hash_info = self.backend.fetch_hash(filepath_old, "sha256")
        self.assertIsNone(cached_hash_info, "The cached hash should have been removed")

        # The new hash should still be there.
        cached_hash_info = self.backend.fetch_hash(filepath_new, "sha256")
        self.assertEqual(fake_hash_info, cached_hash_info)

        # The file entry itself should also have been removed.
        with self.backend._transaction_ro() as db:
            cursor = db.execute("SELECT * FROM files")
            all_files = cursor.fetchall()
            file_id = 2  # The 2nd inserted file should still be there.
            self.assertEqual([(file_id, str(filepath_new))], all_files)


class DiskFileHashServiceTest(unittest.TestCase):
    storagepath: Path
    filepath: Path
    backend: backend_sqlite.SQLiteBackend
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

        # Update the file timestamp, this should trigger a re-hashing.
        #
        # Change the modification time to trigger a re-hash. Use a nice large
        # increment to ensure this is a larger step than the filesystem
        # timestamp resolution.
        new_mtime = stat.st_mtime + 3600
        os.utime(self.filepath, (new_mtime, new_mtime))
        updated_hash = self.service.get_hash(self.filepath, "sha256")
        self.assertEqual("43231d711ce5992cd9090ffa5cbb8779148e291bc1472353cdeebd040bef0b93", updated_hash)

        # Change the contents to something of a different length, but keep the
        # mtime the same as in the cache. This also should trigger a re-hashing.
        self.filepath.write_text("New Content 😿")
        os.utime(self.filepath, (stat.st_atime, stat.st_mtime))
        updated_hash = self.service.get_hash(self.filepath, "sha256")
        self.assertEqual("49a02e79cb4c68a5f1626d34a05a021b60cbd0b22f9485dcd4026ab3e9201b5a", updated_hash)

    def test_store_hash_callback(self) -> None:
        # Construct another file, that mimicks a just-downloaded file that was
        # saved to a temp location and about to be moved to its final location
        # (self.filepath).
        other_path = self.filepath.with_stem("temp-download-file")
        other_path.write_text("New Content 😿")
        other_path_stat = other_path.stat()

        # Construct the hash info that is NOT valid currently, but will be when the callback returns.
        hash_info = types.FileHashInfo(
            hexhash="49a02e79cb4c68a5f1626d34a05a021b60cbd0b22f9485dcd4026ab3e9201b5a",
            file_size_bytes=other_path_stat.st_size,
            file_stat_mtime=other_path_stat.st_mtime,
        )

        # Calling the store_hash function without callback should raise an
        # exception, as the hash info is not valid.
        with self.assertRaises(ValueError):
            self.service.store_hash(self.filepath, 'sha256', hash_info)
        self.assertIsNone(self.backend.fetch_hash(self.filepath, "sha256"))

        # Provide a callback that raises an exception. This should prevent the
        # hash from being stored.
        class SpecificError(BaseException):
            pass

        def errorring_callback() -> None:
            raise SpecificError()

        with self.assertRaises(SpecificError):
            self.service.store_hash(self.filepath, 'sha256', hash_info, errorring_callback)
        self.assertIsNone(self.backend.fetch_hash(self.filepath, "sha256"))

        def pre_write_callback() -> None:
            # Move the 'other' path to the actually-hashed path.
            self.filepath.unlink()
            other_path.rename(self.filepath)

        self.service.store_hash(self.filepath, 'sha256', hash_info, pre_write_callback)

        # Check that the back-end now has the hash stored.
        backend_info = self.backend.fetch_hash(self.filepath, "sha256")
        assert backend_info is not None
        self.assertEqual(hash_info, backend_info)

    def test_file_matches(self) -> None:
        # Tell the back-end to store a fake hash, so that we get a different
        # result (the actual file hash) when the file is re-hashed.
        stat = self.filepath.stat()
        self.backend.store_hash(self.filepath, "sha256", types.FileHashInfo(
            hexhash="fake hash", file_size_bytes=stat.st_size, file_stat_mtime=stat.st_mtime))
        self.assertTrue(self.service.file_matches(self.filepath, "sha256", "fake hash", stat.st_size))
        self.assertFalse(self.service.file_matches(self.filepath, "sha256", "fake hash", stat.st_size + 5))

        # Change the modification time to trigger a re-hash. Use a nice large
        # increment to ensure this is a larger step than the filesystem
        # timestamp resolution.
        new_mtime = stat.st_mtime + 3600
        os.utime(self.filepath, (new_mtime, new_mtime))
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

    def test_cleanup_on_close(self) -> None:
        # Monkeypatch the backend so that it thinks it's the past, so that the hash we store is back-dated.
        orig_now = self.backend._now
        self.backend._now = lambda: datetime.datetime(
            year=2024, month=1, day=1, hour=0, minute=0, second=0, tzinfo=datetime.timezone.utc)

        filepath = Path("old-file.blend")
        fake_hash_info = types.FileHashInfo(
            hexhash="fake hash",
            file_size_bytes=100,
            file_stat_mtime=47.327,
        )
        self.backend.store_hash(filepath, "sha256", fake_hash_info)

        # Restore the 'now' function for the backend.
        self.backend._now = orig_now

        # Close the service. This should remove outdated hashes.
        self.service.close()

        # The old hash should have been removed. We have to create a new backend
        # to test this, because the old backend has been closed already by
        # closing the service.
        new_backend = backend_sqlite.SQLiteBackend(self.storagepath)
        new_backend.open()
        try:
            cached_hash_info = new_backend.fetch_hash(filepath, "sha256")
            self.assertIsNone(cached_hash_info)
        finally:
            new_backend.close()


    def _backdate_now(self) -> Callable[[], datetime.datetime]:
        """Replace the backend's `_now` with a back-dated value so that any
        subsequently-stored hash has an old `last_checked` timestamp. Returns
        the original `_now` so the caller can restore it.
        """
        orig_now = self.backend._now
        self.backend._now = lambda: datetime.datetime(
            year=2024, month=1, day=1, hour=0, minute=0, second=0, tzinfo=datetime.timezone.utc)
        return orig_now

    def _last_checked(self, filepath: Path, hash_algorithm: str) -> datetime.datetime:
        """Read the raw `last_checked` column for a (file, algo) pair."""
        with self.backend._transaction_ro() as db:
            cursor = db.execute(
                "SELECT h.last_checked FROM files f INNER JOIN hashes h USING (file_id) " +
                "WHERE f.path=? AND h.hash_algo=?",
                (str(filepath), hash_algorithm))
            row = cursor.fetchone()
        assert row is not None, "no hash row for {!r}/{!r}".format(filepath, hash_algorithm)
        return datetime.datetime.fromisoformat(row[0])

    def test_close_refreshes_old_hash_for_matching_file(self) -> None:
        """An old hash whose file still matches on disk should survive close() instead of being deleted."""
        orig_now = self._backdate_now()

        stat = self.filepath.stat()
        matching_info = types.FileHashInfo(
            hexhash="cached hash for real file",
            file_size_bytes=stat.st_size,
            file_stat_mtime=stat.st_mtime,
        )
        self.backend.store_hash(self.filepath, "sha256", matching_info)
        self.backend._now = orig_now

        self.service.close()

        new_backend = backend_sqlite.SQLiteBackend(self.storagepath)
        new_backend.open()
        try:
            self.assertEqual(matching_info, new_backend.fetch_hash(self.filepath, "sha256"))
        finally:
            new_backend.close()

    def test_close_actually_bumps_last_checked_for_matching_file(self) -> None:
        """Surviving an old close() pass should be due to a real `last_checked` update,
        not just the row being skipped — guards against a no-op `mark_hashes_as_fresh`.
        """
        orig_now = self._backdate_now()

        stat = self.filepath.stat()
        matching_info = types.FileHashInfo(
            hexhash="cached hash for real file",
            file_size_bytes=stat.st_size,
            file_stat_mtime=stat.st_mtime,
        )
        self.backend.store_hash(self.filepath, "sha256", matching_info)

        # Confirm the stored timestamp is in fact back-dated before close() runs.
        before_close = self._last_checked(self.filepath, "sha256")
        self.assertLess(before_close, datetime.datetime(2024, 6, 1, tzinfo=datetime.timezone.utc))

        self.backend._now = orig_now
        self.service.close()

        new_backend = backend_sqlite.SQLiteBackend(self.storagepath)
        new_backend.open()
        try:
            self.backend = new_backend  # so _last_checked uses the open connection
            after_close = self._last_checked(self.filepath, "sha256")
            # Should have been bumped to roughly "now" (well within the last hour).
            now = datetime.datetime.now(tz=datetime.timezone.utc)
            self.assertGreater(after_close, now - datetime.timedelta(hours=1))
        finally:
            new_backend.close()

    def test_close_removes_old_hash_for_mismatched_file(self) -> None:
        """An old hash for a file whose stats no longer match should be deleted by close()."""
        orig_now = self._backdate_now()

        mismatched_filepath = scratch_dir / "mismatched-file.txt"
        mismatched_filepath.write_text("some content")
        mismatched_info = types.FileHashInfo(
            hexhash="stale hash",
            file_size_bytes=999,  # Deliberately wrong size.
            file_stat_mtime=1.0,
        )
        self.backend.store_hash(mismatched_filepath, "sha256", mismatched_info)
        self.backend._now = orig_now

        self.service.close()

        new_backend = backend_sqlite.SQLiteBackend(self.storagepath)
        new_backend.open()
        try:
            self.assertIsNone(new_backend.fetch_hash(mismatched_filepath, "sha256"))
        finally:
            new_backend.close()

    def test_close_only_refreshes_matching_among_mixed_old_hashes(self) -> None:
        """When old hashes are a mix of matching/missing/mismatched, only matching ones survive close()."""
        orig_now = self._backdate_now()

        stat = self.filepath.stat()
        matching_info = types.FileHashInfo(
            hexhash="cached hash for real file",
            file_size_bytes=stat.st_size,
            file_stat_mtime=stat.st_mtime,
        )
        self.backend.store_hash(self.filepath, "sha256", matching_info)

        missing_filepath = scratch_dir / "missing-file.blend"
        self.backend.store_hash(missing_filepath, "sha256", types.FileHashInfo(
            hexhash="dead hash", file_size_bytes=10, file_stat_mtime=42.0))

        mismatched_filepath = scratch_dir / "mismatched-file.txt"
        mismatched_filepath.write_text("some content")
        self.backend.store_hash(mismatched_filepath, "sha256", types.FileHashInfo(
            hexhash="stale hash", file_size_bytes=999, file_stat_mtime=1.0))

        self.backend._now = orig_now
        self.service.close()

        new_backend = backend_sqlite.SQLiteBackend(self.storagepath)
        new_backend.open()
        try:
            self.assertEqual(matching_info, new_backend.fetch_hash(self.filepath, "sha256"))
            self.assertIsNone(new_backend.fetch_hash(missing_filepath, "sha256"))
            self.assertIsNone(new_backend.fetch_hash(mismatched_filepath, "sha256"))
        finally:
            new_backend.close()


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

    def test_doubly_closing(self) -> None:
        """A service that has been opened should be closable twice."""
        self.service.open()
        self.service.close()
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
