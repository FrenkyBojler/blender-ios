#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2020-2023 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0
"""
The main entry point to running benchmark tests.

See https://developer.blender.org/docs/handbook/testing/performance/
for a general introduction to the topic.
"""

import api
import argparse
import fnmatch
import glob
import logging
import pathlib
import shutil
import sys


def find_blender_git_dir() -> pathlib.Path:
    # Find .git directory of the repository we are in.
    cwd = pathlib.Path.cwd()

    for path in [cwd] + list(cwd.parents):
        if (path / '.git').exists():
            return path

    return None


def get_tests_base_dir(blender_git_dir: pathlib.Path) -> pathlib.Path:
    # Benchmarks dir is next to the Blender source folder.
    return blender_git_dir.parent / 'benchmark'


def use_revision_columns(config: api.TestConfig) -> bool:
    return (
        config.benchmark_type == "comparison" and
        len(config.queue.entries) > 0
    )


def init_table(config: api.TestConfig) -> api.MarkdownTable:
    table = api.MarkdownTable()
    table.add_column("Revision")
    table.add_column("Category", is_visible=config.queue.has_multiple_categories)
    table.add_column("Device", is_visible=config.queue.has_multiple_devices)
    table.add_column("Test", width=40)
    if use_revision_columns(config):
        for revision_name in config.revision_names():
            table.add_column(revision_name, width=20, alignment='RIGHT')
        table.columns[0].is_visible = False
    else:
        table.add_column("Result", width=20, alignment='RIGHT')
    return table


def print_row(table: api.MarkdownTable, entries: list, end='\n') -> None:
    # Print one or more test entries on a row.
    row = []

    # For time series, revision is printed first.
    row.append(entries[0].revision)
    row.append(entries[0].category)
    row.append(entries[0].device_type)
    row.append(entries[0].test)

    for entry in entries:
        # Show time or status.
        status = entry.status
        output = entry.output
        result = ''
        if status in {'done', 'outdated'} and output:
            if 'time' in output:
                result = '%7.4f s' % output['time']
            elif 'fps' in output:
                result = '%8.3f fps' % output['fps']

            if status == 'outdated':
                result += " (outdated)"
        elif status == 'failed':
            result = "failed: " + entry.error_msg
        else:
            result = status
        row.append(result)

    table.print_row(row, end=end)


def print_entry(table: api.MarkdownTable, entry: api.TestEntry) -> None:
    # Print a single test entry, potentially on multiple lines, with more details than in `print_row`.
    # NOTE: Currently only used to print detailed error info.

    print_row(table, [entry])

    if entry.status != 'failed':
        return
    if not entry.exception_msg:
        return
    print(entry.exception_msg, flush=True)


def match_entry(entry: api.TestEntry, args: argparse.Namespace):
    # Filter tests by name and category.
    return (
        fnmatch.fnmatch(entry.test, args.test) or
        fnmatch.fnmatch(entry.category, args.test) or
        entry.test.find(args.test) != -1 or
        entry.category.find(args.test) != -1
    )


