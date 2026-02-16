# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

r"""
Run clang-tidy with the modernize-use-designated-initializers check on all C/C++ files in the compile_commands.json, fixing issues in-place.

Requirements:
- clang-tidy available on PATH, OR set CLANG_TIDY / CLANG_TIDY_PATH to a full path.

Optional Logging:
- Set TIDY_LOG (or CLANG_TIDY_LOG) to write a detailed log file.

Examples (PowerShell):

  $env:CLANG_TIDY = "C:\Path\To\clang-tidy.exe"   # optional if clang-tidy is on PATH

  Where to write a log:
  $env:TIDY_LOG   = "C:\temp\tidy_run.log"

Optional:
- Set TIDY_SUBDIR to restrict fixes to a subdirectory, e.g.:

    $env:TIDY_SUBDIR = "source/blender/nodes"

    Raw:
    $env:CLANG_TIDY = "$env:USERPROFILE\.vscode\extensions\ms-vscode.cpptools-*\LLVM\bin\clang-tidy.exe"
    $env:TIDY_LOG = "C:\temp\tidy_run.log"

  python .\tools\utils_maintenance\tidy_designated_init.py `
    .\out\build\x64-release-clangd\compile_commands.json `
    . `

    With TIDY_SUBDIR:
    $env:CLANG_TIDY = "$env:USERPROFILE\.vscode\extensions\ms-vscode.cpptools-*\LLVM\bin\clang-tidy.exe"
    $env:TIDY_LOG = "C:\temp\tidy_run.log"
    $env:TIDY_SUBDIR = "source/blender/nodes"

    python .\tools\utils_maintenance\tidy_designated_init.py `
      .\out\build\x64-release-clangd\compile_commands.json `
      . `
"""

__all__ = (
    "main",
)

import json
import os
import shutil
import shlex
import subprocess
import sys
from pathlib import Path
from datetime import datetime, timezone

print("tidy_designated_init: starting...")
sys.stdout.flush()  # Ensure the above message is printed before any potential buffering from subprocesses.

def cmd_as_text(cmd: list[str]) -> str:
    """Return a copy/pasteable command string for the current platform."""
    # Note: On Windows, type checkers may treat `os.name` as a Literal["nt"],
    # which can incorrectly flag the non-Windows branch as unreachable.
    if getattr(os, "name", "") == "nt":
        return subprocess.list2cmdline(cmd)
    return shlex.join(cmd)

