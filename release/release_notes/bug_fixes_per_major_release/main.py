# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from argparse import ArgumentParser

def argparse_create() -> ArgumentParser:
    parser = ArgumentParser()
    parser.add_argument("-o", "--override", action="store_true", help="Create a override for a commit")

    return parser

if __name__ == "__main__":
    from commit_info import get_fix_commits
    from sort_commits import classify_commits
    from print_to_terminal import print_release_notes
    from overrides import overrides_apply, create_override
    from cache_utils import cached_commits_store, cached_commits_load

    args = argparse_create().parse_args()
    if args.override:
        create_override()
        quit()

    list_of_commits = get_fix_commits()

    cached_commits_load(list_of_commits)

    overrides_apply(list_of_commits)

    classify_commits(list_of_commits)

    cached_commits_store(list_of_commits)

    print_release_notes(list_of_commits)
