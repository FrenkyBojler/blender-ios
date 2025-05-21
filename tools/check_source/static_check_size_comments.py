#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

r"""
Validates sizes in C/C++ sources written as: ``type name[/*NAME_MAX*/ 64]``
where NAME_MAX is expected to be a define equal to 64, otherwise a warning is reported.
"""
__all__ = (
    "main",
)

import os
import sys

THIS_DIR = os.path.dirname(__file__)
sys.path.append(os.path.join(THIS_DIR, "..", "utils_maintenance", "modules"))

from batch_edit_text import run

BASE_DIR = os.path.normpath(os.path.abspath(os.path.normpath(os.path.join(THIS_DIR, "..", ".."))))

# TODO, move to config file
SOURCE_DIRS = (
    "source",
)

SOURCE_EXT = (
    # C/C++
    ".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".cxx", ".hxx", ".inl",
    # Objective C
    ".m", ".mm",
    # GLSL
    ".glsl",
)

# Mainly useful for development to check extraction & validation are working.
SHOW_SUCCESS = True

# Map defines to a list of (filename-split, value) pairs.
global_defines: dict[
    # The define ID.
    str,
    # Value(s), in case it's defined in multiple files.
    list[
        tuple[
            # The `BASE_DIR` relative path (split by `os.sep`).
            tuple[str, ...],
            # The value of the define,
            # a literal string with comments stripped out.
            str,
        ],
    ],
] = {}

import re
re_sizes = re.compile("\\[\\/\\*([^\\s]+)\\*\\/\\s*(\\d+)\\]")
re_defines = re.compile("^\\s*#\\s*define\\s+([A-Za-z_][A-Za-z_0-9]*)[ \t]+([^\n]+)", re.MULTILINE)

re_c_ids = re.compile("[A-Za-z0-9_]+")


def extract_defines(filepath: str, data_src: str) -> None:
    filepath_rel = os.path.relpath(filepath, BASE_DIR)
    for m in re_defines.finditer(data_src):
        value_id = m.group(1)
        value_literal = m.group(2)

        # Weak comment stripping.
        # This is (arguably) acceptable since the intent is to extract numbers,
        # if developers feel the need to write lines such as:
        # `#define VALUE_MAX /* Lets make some trouble! */ 64`
        # Then they can consider if that's actually needed (sigh!)...
        # Otherwise, we could replace this with a full parser such as CLANG,
        # however this is a bit of a hassle to setup.
        if "//" in value_literal:
            value_literal = value_literal.split("//", 1)[0]
        if "/*" in value_literal:
            value_literal = value_literal.split("/*", 1)[0]

        try:
            global_defines[value_id].append((tuple(filepath_rel.split(os.sep)), value_literal))
        except KeyError:
            global_defines[value_id] = [(tuple(filepath_rel.split(os.sep)), value_literal)]

    # Returning None indicates the file is not edited.


def path_score_distance(a: tuple[str, ...], b: tuple[str, ...]) -> tuple[int, int]:
    """
    Compare two paths, to find which paths are "closer" to each-other.
    This is used as a tie breaker when defines are found in multiple headers.
    """
    count_shared = 0
    range_min = min(len(a), len(b))
    range_max = max(len(a), len(b))
    for i in range(range_min):
        if a[i] != b[i]:
            break
        count_shared += 1

    count_nested = range_max - count_shared
    # Negate shared so smaller is better.
    # Less path nesting also gets priority.
    return (-count_shared, count_nested)


def eval_define(
        value_literal: str,
        *,
        default: str,
        lookups_failed: list[str],
        filepath_ref_split: tuple[str, ...],
) -> str:
    failed: list[str] = []

    def re_replace_fn(match: re.Match[str]) -> str:
        value = match.group()
        if value.isdigit():
            return value

        other_values = global_defines.get(value)
        if other_values is None:
            failed.append(value)
            return value

        if len(other_values) == 1:
            other_filepath_split, other_literal = other_values[0]
        else:
            # Find the "closest" on the file system.
            # In practice favor paths which are co-located works fairly well,
            # needed as it's now known which headers ID's in a head *could* reference.
            other_literal_best = ""
            other_score_best = (0, 0)
            other_filepath_split_best: tuple[str, ...] = ("",)

            for other_filepath_split_test, other_literal_test in other_values:
                other_score_test = path_score_distance(filepath_ref_split, other_filepath_split_test)
                if (
                    # First time.
                    (not other_literal_best) or
                    # A lower score has been found (smaller is better).
                    (other_score_test < other_score_best)
                ):
                    other_literal_best = other_literal_test
                    other_score_best = other_score_test
                    other_filepath_split_best = other_filepath_split_test
                del other_score_test
            other_literal = other_literal_best
            other_filepath_split = other_filepath_split_best
            del other_literal_best, other_score_best, other_filepath_split_best

        other_literal_eval = eval_define(
            other_literal,
            default="",
            lookups_failed=failed,
            filepath_ref_split=other_filepath_split,
        )
        if other_literal_eval:
            return other_literal_eval

        failed.append(value)
        return value

    # Use integer division.
    value_literal = value_literal.replace(r"/", r"//")

    value_literal_eval = re_c_ids.sub(re_replace_fn, value_literal)

    if failed:
        # One or more ID could not be found.
        lookups_failed.extend(failed)
        return default

    # This could use exception handling, don't unless it's needed though.
    return str(eval(value_literal_eval))


def validate_sizes(filepath: str, data_src: str) -> None:
    # Nicer for printing.
    filepath_rel = os.path.relpath(filepath, BASE_DIR)
    filepath_rel_split = tuple(filepath_rel.split(os.sep))
    for m in re_sizes.finditer(data_src):
        value_id = m.group(1)
        value_literal = m.group(2)

        lookups_failed: list[str] = []
        value_eval = eval_define(
            value_id,
            default="",
            lookups_failed=lookups_failed,
            filepath_ref_split=filepath_rel_split,
        )

        if lookups_failed:
            print("WARNING:", "[{:s}]".format(", ".join(lookups_failed)), "unknown", "in", filepath_rel)
            continue

        if value_literal != value_eval:
            print("WARNING:", value_id, "mismatch", "({:s} != {:s})".format(value_literal, value_eval), filepath_rel)
            continue

        if SHOW_SUCCESS:
            print("OK:", "{:s}={:s},".format(value_id, value_literal), "in", filepath_rel)

    # Returning None indicates the file is not edited.


def main() -> int:

    # Extract defines.
    run(
        directories=[os.path.join(BASE_DIR, d) for d in SOURCE_DIRS],
        is_text=lambda filepath: filepath.endswith(SOURCE_EXT),
        text_operation=extract_defines,
        # Can't be used if we want to accumulate in a global variable.
        use_multiprocess=False,
    )

    # For predictable lookups on tie breakers.
    # In practice it should almost never matter.
    for values in global_defines.values():
        if len(values) > 1:
            values.sort()

    # Validate sizes.
    run(
        directories=[os.path.join(BASE_DIR, d) for d in SOURCE_DIRS],
        is_text=lambda filepath: filepath.endswith(SOURCE_EXT),
        text_operation=validate_sizes,
        # Can't be used if we want to accumulate in a global variable.
        use_multiprocess=False,
    )

    return 0


if __name__ == "__main__":
    sys.exit(main())
