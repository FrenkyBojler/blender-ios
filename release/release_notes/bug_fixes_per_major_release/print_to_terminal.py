# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

def prepare_for_print(list_of_commits):
    from shared_variables import FIXED_OLD_ISSUE, NEEDS_MANUAL_SORTING, REVERT, FIXED_PR, FIXED_NEW_ISSUE

    # This function takes in a list of commits, and sorts them based on their classification and module.

    valid_classifications = [FIXED_OLD_ISSUE, NEEDS_MANUAL_SORTING, REVERT, FIXED_PR, FIXED_NEW_ISSUE]
    dict_of_sorted_commits = {}
    for item in valid_classifications:
        dict_of_sorted_commits[item] = {}

    for commit in list_of_commits:
        commit_classification = commit.classification
        if commit_classification in valid_classifications:
            commit_module = commit.module
            try:
                # Try to append to a list. If it fails (The list doesn't exist), create the list.
                dict_of_sorted_commits[commit_classification][commit_module].append(commit)
            except:
                dict_of_sorted_commits[commit_classification][commit_module] = [commit]

    for item in valid_classifications:
        # Sort modules alphabetically
        dict_of_sorted_commits[item] = dict(sorted(dict_of_sorted_commits[item].items()))

    return dict_of_sorted_commits


def print_list_of_commits(title, list_of_commits):
    from shared_variables import UNKNOWN

    commits_message = ""
    number_of_commits = 0
    unknown_module_commit_message = ""
    for module in list_of_commits:
        number_of_commits += len(list_of_commits[module])
        module_label = f"\n## {module}\n"
        module_is_unknown = (module == UNKNOWN)
        if module_is_unknown:
            unknown_module_commit_message += module_label
        else:
            commits_message += module_label

        for commit in list_of_commits[module]:
            printed_line = commit.generate_release_note_ready_string()

            if module_is_unknown:
                unknown_module_commit_message += printed_line
            else:
                commits_message += printed_line

    if number_of_commits != 0:
        print(f"{title} {number_of_commits}")
        print(commits_message)
        print(unknown_module_commit_message)
        print("\n\n\n")


def print_release_notes(list_of_commits):
    from shared_variables import FIXED_OLD_ISSUE, REVERT, NEEDS_MANUAL_SORTING, FIXED_PR, FIXED_NEW_ISSUE
    dict_of_sorted_commits = prepare_for_print(list_of_commits)

    print_list_of_commits("Commits that fixed old issues:", dict_of_sorted_commits[FIXED_OLD_ISSUE])
    
    print_list_of_commits("Revert commits:", dict_of_sorted_commits[REVERT])

    print_list_of_commits("Commits that need manual sorting:", dict_of_sorted_commits[NEEDS_MANUAL_SORTING])

    print_list_of_commits("Commits that need a override (launch this script with -o) as they claim to fix a PR:", dict_of_sorted_commits[FIXED_PR])

    # Currently disabled as this information isn't particularly useful.
    # print_list_of_commits(dict_of_sorted_commits[FIXED_NEW_ISSUE])

    print("""What to do with this output:
    - Go through every commit in the "Commits that need manual sorting" section and:
      - Find the corrisponding issue that was fixed (it will be in the commit message)
      - Update the "Broken" and/or "Working" fields of the report with relevant information so this script can sort it.
        - Add a module label if it's missing one.
      - Rerun this script.
    - Repeat the previous steps until there are no commits that need manual sorting.
    - This should be done by the triaging module through out the release cycle, so the list should be quite small.

    - Go through the "Revert commits" section and if needed, find the commit they reverted and remove them from the list of "Commits that fixed old issues" (This can be done manually or with the overrides feature).
    - Double check if there are any obvious commits in the "Commits that fixed old issues" section that shouldn't be there and remove them (E.g. A fix for a feature that has been in development over a few releases, but was only enabled in this release).
    - Add the output of the "Commits that fixed old issues" section to the release notes: https://projects.blender.org/blender/blender-developer-docs/src/branch/main/docs/release_notes
      Here is the release notes for a previous release for reference: https://projects.blender.org/blender/blender-developer-docs/src/branch/main/docs/release_notes/4.3/bugfixes.md""")
