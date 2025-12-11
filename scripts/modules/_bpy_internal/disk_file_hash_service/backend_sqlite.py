# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__all__ = (
    'SQLiteBackend',
)

import contextlib
import datetime
import sqlite3
from pathlib import Path
from typing import Iterator


DB_TIMEOUT_MSEC = 5000  # SQLite busy timeout in milliseconds.
DB_SCHEMA_VERSION = 1
CREATE_SCHEMA_V1 = """
CREATE TABLE IF NOT EXISTS files (
    file_id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
    path TEXT NOT NULL,
    size_in_bytes BIGINT NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS files_path ON files (path);

CREATE TABLE IF NOT EXISTS hashes (
    file_id INTEGER NOT NULL,
    hash_algo VARCHAR(10) NOT NULL,
    hexdigest TEXT NOT NULL,
    file_stat_mtime FLOAT NOT NULL,
    last_checked DATETIME NOT NULL,
    PRIMARY KEY(file_id, hash_algo)
    FOREIGN KEY(file_id) REFERENCES files(file_id) ON DELETE CASCADE ON UPDATE CASCADE
);
"""


class SQLiteBackend:
    """DiskFileHashBackend implementation using SQLite as storage engine."""

    dbfile_path: Path  # Path of the .sqlite file to use.

    db_conn_rw: sqlite3.Connection | None = None
    db_conn_ro: sqlite3.Connection | None = None

    def __init__(self, storage_path: Path) -> None:
        assert not storage_path.is_dir(), "SQLite back-end expects a directory + file prefix as storage path"

        self.dbfile_path = storage_path.with_name("{}_v{}.sqlite".format(storage_path.stem, DB_SCHEMA_VERSION))
        self._debug_queries = True
        self.db_conn_rw = None
        self.db_conn_ro = None

    def open(self) -> None:
        """Prepare the back-end for use.

        Create the directory structure & database file, and ensure the schema is as expected.
        """
        import sqlite3

        self.dbfile_path.parent.mkdir(parents=True, exist_ok=True)

        # Connect to the database. I (Sybren) am not sure whether the timeout to
        # `sqlite3.connect()` actually applies, as typically SQLite only checks
        # for locked databases when a transaction is started (either implicitly
        # or explicitly).
        self.db_conn_rw = sqlite3.connect(self.dbfile_path, timeout=DB_TIMEOUT_MSEC / 1000)

        if self._debug_queries:
            def callback_rw(query: str) -> None:
                print(f"SQL/\033[95mRW: {query}\033[0m")
            self.db_conn_rw.set_trace_callback(callback_rw)
        self._execute_pragmas_on_connect(self.db_conn_rw)

        # Assumption: if the table exists, it should be in the right shape. If
        # that's not the case, the DB_SCHEMA_VERSION class variable should have
        # been incremented, and we'd be accessing another database file.
        with self._transaction_rw() as db_conn:
            db_conn.executescript(CREATE_SCHEMA_V1)

        # After the database is set up, open another connection that's read-only.
        uri = self.dbfile_path.as_uri() + "?mode=ro"
        self.db_conn_ro = sqlite3.connect(uri, uri=True, timeout=DB_TIMEOUT_MSEC / 1000)

        if self._debug_queries:
            def callback_ro(query: str) -> None:
                print(f"SQL/\033[96mRO: {query}\033[0m")
            self.db_conn_rw.set_trace_callback(callback_ro)
        self._execute_pragmas_on_connect(self.db_conn_ro)

    def close(self) -> None:
        """Close the database connection."""
        if self.db_conn_ro:
            self.db_conn_ro.close()
            self.db_conn_ro = None

        # Close the read-write connection last, otherwise the WAL journal files
        # will not be checkpointed and removed.
        if self.db_conn_rw:
            self.db_conn_rw.close()
            self.db_conn_rw = None

    def get_hash(self, filepath: Path, hash_algorithm: str) -> str:
        """Return the hash of the given file, as hex string.

        Raises a FileNotFoundError if the file does not exist on disk.
        """

        row = None
        with self._transaction_ro() as db:
            cursor = db.execute(
                "SELECT f.size_in_bytes, h.hexdigest, h.file_stat_mtime " +
                "FROM files f LEFT JOIN hashes h USING (file_id) " +
                "WHERE f.path=? AND h.hash_algo=?",
                (str(filepath), hash_algorithm))
            # The uniqueness constraints should ensure there is at most one row.
            row = cursor.fetchone()

        if row:
            # Check if the cached info is still ok.
            size_in_bytes, hexdigest, file_stat_mtime = row
            if hexdigest and self._file_stat_matches(filepath, size_in_bytes, file_stat_mtime):
                # There is no reason to suspect the cached hex digest is stale, so just use it.
                return hexdigest

        raise NotImplementedError()

    def store_hash(self, filepath: Path, hash_algorithm: str, hexhash: str) -> None:
        """Store a pre-computed hash for the given file path. The path has to exist."""
        stat = filepath.stat()
        now = self._now_string()

        with self._transaction_rw() as db:
            # The 'RETURNING file_id' ensures that we know which file ID was
            # upserted. We can't rely on last_insert_rowid() or
            # cursor.lastrowid, as that only works on INSERT and not UPDATE.
            cursor = db.execute(
                "INSERT INTO files (path, size_in_bytes) values (:path, :size) " +
                "ON CONFLICT DO UPDATE SET size_in_bytes=:size RETURNING file_id",
                {"path": str(filepath), "size": stat.st_size},
            )
            file_id = cursor.fetchone()[0]
            assert file_id, f'{file_id=}'

            db.execute(
                "INSERT INTO hashes " +
                "(file_id, hash_algo, hexdigest, file_stat_mtime, last_checked) " +
                "VALUES (:file_id, :hash_algo, :hex, :mtime, :now) ON CONFLICT DO UPDATE " +
                "SET hexdigest=:hex, file_stat_mtime=:mtime, last_checked=:now", {
                    "file_id": file_id,
                    "hash_algo": hash_algorithm,
                    "hex": hexhash,
                    "mtime": stat.st_mtime,
                    "now": now,
                },
            )

    def file_matches(self, filepath: Path, hash_algorithm: str, hexhash: str, size_in_byes: int) -> bool:
        """Check the file on disk, to see if it matches the given properties."""
        raise NotImplementedError()

    def _now(self) -> datetime.datetime:
        """Current time, as UTC, in a timezone-aware object."""
        return datetime.datetime.now(tz=datetime.timezone.utc)

    def _now_string(self) -> str:
        """Current time, as UTC, in ISO 6801 notation."""
        return self._now().isoformat()

    @contextlib.contextmanager
    def _transaction_rw(self) -> Iterator[sqlite3.Connection]:
        """Start a read-write transaction.

        The transaction is rolled back when an exception is raised, and
        committed otherwise.
        """
        assert self.db_conn_rw is not None

        self.db_conn_rw.execute("BEGIN EXCLUSIVE")
        try:
            yield self.db_conn_rw
        except BaseException:
            self.db_conn_rw.rollback()
            raise
        else:
            self.db_conn_rw.commit()

    @contextlib.contextmanager
    def _transaction_ro(self) -> Iterator[sqlite3.Connection]:
        """Start a read-write transaction.

        The transaction is always rolled back, because it shouldn't write
        anything anyway.
        """
        assert self.db_conn_ro is not None

        self.db_conn_ro.execute("BEGIN IMMEDIATE")
        try:
            yield self.db_conn_ro
        finally:
            self.db_conn_ro.rollback()

    def _execute_pragmas_on_connect(self, db_conn: sqlite3.Connection) -> None:
        db_conn.execute("PRAGMA busy_timeout = {:d}".format(DB_TIMEOUT_MSEC))
        db_conn.execute("PRAGMA foreign_keys = 1")
        db_conn.execute("PRAGMA journal_mode = WAL")
        db_conn.execute("PRAGMA synchronous = normal")

    def _file_stat_matches(self, filepath: Path, size_in_bytes: int, file_stat_mtime: float) -> bool:
        """Check whether the file on disk matches this size & timestamp."""
        try:
            stat = filepath.stat()
        except FileNotFoundError:
            return False
        return stat.st_size == size_in_bytes and stat.st_mtime == file_stat_mtime