def run_entry(env: api.TestEnvironment,
              config: api.TestConfig,
              table: api.MarkdownTable,
              row: list,
              entry: api.TestEntry,
              update_only: bool,
              count: int,
              update_submodules: bool = True):
    updated = False
    failed = False

    # Check if entry needs to be run.
    if update_only and entry.status not in {'queued', 'outdated'}:
        print_row(table, row, end='\r')
        return updated, failed

    # Run test entry.
    revision = entry.revision
    git_hash = entry.git_hash
    environment = entry.environment
    testname = entry.test
    testcategory = entry.category
    device_type = entry.device_type
    device_id = entry.device_id
    gpu_backend = {
        'VULKAN': 'vulkan',
        'METAL': 'metal',
        'OPENGL': 'opengl'
    }.get(device_type, 'default')

    test = config.tests.find(testname, testcategory)
    if not test:
        return updated, failed

    updated = True

    # Log all output to dedicated log file.
    logname = testcategory + '_' + testname + '_' + device_id + '_' + revision
    env.set_log_file(config.logs_dir / (logname + '.log'), clear=True)

    # Clear output
    entry.output = None
    entry.error_msg = ''

    # Build revision, or just set path to existing executable.
    executable_ok = True
    if len(entry.executable):
        env.set_blender_executable(pathlib.Path(entry.executable), environment)
    else:
        entry.status = 'building'
        print_row(table, row, end='\r')

        if config.benchmark_type == "comparison":
            install_dir = config.builds_dir / revision
        else:
            install_dir = env.install_dir
        executable_ok = env.build(git_hash, install_dir, update_submodules)

        if not executable_ok:
            entry.status = 'failed'
            entry.error_msg = 'Failed to build'
            failed = True
        else:
            env.set_blender_executable(install_dir, environment)

    # Run test and update output and status.
    if executable_ok:
        run_outputs = []
        for run in range(count):
            entry.status = 'running' if count == 1 else f'run [{run + 1}/{count}]'
            print_row(table, row, end='\r')

            try:
                output = test.run(env, device_id, gpu_backend)
                if not output:
                    raise Exception("Test produced no output")
                run_outputs.append(output)
                entry.status = 'done'
            except KeyboardInterrupt as e:
                raise e
            except Exception as e:
                failed = True
                entry.status = 'failed'
                entry.error_msg = 'Failed to run'
                entry.exception_msg = str(e)
                break

        if entry.status == 'done' and run_outputs:
            # Combine results from runs

            keys = set()
            for run_output in run_outputs:
                keys |= run_output.keys()

            output = {}
            output_all_runs = {}
            for key in keys:
                values = []
                for run_output in run_outputs:
                    if key not in run_output:
                        continue
                    values.append(run_output[key])
                output[key] = sum(values) / len(values)
                output_all_runs[key] = values
            entry.output = output
            entry.output_all_runs = output_all_runs

    print_row(table, row, end='\r')

    # Update device name in case the device changed since the entry was created.
    entry.device_name = config.device_name(device_id)

    # Restore default logging and Blender executable.
    env.unset_log_file()
    env.set_default_blender_executable()

    return updated, failed


def cmd_init(env: api.TestEnvironment, argv: list):
    # Initialize benchmarks folder.
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', default=False, action='store_true')
    parser.add_argument('--blender')
    args = parser.parse_args(argv)
    env.set_log_file(env.base_dir / 'setup.log', clear=False)
    env.init(args.build, args.blender)
    env.unset_log_file()


def cmd_list(env: api.TestEnvironment, argv: list) -> None:
    # List devices, tests and configurations.
    print('DEVICES')
    machine = env.get_machine()
    for device in machine.devices:
        name = f"{device.name} ({device.operating_system})"
        print(f"{device.id: <15} {name}")
    print('')

    print('TESTS')
    collection = api.TestCollection(env)
    for test in collection.tests:
        print(f"{test.category(): <15} {test.name(): <50}")
    print('')

    print('CONFIGS')
    configs = env.get_config_names()
    for config_name in configs:
        print(config_name)


def cmd_status(env: api.TestEnvironment, argv: list):
    # Print status of tests in configurations.
    parser = argparse.ArgumentParser()
    parser.add_argument('config', nargs='?', default=None)
    parser.add_argument('test', nargs='?', default='*')
    args = parser.parse_args(argv)

    configs = env.get_configs(args.config)
    first = True
    for config in configs:
        if not args.config:
            if first:
                first = False
            else:
                print("")
            print(config.name.upper())

        table = init_table(config)
        table.print_header()
        for row in config.queue.rows(use_revision_columns(config)):
            if match_entry(row[0], args):
                print_row(table, row)


def cmd_reset(env: api.TestEnvironment, argv: list):
    # Reset tests to re-run them.
    parser = argparse.ArgumentParser()
    parser.add_argument('config', nargs='?', default=None)
    parser.add_argument('test', nargs='?', default='*')
    args = parser.parse_args(argv)

    configs = env.get_configs(args.config)
    for config in configs:
        table = init_table(config)
        table.print_header()
        for row in config.queue.rows(use_revision_columns(config)):
            if match_entry(row[0], args):
                for entry in row:
                    entry.status = 'queued'
                    entry.result = {}
                print_row(table, row)

        config.queue.write()

        if args.test == '*':
            shutil.rmtree(config.logs_dir)