def main():
    # Usage:
    #   python tidy_designated_init.py <compile_commands.json> [root] [jobs]
    cc_path = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve() if len(sys.argv) >= 3 else cc_path.parent.resolve()
    tidy_subdir = os.environ.get("TIDY_SUBDIR")
    if tidy_subdir:
        tidy_subdir = Path(tidy_subdir)
        if not tidy_subdir.is_absolute():
            tidy_subdir = (root / tidy_subdir).resolve()
        else:
            tidy_subdir = tidy_subdir.resolve()

    extra_args = [
        "--fix",
        "--fix-errors",
        "--format-style=file",
        '--config={"Checks":"-*,modernize-use-designated-initializers"}',
    ]

    data = json.loads(cc_path.read_text(encoding="utf-8"))
    files = []
    seen = set()

    for entry in data:
        f = Path(entry["file"])
        if not f.is_absolute():
            f = (Path(entry.get("directory", root)) / f).resolve()

        # Only touch files inside repo root
        try:
            f.relative_to(root)
        except ValueError:
            continue

        # Only C/C++ sources/headers that Blender actually compiles
        if f.suffix.lower() not in {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}:
            continue

        # Skip generated/build files
        if "build" in f.parts:
            continue

        # --- OPTIONAL SUBDIR FILTER ---
        if tidy_subdir is not None:
            try:
                f.relative_to(tidy_subdir)
            except ValueError:
                continue

        sf = str(f)
        if sf not in seen:
            seen.add(sf)
            files.append(sf)

    print(f"Collected {len(files)} files, starting clang-tidy")
    sys.stdout.flush()

    # Optional log file (set via env var TIDY_LOG or CLANG_TIDY_LOG)
    log_path = os.environ.get("TIDY_LOG") or os.environ.get("CLANG_TIDY_LOG")
    log_file = None
    if log_path:
        try:
            log_file = open(log_path, "a", encoding="utf-8")
            log_file.write(
                f"\n--- tidy_designated_init run at {datetime.now(timezone.utc).isoformat().replace('+00:00', 'Z')} ---\n"
            )
            log_file.write(f"Will run clang-tidy on {len(files)} files\n")
        except Exception as e:
            print(f"Warning: could not open log file '{log_path}': {e}")
            log_file = None

    # Allow explicit override via environment variable.
    env_path = os.environ.get("CLANG_TIDY") or os.environ.get("CLANG_TIDY_PATH")
    clang_tidy_exe = None
    if env_path:
        if Path(env_path).exists():
            clang_tidy_exe = str(Path(env_path))
        else:
            print(f"Warning: CLANG_TIDY path '{env_path}' does not exist (ignoring)")

    # Ensure clang-tidy is available on PATH (Windows will need clang-tidy.exe available).
    if not clang_tidy_exe:
        clang_tidy_exe = shutil.which("clang-tidy")

    if not clang_tidy_exe:
        print("Error: 'clang-tidy' not found in PATH. Install LLVM/Clang tools or add clang-tidy to your PATH.")
        print("On Windows, add the directory containing clang-tidy.exe to PATH or set CLANG_TIDY/CLANG_TIDY_PATH to point to it.")
        if log_file:
            log_file.write("Error: 'clang-tidy' not found in PATH.\n")
            log_file.close()
        sys.exit(2)

    # Run in chunks to avoid command-line length limits
    chunk = 200
    total_chunks = (len(files) + chunk - 1) // chunk
    try:
        for chunk_index, i in enumerate(range(0, len(files), chunk), start=1):
            batch = files[i : i + chunk]
            non_file_args = [clang_tidy_exe, f"-p={cc_path.parent}", *extra_args]
            cmd = [*non_file_args, *batch]

            # This goes to print() only:
            print(
                f"[{chunk_index}/{total_chunks}] "
                f"clang-tidy {len(batch)} files "
                f"({i + 1}-{i + len(batch)})"
            )

            try:
                r = subprocess.run(cmd, cwd=str(root), capture_output=True, text=True)
            except Exception as e:
                print(f"Error running clang-tidy: {e}")
                if log_file:
                    log_file.write(f"Error running clang-tidy on batch: {e}\n")
                    log_file.flush()
                sys.exit(2)

            if log_file:
                log_file.write(f"\n=== Chunk {chunk_index}/{total_chunks} ===\n")
                log_file.write(
                    f"Files: {i + 1}-{i + len(batch)} "
                    f"({len(batch)} files)\n"
                )

                log_file.write("Command (non-file args):\n")
                for arg in non_file_args:
                    log_file.write(f"  {arg}\n")

                log_file.write("Files:\n")
                for f in batch:
                    log_file.write(f"  {f}\n")

                    log_file.write(f"Command (copy/paste):\n")
                    log_file.write(f"  {cmd_as_text(cmd)}\n")

                if r.stdout:
                    log_file.write("--- STDOUT ---\n")
                    log_file.write(r.stdout)

                if r.stderr:
                    log_file.write("--- STDERR ---\n")
                    log_file.write(r.stderr)

                log_file.write(f"Exit code: {r.returncode}\n")
                log_file.flush()
    finally:
        if log_file:
            log_file.close()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python tidy_designated_init.py <compile_commands.json> [repo_root] [jobs]")
        sys.exit(2)
    main()
    # Ensure any open log file is closed by exit path (main handles closing on error paths).
