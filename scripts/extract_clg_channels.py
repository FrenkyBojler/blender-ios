#!/usr/bin/env python3
"""
Extract CLG log channel strings from Blender source files.

Usage:
  python3 scripts/extract_clg_channels.py [path]

Defaults to: source/blender
Prints a sorted, unique list of channel names (one per line).



This can also be run in bash via
grep -R --line-number -Pzo 'CLG_LOGREF_DECLARE_GLOBAL\s*\([^)]*\)|static\s+CLG_LogRef\s+[A-Za-z0-9_]+\s*=\s*\{[^}]*\}' source/blender | tr '\0' '\n' | sed -nE 's/.*"([^"]+)".*/\1/p; s/.*\{\s*"([^\"]+)"\s*\}.*/\1/p' | sort -u


"""
import argparse
import os
import re

DEFAULT_ROOT = "source/blender"

# Patterns to match. Use DOTALL so macros spanning multiple lines are handled.
PATTERNS = [
    re.compile(r'CLG_LOGREF_DECLARE_GLOBAL\s*\([^)]*"([^\"]+)"[^)]*\)', re.DOTALL),
    re.compile(r'static\s+CLG_LogRef\s+[A-Za-z0-9_]+\s*=\s*\{\s*"([^\"]+)"\s*\}', re.DOTALL),
    # Also match non-static initializers: CLG_LogRef NAME = {"channel"};
    re.compile(r'CLG_LogRef\s+[A-Za-z0-9_]+\s*=\s*\{\s*"([^\"]+)"\s*\}', re.DOTALL),
]

# File extensions to search (common C/C++/headers and any file as fallback).
EXTS = ['.c', '.cc', '.cpp', '.h', '.hh', '.hpp']


def find_files(root):
    for dirpath, dirnames, filenames in os.walk(root):
        for fn in filenames:
            # prefer common source file extensions, but include any file to be safe
            yield os.path.join(dirpath, fn)


def is_likely_text(file_path):
    # quick heuristic: skip very large files and binary files
    # This really only applies to `source/blender/draw/engines/eevee/eevee_lut.cc`
    try:
        size = os.path.getsize(file_path)
        if size > 2_000_000:  # skip files > 2MB
            return False
        with open(file_path, 'rb') as f:
            chunk = f.read(8192)
            if b'\0' in chunk:
                return False
    except Exception:
        return False
    return True


def extract_from_text(text):
    found = []
    for pat in PATTERNS:
        for m in pat.finditer(text):
            grp = m.group(1)
            if grp:
                found.append(grp)
    return found


def main():
    parser = argparse.ArgumentParser(description='Extract CLG log channel strings from source.')
    parser.add_argument('root', nargs='?', default=DEFAULT_ROOT, help='directory to search (default: source/blender)')
    parser.add_argument('--ext-only', action='store_true', help='only search files with common source extensions')
    parser.add_argument('--out', '-o', help='write output to file instead of stdout (also always writes CLOG_args.txt')
    args = parser.parse_args()

    root = args.root
    if not os.path.isdir(root):
        parser.error(f"Path not found or not a directory: {root}")

    channels = set()

    for fp in find_files(root):
        if args.ext_only and not any(fp.endswith(ext) for ext in EXTS):
            continue
        if not is_likely_text(fp):
            continue
        try:
            with open(fp, 'r', encoding='utf-8', errors='ignore') as f:
                text = f.read()
        except Exception:
            continue

        for ch in extract_from_text(text):
            ch = ch.strip()
            # only accept simple channel names: letters/digits/underscore/dot
            if re.fullmatch(r'[A-Za-z0-9_.]+', ch):
                channels.add(ch)

    out_lines = sorted(channels)

    # Always write the canonical file next to this script.
    script_dir = os.path.dirname(os.path.abspath(__file__))
    canonical_path = os.path.join(script_dir, 'CLOG_args.txt')
    try:
        with open(canonical_path, 'w', encoding='utf-8') as f:
            for l in out_lines:
                f.write(l + '\n')
    except Exception as e:
        print(f'Error writing canonical output {canonical_path}: {e}')

    # If user requested a different output path, also write that.
    if args.out:
        try:
            with open(args.out, 'w', encoding='utf-8') as f:
                for l in out_lines:
                    f.write(l + '\n')
        except Exception as e:
            print(f'Error writing output {args.out}: {e}')

    # Print only the canonical output path and any user-specified output path.
    if args.out:
        print(f'Wrote: {canonical_path} and {args.out}')
    else:
        print(f'Wrote: {canonical_path}')


if __name__ == '__main__':
    main()
