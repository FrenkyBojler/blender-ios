# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import datetime
from collections.abc import Callable
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .environment import TestEnvironment
    from .test import Test


def date_str(ts: int) -> str:
    """Format a Unix timestamp as 'YYYY-MM-DD HH:MM:SS' in UTC."""
    return datetime.datetime.fromtimestamp(ts, tz=datetime.timezone.utc).strftime('%Y-%m-%d %H:%M:%S')


def is_good(value: float, success: str, threshold: float) -> bool:
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
        Tuple ``(average_value, status)`` where status is one of
        ``'skip'``, ``'build'``, ``'run'``, ``'pass'``, or ``'fail'``.
    """
    if commit_hash in tested:
        return None, 'skip'
    tested.add(commit_hash)

    title = env.commit_title(commit_hash)[:70]
    on_progress([commit_hash, date_str(commit_ts), title, '', 'building'], end='\r')

    install_dir = env.install_dir
    ok = env.build(commit_hash, install_dir)
    if not ok:
        on_progress([commit_hash, date_str(commit_ts), title, 'error', 'FAIL (build)'])
        return None, 'build'

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
                return None, 'run'
            values.append(output[attribute])
    except Exception as e:
        env.set_default_blender_executable()
        on_progress([commit_hash, date_str(commit_ts), title, 'error', str(e)[:30]])
        return None, 'run'

    env.set_default_blender_executable()
    avg = sum(values) / len(values)

    good = is_good(avg, success, threshold)
    status = 'PASS' if good else 'FAIL'
    on_progress([commit_hash, date_str(commit_ts), title, f'{avg:.4f}', status])
    return avg, 'pass' if good else 'fail'
