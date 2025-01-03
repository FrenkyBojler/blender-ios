# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

def get_version_numbers(broken_lines: str, working_lines: str) -> tuple[list[str], list[str]]:
    from parameters import list_of_official_blender_versions

    def extract_numbers(string: str) -> list[str]:
        import re
        return re.findall(r"(\d+\.\d+)", string)

    # Extracts all version numbers from the broken and working fields (Sometimes including weird version numbers like dates).
    temp_broken_versions = extract_numbers(broken_lines)
    temp_working_versions = extract_numbers(working_lines)

    broken_versions = []
    working_versions = []

    for version_number in temp_broken_versions:
        # Filter out any numbers picked up in the previous step that aren't official Blender version numbers.
        if version_number in list_of_official_blender_versions:
            broken_versions.append(version_number)
    for version_number in temp_working_versions:
        # Filter out any numbers picked up in the previous step that aren't official Blender version numbers.
        if version_number in list_of_official_blender_versions:
            working_versions.append(version_number)

    return broken_versions, working_versions


def version_extraction(report_body: str) -> tuple[list[str], list[str]]:
    broken_lines = ''
    working_lines = ''
    for line in report_body.splitlines():
        lower_line = line.lower()
        example_in_line = 'example' in lower_line
        if lower_line.startswith('brok') and not example_in_line:
            # Use "brok" to be able to detect different variations of "broken".
            broken_lines += f'{line}\n'
        if lower_line.startswith('work'):
            # Use "work" to be able to detect both "worked" and "working".
            if not example_in_line:
                working_lines += f'{line}\n'

    return get_version_numbers(broken_lines, working_lines)


def compare_versions(comparing_version: str, reference_version: str) -> str:
    from shared_variables import OLDER_VERION, NEWER_VERION, SAME_VERION

    # Compare two versions of Blender and return how they compare relative to each other.

    comp_version = comparing_version.split(".")
    ref_version = reference_version.split(".")
    comparing_major = int(comp_version[0])
    comparing_minor = int(comp_version[1])
    reference_major = int(ref_version[0])
    reference_minor = int(ref_version[1])

    if comparing_major < reference_major:
        return OLDER_VERION

    if comparing_major == reference_major:
        # The major version matches, so we must compare based on the minor version number.
        if comparing_minor < reference_minor:
            return OLDER_VERION
        if comparing_minor == reference_minor:
            return SAME_VERION

    return NEWER_VERION


# ----------


def classify_based_on_report(report_body: str) -> str:
    from parameters import current_version_number, previous_version_numer
    from shared_variables import FIXED_OLD_ISSUE, FIXED_NEW_ISSUE, NEEDS_MANUAL_SORTING, OLDER_VERION, NEWER_VERION, SAME_VERION

    # Get a list of broken and working versions of Blender according to the report that was fixed.
    broken_versions, working_versions = version_extraction(report_body)

    broken_is_current_or_newer = False

    for broken_version in broken_versions:
        relative_version = compare_versions(broken_version, current_version_number)
        if relative_version == OLDER_VERION:
            # Broken version is older than current release. So the issue is from a older version.
            return FIXED_OLD_ISSUE
        if relative_version in (SAME_VERION, NEWER_VERION):
            broken_is_current_or_newer = True

    for working_version in working_versions:
        relative_version = compare_versions(working_version, current_version_number)
        if relative_version in (SAME_VERION, NEWER_VERION):
            # Working version is current version or newer. So the issue was introduced in this version.
            return FIXED_NEW_ISSUE

    if broken_is_current_or_newer and (previous_version_numer in working_versions):
        # Issue is in current release, but wasn't in previous release. So it must of been introduced in the current release.
        return FIXED_NEW_ISSUE

    return NEEDS_MANUAL_SORTING
