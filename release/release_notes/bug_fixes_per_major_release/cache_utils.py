# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

def load_cached_commits(list_of_commits):
    import json

    from parameters import use_caching
    from shared_variables import path_to_cached_commits

    if use_caching and path_to_cached_commits.exists():
        with open(str(path_to_cached_commits), 'r', encoding='utf-8') as file:
            cached_data = json.load(file)
        for commit in list_of_commits:
            if commit.hash in cached_data:
                commit.read_from_cache(cached_data[commit.hash])


def cache_commits(list_of_commits):
    import json

    from parameters import use_caching
    from shared_variables import NEEDS_MANUAL_SORTING, path_to_cached_commits

    # Cache information for commits that have been sorted.
    # Commits that still need sorting are not cached.
    # This is done so if a user is repeatably running this script so they can sort
    # the "needs sorting" section, they don't have to wait for information requests to Gitea
    # on commits that are already sorted (and they're not interested in).

    if use_caching:
        data_to_cache = {}
        for commit in list_of_commits:
            if (commit.classification != NEEDS_MANUAL_SORTING) and not (commit.has_been_overwritten):
                commit_hash, data = commit.prepare_for_cache()
                data_to_cache[commit_hash] = data

        with open(str(path_to_cached_commits), 'w', encoding='utf-8') as file:
            json.dump(data_to_cache, file, indent=4)
