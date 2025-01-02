# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

class CommitInfo():
    def __init__(self, commit_line):
        split_message = commit_line.split()

        # Commit line is in the format:
        # COMMIT_HASH Title of commit
        self.hash = split_message[0]
        self.commit_title = " ".join(split_message[1:])

        self.set_defaults()

    def set_defaults(self):
        from shared_variables import UNKNOWN

        self.is_revert = 'revert' in self.commit_title.lower()

        self.fixed_reports = None
        self.check_full_commit_message_for_fixed_reports()

        # Setup some "useful" empty defaults.
        self.backport_list = []
        self.module = UNKNOWN
        self.report_title = UNKNOWN
        self.classification = UNKNOWN

        # Varibales below this point should not be saved to the cache.
        self.needs_update = True
        self.has_been_overwritten = False

    def check_full_commit_message_for_fixed_reports(self):
        import re
        import subprocess

        command = ['git', 'show', '-s', '--format=%B', self.hash]
        command_output = subprocess.run(command, capture_output=True).stdout.decode('utf-8')
        
        # Find every instance of #NUMBER. These are the report that the commit claims to fix.
        match = re.findall(r'#(\d+)', command_output)
        if match:
            self.fixed_reports = match

    def get_backports(self, dict_of_backports):
        from shared_variables import FIXED_OLD_ISSUE

        # Figures out if the commit was backported, and to what verion(s).
        if self.needs_update:
            for version_number in dict_of_backports:
                for backported_commit in dict_of_backports[version_number]:
                    if self.hash.startswith(backported_commit):
                        self.backport_list.append(version_number)
                        break

        if len(self.backport_list) > 0:
            # If the fix was backported to a old release, then it fixed a old issue.
            self.classification = FIXED_OLD_ISSUE

    def override_report_info(self, new_classification, new_title, new_module):
        from shared_variables import FIXED_NEW_ISSUE, FIXED_OLD_ISSUE, NEEDS_MANUAL_SORTING, FIXED_PR, UNKNOWN

        if new_classification in (FIXED_NEW_ISSUE, FIXED_OLD_ISSUE):
            # Clear classifications are more important then any other. So always override in this case.
            self.classification = new_classification
            self.report_title = new_title
            self.module = new_module
            return

        if new_classification in (NEEDS_MANUAL_SORTING, FIXED_PR):
            if (self.classification == UNKNOWN) or ((new_classification == NEEDS_MANUAL_SORTING) and (self.classification == FIXED_PR)):
                # Only replace information if the previous classification was the default (UNKNOWN)
                # or the new classification is NEEDS_MANUAL_SORTING and the old one was FIXED_PR (NEEDS_MANUAL_SORTING is more useful than FIXED_PR).
                self.classification = new_classification
                self.report_title = new_title
                self.module = new_module
            return

    def get_module(self, labels):
        from shared_variables import UNKNOWN

        # Figures out what module the report that was fixed belongs too.
        for label in labels:
            if "module" in label['name'].lower():
                # Module labels are typically in the format Module/NAME.
                return " ".join(label['name'].split("/")[1:])

        return UNKNOWN

    def classify(self):
        from gitea_utils import url_json_get
        from classify_report import classify_based_on_report
        from shared_variables import FIXED_NEW_ISSUE, FIXED_OLD_ISSUE, FIXED_PR, REVERT, UNKNOWN

        if not self.needs_update:
            # The data was loaded from cache, no need to reprocess it.
            return

        if self.is_revert:
            self.classification = REVERT
            # Give reverts commits their commit title so when it is printed to terminal, it has a useful name.
            self.report_title = self.commit_title
            return

        sorted_classes = (FIXED_NEW_ISSUE, FIXED_OLD_ISSUE)

        for report_number in self.fixed_reports:
            report_information = url_json_get(f"https://projects.blender.org/api/v1/repos/blender/blender/issues/{report_number}")

            report_title = report_information['title']
            module = self.get_module(report_information['labels'])

            if "pull" in report_information['html_url']:
                # The fixed issue turns out to be a pull request.
                # This was probably a typo, but note it down away so we can check and fix it.
                self.override_report_info(FIXED_PR, self.commit_title, UNKNOWN)
            else:
                classification = classify_based_on_report(report_information['body'])
                self.override_report_info(classification, report_title, module)

            if self.classification in sorted_classes:
                # The commit has been sorted. No need to process more reports.
                break

    def generate_release_note_ready_string(self):
        # Breakup report_title based on words, and remove `:` if it's at the end of the first word.
        # This is because the website the release notes are being posted to applies some undesirable
        # formatting to ` * Word:`.
        title = self.report_title
        title = title.split()
        title[0] = title[0].strip(":")

        # Capitalize the first letter of the issue title.
        title[0] = title[0][0].upper() + title[0][1:]

        title = " ".join(title)

        formatted_string = f" * {title} [[{self.hash[:11]}](https://projects.blender.org/blender/blender/commit/{self.hash})]"
        if len(self.backport_list) > 0:
            formatted_string += f" - Backported to {' & '.join(self.backport_list)}"
        formatted_string += "\n"

        return formatted_string

    def prepare_for_cache(self):
        return self.hash, {'is_revert': self.is_revert,
                           'fixed_reports': self.fixed_reports,
                           'backport_list': self.backport_list,
                           'module': self.module,
                           'report_title': self.report_title,
                           'classification': self.classification}

    def read_from_cache(self, cache_data):
        self.is_revert = cache_data['is_revert']
        self.fixed_reports = cache_data['fixed_reports']
        self.backport_list = cache_data['backport_list']
        self.module = cache_data['module']
        self.report_title = cache_data['report_title']
        self.classification = cache_data['classification']

        self.needs_update = False

    def read_from_override(self, override_data):
        self.set_defaults()
        self.fixed_reports = override_data

        self.has_been_overwritten = True


# ----------


def setup_commit_info(commit):
    commit_information = CommitInfo(commit)
    if commit_information.fixed_reports:
        return commit_information


def get_fix_commits():
    import subprocess
    from parameters import previous_release_tag, current_release_tag

    # --no-pager means it prints everything all at once rather than providing a interactive scrollable page.
    # --no-abbrev-commit tells git to always show the full commit hash.
    # -i tells grep to ignore case when searching through commits.
    # -P tells grep to use a specific type of regular expression.

    # This searches for Fix{anything}{one_or_more #}{number}.
    # .* = {anything}
    # #+ = {one_or_more #}
    # \d+ = {number}
    # This captures the common `Fix #123`, but also the less common `Fixes #123`, `Fix for #123`, and `Fix ##123`.
    command = ['git', '--no-pager', 'log', f'{previous_release_tag}..{current_release_tag}', '--oneline', '--no-abbrev-commit', '-P', '-i', '--grep', r'Fix.*#+\d+']

    git_log_output = subprocess.run(command, capture_output=True).stdout.decode('utf-8')
    git_log_output = git_log_output.splitlines()

    if True:
        # Although setup_commit_info is not compute intensive, it is time consuming due to hundreds of git log calls.
        # Multiprocessing can significantly reduce the time taken (E.g. 19s -> 4s on a 32 thread CPU for 4.3 release).

        import multiprocessing
        pool = multiprocessing.Pool()

        results = pool.map(setup_commit_info, git_log_output)
        list_of_commits = [result for result in results if result]
        pool.close()
        pool.join()
    else:
        # Original non-multiprocessing method.
        list_of_commits = []
        for commit in git_log_output:
            commit_information = CommitInfo(commit)
            if commit_information.fixed_reports:
                list_of_commits.append(commit_information)


    return list_of_commits
