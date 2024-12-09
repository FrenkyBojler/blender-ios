# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

def argparse_create():
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--override", action="store_true", help="Create a override for a commit")

    return parser

if __name__ == "__main__":
    from commit_info import get_fix_commits
    from sort_commits import classify_commits
    from print_to_terminal import print_release_notes
    from overrides import apply_overrides, create_override
    from cache_utils import cache_commits, load_cached_commits

    args = argparse_create().parse_args()
    if args.override:
        create_override()
        quit()

    list_of_commits = get_fix_commits()

    load_cached_commits(list_of_commits)

    apply_overrides(list_of_commits)

    classify_commits(list_of_commits)

    cache_commits(list_of_commits)

    print_release_notes(list_of_commits)
