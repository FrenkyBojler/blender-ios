# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path

UNKNOWN = "UNKNOWN"

FIXED_NEW_ISSUE = "FIXED NEW"
NEEDS_MANUAL_SORTING = "MANUALLY SORT"
FIXED_OLD_ISSUE = "FIXED OLD"
FIXED_PR = "FIXED PR"
REVERT = "REVERT"

OLDER_VERION = "OLDER"
NEWER_VERION = "NEWER"
SAME_VERION = "SAME"

dir_of_script = Path(__file__).parent.resolve()
path_to_overrides = dir_of_script.joinpath('overrides.json')
path_to_cached_commits = dir_of_script.joinpath('cached_commits.json')
