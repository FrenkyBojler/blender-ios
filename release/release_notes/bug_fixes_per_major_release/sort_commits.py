# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

def get_backported_commits(issue_number):
    # Adapted from https://projects.blender.org/blender/blender/src/branch/main/release/lts/lts_issue.py
    import re
    from gitea_utils import url_json_get

    base_url = "https://projects.blender.org/api/v1/repos"
    issues_url = base_url + "/blender/blender/issues/"

    response = url_json_get(issues_url + issue_number)
    description = response["body"]

    lines = description.split("\n")
    current_version = None

    dict_of_backports = {}

    blender_version_start = "## Blender "
    for line in lines:
        if line.startswith(blender_version_start):
            current_version = line.strip(blender_version_start)
        if current_version == None:
            # We haven't got a Blender version yet.
            continue
        if not line.strip():
            continue
        if not ("|" in line):
            # Not part of the backports table.
            continue
        if line.startswith("| **Report**"):
            continue
        if line.find("| -- |") != -1:
            continue

        items = line.split("|")
        commit_string = items[2].strip()
        commit_string = commit_string.split(",")[0]
        commit_string = commit_string.split("]")[0]
        commit_string = commit_string.replace("[", "")

        pattern = r"blender/blender@([a-zA-Z0-9]+)"
        matches = re.findall(pattern, commit_string)
        if len(matches) > 0:
            try:
                dict_of_backports[current_version] += matches
            except:
                dict_of_backports[current_version] = matches

    return dict_of_backports


def get_backports():
    from parameters import backport_tasks

    dict_of_backports = {}
    for item in backport_tasks:
        dict_of_backports.update(get_backported_commits(item))

    return dict_of_backports

# ----------

def classify_commits(list_of_commits):
    from time import time
    from gitea_utils import crawl_delay

    number_of_commits = len(list_of_commits)

    print("Identifying if fixes are for a bug introduced in this release, or if the bug was there in a previous release.")
    print("This requires querying information from Gitea, and may take a while.\n")

    dict_of_backports = get_backports()

    i = 0
    start_time = time()
    for commit in list_of_commits:
        # Simple progress bar.
        i += 1
        print(f"{i}/{number_of_commits} - Estimated time remaining: {(((time() - start_time) / i) * (number_of_commits - i))/60:.1f} minutes", end="\r", flush=True)

        commit.classify()
        commit.get_backports(dict_of_backports)

    # Print so we're away from the progress bar.
    print("\n\n\n")
