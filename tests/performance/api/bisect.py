# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import datetime
from collections.abc import Callable
from dataclasses import dataclass

from .environment import TestEnvironment
from .test import Test


def date_str(ts: int) -> str:
    """Format a Unix timestamp as 'YYYY-MM-DD HH:MM:SS' in UTC."""
    return datetime.datetime.fromtimestamp(ts, tz=datetime.timezone.utc).strftime('%Y-%m-%d %H:%M:%S')


def passes_threshold(value: float, success: str, threshold: float) -> bool:
    """Check if a performance value passes the given threshold.

    Returns True when the value is on the good side of the threshold
    based on the success direction ('greater_than' or 'less_than').
    """
    if success == 'greater_than':
        return value > threshold
    elif success == 'less_than':
        return value < threshold
    return False


def test_commit(
    env: TestEnvironment,
    test: Test,
    device_id: str,
    gpu_backend: str,
    count: int,
    attribute: str,
    success: str,
    threshold: float,
    tested: set[str],
    on_progress: Callable[..., None],
    commit_hash: str,
    commit_ts: int,
) -> tuple[float | None, str]:
    """Build, benchmark, and evaluate a single commit.

    Builds the given git hash, runs the test ``count`` times, averages the
    results, and checks whether the value passes the threshold.

    Args:
        env: TestEnvironment for git/build operations.
        test: Test object to run.
        device_id: Device identifier string.
        gpu_backend: GPU backend string.
        count: Number of benchmark runs per commit.
        attribute: Name of the performance attribute to extract from output.
        success: Comparison direction ('greater_than' or 'less_than').
        threshold: Pass/fail threshold value.
        tested: Mutable set tracking already-tested commit hashes.
        on_progress: Callable ``(row_values, end)`` for printing table rows.
        commit_hash: Commit hash to test.
        commit_ts: Unix timestamp of the commit.

    Returns:
        Tuple ``(value, status)`` where status is one of
        ``'skip'``, ``'build_error'``, ``'no_output'``, ``'run_error``, ``'pass'`` or ``'fail'``.
        value can be None when status is ``'skip'``, ``'build_error'``, ``'no_output'``, ``'run_error'``.
    """
    # During the weekends it can happen that a day doesn't have any commit. In that case a commit
    # can be selected that has already been performed.
    if commit_hash in tested:
        return None, 'skip'
    tested.add(commit_hash)

    title = env.commit_title(commit_hash)[:70]
    on_progress([commit_hash, date_str(commit_ts), title, '', 'building'], end='\r')

    install_dir = env.install_dir
    ok = env.build(commit_hash, install_dir)
    if not ok:
        on_progress([commit_hash, date_str(commit_ts), title, 'error', 'FAIL (build)'])
        return None, 'build_error'

    env.set_blender_executable(install_dir, {})

    values: list[float] = []
    try:
        for run_idx in range(count):
            run_status = 'running' if count == 1 else f'run [{run_idx + 1}/{count}]'
            on_progress([commit_hash, date_str(commit_ts), title, '', run_status], end='\r')
            output = test.run(env, device_id, gpu_backend)
            if not output or attribute not in output:
                env.set_default_blender_executable()
                on_progress([commit_hash, date_str(commit_ts), title, 'error', 'run'])
                return None, 'no_output'
            values.append(output[attribute])
    except Exception as e:
        env.set_default_blender_executable()
        on_progress([commit_hash, date_str(commit_ts), title, 'error', str(e)[:30]])
        return None, 'run_error'

    env.set_default_blender_executable()
    avg = sum(values) / len(values)

    good = passes_threshold(avg, success, threshold)
    status = 'PASS' if good else 'FAIL'
    on_progress([commit_hash, date_str(commit_ts), title, f'{avg:.4f}', status])
    return avg, 'pass' if good else 'fail'


@dataclass
class _SearchBounds:
    min_index: int
    max_index: int
    last_good: str | None
    first_bad: str | None


