# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import io
from pathlib import Path
from typing import Callable

__all__ = (
    'mutex_lock_and_open',
)


def mutex_lock_and_open(file_path: Path, mode: str) -> tuple[io.IOBase | None, Callable[[io.IOBase], None] | None]:
    """Obtain an exclusive lock on a file.

    Create a file on disk, and immediately lock it for exclusive use by this
    process.

    This uses approaches from:
        - https://www.pythontutorials.net/blog/make-sure-only-a-single-instance-of-a-program-is-running/
        - https://yakking.branchable.com/posts/procrun-2-pidfiles/

    :param: mode MUST be a binary mode, to be compatible with the file locking
        on Windows. So either 'rb' or 'wb'.

    :returns: If the file was opened & locked succesfully, a tuple (file,
        unlocker) is returned. Otherwise returns None.
    """

    import sys

    # Choose platform-dependent _obtain_lock(file) and _release_lock() functions.
    if sys.platform == "win32":
        import msvcrt

        def _obtain_lock(file: io.IOBase) -> None:
            # Lock the first byte of the file (arbitrary choice)
            msvcrt.locking(file.fileno(), msvcrt.LK_NBLCK, 1)

        def _unlock_and_close(file: io.IOBase) -> None:
            msvcrt.locking(file.fileno(), msvcrt.LK_UNLCK, 1)
            file.close()
    else:
        import fcntl

        def _obtain_lock(file: io.IOBase) -> None:
            fcntl.flock(file, fcntl.LOCK_EX | fcntl.LOCK_NB)

        def _unlock_and_close(file: io.IOBase) -> None:
            # Closing the file automatically releases the lock.
            file.close()

    assert isinstance(file_path, Path)
    assert 'b' in mode, "mode must include 'b' for binary"

    # It is not suitable here to use an 'exclusive create' ('x' option) here.
    # That will still create a race condition, with the space between creation
    # of the file and locking it. So, better to make the existence of the file
    # meaningless, and only communicate the lock state with an actual filesystem
    # lock.
    try:
        # Type is ignored here, because the type checker doesn't realize that
        # the above assert ensures the file is opened in a binary mode.
        lockfile: io.IOBase
        lockfile = file_path.open(mode)  # type: ignore
    except OSError:
        # on Windows, opening a file for writing, while another process already
        # has it open, can fail. That just means somebody else has ownership of
        # it.
        return None, None

    try:
        _obtain_lock(lockfile)
    except OSError:
        # Lock is already held by another Blender.
        lockfile.close()
        return None, None

    # We have obtained an exclusive lock, which the OS will release when this
    # process is killed.
    return lockfile, _unlock_and_close