def cmd_run(env: api.TestEnvironment, argv: list, update_only: bool):
    # Run tests.
    parser = argparse.ArgumentParser()
    parser.add_argument('config', nargs='?', default=None)
    parser.add_argument('test', nargs='?', default='*')
    parser.add_argument('--count', default=1, type=int, help="Number of runs to perform (default=1)")
    parser.add_argument(
        '--no-submodules',
        action='store_true',
        help="Skip updating submodules when checking out revisions. Useful when testing performance regressions for library changes.")
    args = parser.parse_args(argv)

    exit_code = 0

    configs = env.get_configs(args.config)
    for config in configs:
        updated = False
        cancel = False
        table = init_table(config)
        table.print_header()
        for row in config.queue.rows(use_revision_columns(config)):
            if match_entry(row[0], args):
                for entry in row:
                    try:
                        test_updated, test_failed = run_entry(
                            env, config, table, row, entry, update_only, args.count, not args.no_submodules)
                        if test_updated:
                            updated = True
                            # Write queue every time in case running gets interrupted,
                            # so it can be resumed.
                            config.queue.write()
                        if test_failed:
                            exit_code = 1
                            print_entry(table, entry)
                    except KeyboardInterrupt as e:
                        cancel = True
                        break

                print_row(table, row)

            if cancel:
                break

        if updated:
            # Generate graph if test were run.
            json_filepath = config.base_dir / "results.json"
            html_filepath = config.base_dir / "results.html"
            graph = api.TestGraph([json_filepath])
            graph.write(html_filepath)

            print("\nfile://" + str(html_filepath))

    sys.exit(exit_code)


def _resolve_device(env: api.TestEnvironment, device_str: str):
    """Resolve a device string to a device_id and gpu_backend pair."""
    machine = env.get_machine(need_gpus=True)
    device_id = device_str
    gpu_backend = 'default'

    for device in machine.devices:
        if device.id == device_str or device.type == device_str:
            device_id = device.id
            gpu_backend = {
                'VULKAN': 'vulkan',
                'METAL': 'metal',
                'OPENGL': 'opengl'
            }.get(device.type, 'default')
            break

    return device_id, gpu_backend