class BisectProgress:
    """Tracks the current search window during a binary search."""

    def __init__(self, min_index: int, max_index: int) -> None:
        self.min_index = min_index
        self.max_index = max_index

    @property
    def remaining(self) -> int:
        return self.max_index - self.min_index


def binary_search(
    commits: list[tuple[str, int]],
    min_index: int,
    max_index: int,
    last_good: str | None,
    first_bad: str | None,
    commit_status: dict[str, str],
    test_commit: Callable[..., tuple[float | None, str]],
    progress: BisectProgress | None = None,
) -> tuple[str | None, str | None]:
    """
    Binary search to find the first failing commit.

    When a commit errors on build or run, a forward scan finds the next testable commit.

    Args:
        commits: List of ``(commit_hash, unix_timestamp)`` tuples to search.
        min_index: Lower bound (inclusive) for the search range.
        max_index: Upper bound (exclusive) for the search range.
        last_good: Commit hash of the last known passing commit (or None).
        first_bad: Commit hash of the first known failing commit (or None).
        commit_status: Map of previously tested commit hashes to ``'pass'`` or ``'fail'``.
        test_commit: Callable ``(commit_hash, commit_ts) -> (value, status)`` that tests a commit.
        progress: Optional ``BisectProgress`` updated as the search bounds change.

    Returns:
        Tuple ``(last_good, first_bad)`` with the updated bounds after the search.
    """
    bounds = _SearchBounds(min_index, max_index, last_good, first_bad)

    while bounds.min_index < bounds.max_index:
        mid = (bounds.min_index + bounds.max_index) // 2
        commit_hash, commit_ts = commits[mid]

        if commit_hash in commit_status:
            if commit_status[commit_hash] == 'pass':
                bounds.min_index = mid + 1
            else:
                bounds.max_index = mid
            if progress:
                progress.min_index = bounds.min_index
                progress.max_index = bounds.max_index
            continue

        _, status = test_commit(commit_hash, commit_ts)

        if status == 'pass':
            commit_status[commit_hash] = 'pass'
            bounds.last_good = commit_hash
            bounds.min_index = mid + 1
        elif status == 'fail':
            commit_status[commit_hash] = 'fail'
            bounds.first_bad = commit_hash
            bounds.max_index = mid
        else:
            found, bounds = _forward_scan(
                commits, mid + 1, commit_status, test_commit, bounds, progress)
            if not found:
                break
        if progress:
            progress.min_index = bounds.min_index
            progress.max_index = bounds.max_index

    return bounds.last_good, bounds.first_bad


def _forward_scan(
    commits: list[tuple[str, int]],
    start_index: int,
    commit_status: dict[str, str],
    test_commit: Callable[..., tuple[float | None, str]],
    bounds: _SearchBounds,
    progress: BisectProgress | None = None,
) -> tuple[bool, _SearchBounds]:
    """Scan forward from start_index for a testable commit.

    Returns (found, bounds) where bounds are updated if a testable commit was found.
    """
    for scan_index in range(start_index, bounds.max_index):
        scan_hash, scan_ts = commits[scan_index]
        if scan_hash in commit_status:
            if commit_status[scan_hash] == 'fail':
                bounds.first_bad = scan_hash
                bounds.max_index = scan_index
                if progress:
                    progress.min_index = bounds.min_index
                    progress.max_index = bounds.max_index
                return True, bounds
            continue
        _, status = test_commit(scan_hash, scan_ts)
        if status == 'pass':
            commit_status[scan_hash] = 'pass'
            bounds.last_good = scan_hash
            bounds.min_index = scan_index + 1
            if progress:
                progress.min_index = bounds.min_index
                progress.max_index = bounds.max_index
            return True, bounds
        elif status == 'fail':
            commit_status[scan_hash] = 'fail'
            bounds.first_bad = scan_hash
            bounds.max_index = scan_index
            if progress:
                progress.min_index = bounds.min_index
                progress.max_index = bounds.max_index
            return True, bounds
    return False, bounds
