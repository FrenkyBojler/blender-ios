# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from commit_info import CommitInfo

def overrides_load() -> dict[str, list[str]]:
    import json
    from shared_variables import path_to_overrides

    override_data = {}
    if path_to_overrides.exists():
        with open(str(path_to_overrides), 'r', encoding='utf-8') as file:
            override_data = json.load(file)

    return override_data


def overrides_store(override_data: dict[str, list[str]]) -> None:
    import json
    from shared_variables import path_to_overrides

    with open(str(path_to_overrides), 'w', encoding='utf-8') as file:
        json.dump(override_data, file, indent=4)


def overrides_apply(list_of_commits: list[CommitInfo]) -> None:
    override_data = overrides_load()
    if len(override_data) > 0:
        for commit in list_of_commits:
            if commit.hash in override_data:
                commit.read_from_override(override_data[commit.hash])


def create_override() -> None:
    commit_hash = input("Please input the full hash of the commit you want to override: ")
    issue_number = input("Please input the issue number you want to override it with: ")

    override_data = overrides_load()
    override_data[commit_hash] = [issue_number]

    overrides_store(override_data)