def cmd_bisect(env: api.TestEnvironment, argv: list):
    import datetime

    parser = argparse.ArgumentParser()
    parser.add_argument('--device', required=True)
    parser.add_argument('--category', required=True)
    parser.add_argument('--test', required=True)
    parser.add_argument('--attribute', required=True)
    parser.add_argument('--threshold', required=True, type=float)
    parser.add_argument('--success', required=True, choices=['greater_than', 'less_than'])
    parser.add_argument('--range', required=True)
    parser.add_argument('--count', default=1, type=int)
    args = parser.parse_args(argv)

    if not env.build_dir.exists() or not env.blender_dir.exists():
        sys.stderr.write('Error: benchmark build not initialized. Run "benchmark.py init --build" first.\n')
        sys.exit(1)

    try:
        start_str, end_str = args.range.split('-')
        start_dt = datetime.datetime.strptime(start_str, '%Y%m%d').replace(tzinfo=datetime.timezone.utc)
        end_dt = datetime.datetime.strptime(end_str, '%Y%m%d').replace(tzinfo=datetime.timezone.utc)
    except:
        sys.stderr.write('Error: invalid date range format. Use YYYYMMDD-YYYYMMDD\n')
        sys.exit(1)
    if start_dt >= end_dt:
        sys.stderr.write(f'Error: invalid date range {start_str} must be before {end_str}\n')
        sys.exit(1)

    collection = api.TestCollection(env, [args.test], [args.category])
    test = collection.find(args.test, args.category)
    if not test:
        sys.stderr.write(f'Error: test not found: {args.category}/{args.test}\n')
        sys.exit(1)

    device_id, gpu_backend = _resolve_device(env, args.device)
    threshold = args.threshold
    count = args.count

    commit_hash_len = 12
    commit_title_max_len = 70

    def commits_in_window(after_ts, before_ts):
        try:
            lines = env.call(
                [env.git_executable, 'log', '--first-parent', '--reverse',
                 '--after=' + str(after_ts - 1), '--before=' + str(before_ts),
                 '--format=%H %at', 'HEAD'],
                env.blender_git_dir, silent=True)
        except:
            return []
        result = []
        for line in lines:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) >= 2:
                try:
                    result.append((parts[0][:commit_hash_len], int(parts[1])))
                except:
                    pass
        return result

    def date_str(ts):
        return datetime.datetime.fromtimestamp(ts, tz=datetime.timezone.utc).strftime('%Y-%m-%d %H:%M:%S')

    def is_good(value):
        if args.success == 'greater_than':
            return value > threshold
        elif args.success == 'less_than':
            return value < threshold
        return false

    print(f"Device: {args.device}")
    print(f"Category: {args.category}")
    print(f"Test: {args.test}")
    print()

    table = api.MarkdownTable()
    table.add_column("Date (UTC)", width=22)
    table.add_column("Commit", width=commit_hash_len)
    table.add_column("Title", width=commit_title_max_len)
    table.add_column(args.attribute, width=14, alignment='RIGHT')
    table.add_column("Status", width=8)
    table.print_header()

    tested = set()
    _title_cache = {}

    def commit_title(git_hash):
        if git_hash in _title_cache:
            return _title_cache[git_hash]
        try:
            lines = env.call(
                [env.git_executable, 'log', '-n1', '--format=%s', git_hash],
                env.blender_git_dir, silent=True)
            title = lines[0].strip() if lines else ''
        except:
            title = ''
        _title_cache[git_hash] = title
        return title

    def test_commit(git_hash, ts):
        if git_hash in tested:
            return None, 'skip'
        tested.add(git_hash)

        title = commit_title(git_hash)[:commit_title_max_len]
        table.print_row([date_str(ts), git_hash, title, '', 'building'], end='\r')

        install_dir = env.install_dir
        ok = env.build(git_hash, install_dir)
        if not ok:
            table.print_row([date_str(ts), git_hash, title, 'error', 'FAIL (build)'])
            return None, 'build'

        env.set_blender_executable(install_dir, {})

        values = []
        try:
            for run_idx in range(count):
                run_status = 'running' if count == 1 else f'run [{run_idx + 1}/{count}]'
                table.print_row([date_str(ts), git_hash, title, '', run_status], end='\r')
                output = test.run(env, device_id, gpu_backend)
                if not output or args.attribute not in output:
                    env.set_default_blender_executable()
                    table.print_row([date_str(ts), git_hash, title, 'error', 'run'])
                    return None, 'run'
                values.append(output[args.attribute])
        except Exception as e:
            env.set_default_blender_executable()
            table.print_row([date_str(ts), git_hash, title, 'error', str(e)[:30]])
            return None, 'run'

        env.set_default_blender_executable()
        avg = sum(values) / len(values)

        good = is_good(avg)
        status = 'PASS' if good else 'FAIL'
        table.print_row([date_str(ts), git_hash, title, f'{avg:.4f}', status])
        return avg, 'pass' if good else 'fail'

    # Phase 1: Daily scan
    start_ts = int(start_dt.timestamp())
    end_ts = int(end_dt.timestamp())

    good_commit = None
    good_ts = None
    bad_commit = None
    bad_ts = None
    day_ts = start_ts
    last_tested = None

    while day_ts <= end_ts:
        next_day_ts = day_ts + 86400
        day_commits = commits_in_window(day_ts, next_day_ts)

        attempts = 0
        for chash, cts in day_commits:
            if chash == last_tested:
                continue
            if attempts >= 3:
                break
            attempts += 1
            _, status = test_commit(chash, cts)
            if status == 'build':
                continue
            if status == 'pass':
                good_commit = chash
                good_ts = cts
            else:
                bad_commit = chash
                bad_ts = cts
            last_tested = chash
            break

        if bad_commit:
            break
        day_ts = next_day_ts

    if bad_commit is None:
        print('\nNo regression found in the given date range.')
        return

    # Phase 2: Hourly scan between good and bad timestamps
    good_ts = good_ts or start_ts

    all_commits = commits_in_window(good_ts, bad_ts)
    hour_groups = {}
    for chash, cts in all_commits:
        hour_key = (cts // 3600) * 3600
        if hour_key not in hour_groups:
            hour_groups[hour_key] = []
        hour_groups[hour_key].append((chash, cts))

    bad_hour_commit = bad_commit
    bad_hour_ts = bad_ts
    good_hour_commit = good_commit
    good_hour_ts = good_ts
    hourly_found_bad = False

    for hour_key in sorted(hour_groups.keys()):
        hour_commits = hour_groups[hour_key]
        untested = [(h, t) for h, t in hour_commits if h not in tested]
        if not untested:
            continue

        attempts = 0
        for chash, cts in untested:
            if attempts >= 3:
                break
            attempts += 1
            _, status = test_commit(chash, cts)
            if status == 'build':
                continue
            if status == 'pass':
                good_hour_commit = chash
                good_hour_ts = cts
            else:
                bad_hour_commit = chash
                bad_hour_ts = cts
                hourly_found_bad = True
            break

        if hourly_found_bad:
            break

    # Phase 3: Per-commit scan between good and bad hour commits
    per_commits = commits_in_window(good_hour_ts, bad_hour_ts)
    for chash, cts in per_commits:
        if chash in tested:
            continue
        _, status = test_commit(chash, cts)
        if status == 'fail':
            title = commit_title(chash)
            print(f'\nRegression introduced by commit {chash}: {title} ({date_str(cts)})')
            return
        elif status == 'pass':
            continue

    print('\nCould not pinpoint the exact commit.')


def cmd_graph(argv: list):
    # Create graph from a given JSON results file.
    parser = argparse.ArgumentParser()
    parser.add_argument('json_file', nargs='+')
    parser.add_argument('-o', '--output', type=str, required=True)
    args = parser.parse_args(argv)

    # For directories, use all json files in the directory.
    json_files = []
    for path in args.json_file:
        path = pathlib.Path(path)
        if path.is_dir():
            for filepath in glob.iglob(str(path / '*.json')):
                json_files.append(pathlib.Path(filepath))
        else:
            json_files.append(path)

    graph = api.TestGraph(json_files)
    graph.write(pathlib.Path(args.output))


def main():
    logging.basicConfig()
    usage = ('benchmark <command> [<args>]\n'
             '\n'
             'Commands:\n'
             '  init [--build]                       Init benchmarks directory and default config\n'
             '                                       Optionally with automated revision building setup\n'
             '  \n'
             '  list                                 List available tests, devices and configurations\n'
             '  \n'
             '  run [<config>] [<test>]              Execute all tests in configuration\n'
             '  update [<config>] [<test>]           Execute only queued and outdated tests\n'
             '  reset [<config>] [<test>]            Clear tests results in configuration\n'
             '  status [<config>] [<test>]           List configurations and their tests\n'
             '  \n'
             '  graph a.json b.json... -o out.html   Create graph from results in JSON files\n'
             '  \n'
             '  bisect                                Find commit that introduced a regression'
             ' between dates\n')

    parser = argparse.ArgumentParser(
        description='Blender performance testing',
        usage=usage)

    parser.add_argument('command', nargs='?', default='help')
    args = parser.parse_args(sys.argv[1:2])

    argv = sys.argv[2:]
    blender_git_dir = find_blender_git_dir()
    if blender_git_dir is None:
        sys.stderr.write('Error: no blender git repository found from current working directory\n')
        sys.exit(1)

    if args.command == 'graph':
        cmd_graph(argv)
        sys.exit(0)

    base_dir = get_tests_base_dir(blender_git_dir)
    env = api.TestEnvironment(blender_git_dir, base_dir)
    if args.command == 'init':
        cmd_init(env, argv)
        sys.exit(0)

    if not env.base_dir.exists():
        sys.stderr.write(
            'Error: benchmark directory not initialized. '
            'Run the \"init\" command to create the directory and a default configuration.\n')
        sys.exit(1)

    if args.command == 'list':
        cmd_list(env, argv)
    elif args.command == 'run':
        cmd_run(env, argv, update_only=False)
    elif args.command == 'update':
        cmd_run(env, argv, update_only=True)
    elif args.command == 'reset':
        cmd_reset(env, argv)
    elif args.command == 'bisect':
        cmd_bisect(env, argv)
    elif args.command == 'status':
        cmd_status(env, argv)
    elif args.command == 'help':
        parser.print_usage()
    else:
        sys.stderr.write(f'Unknown command: {args.command}\n')


if __name__ == '__main__':
    main()
