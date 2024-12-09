# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Update these variables with relevant information for the release you're interested in.

# Add recent Blender versions to this list, including indevelopment versions.
# This list is used to identify if a version number found in a report is a valid version number.
# This is to help elimintate dates and other weird information people put in their reports in the format of a version number.
list_of_official_blender_versions = ['1.0', '1.60', '1.73', '1.80', '2.04', '2.26', '2.27', '2.27', '2.28', '2.28', '2.28', '2.30', '2.31', '2.31', '2.32', '2.33', '2.33', '2.34', '2.35', '2.36', '2.37', '2.37', '2.39', '2.40', '2.41', '2.42', '2.43', '2.44', '2.45', '2.46', '2.47', '2.48', '2.48', '2.49', '2.49', '2.49', '2.50', '2.53', '2.54', '2.55', '2.56', '2.56', '2.57', '2.58', '2.59', '2.60', '2.61', '2.62', '2.63', '2.64', '2.65', '2.66', '2.67', '2.68', '2.69', '2.70', '2.71', '2.72', '2.73', '2.74', '2.75', '2.76', '2.77', '2.78', '2.79', '2.80', '2.81', '2.82', '2.83', '2.90', '2.91', '2.92', '2.93', '3.0', '3.1', '3.2', '3.3', '3.4', '3.5', '3.6', '4.0', '4.1', '4.2', '4.3', '4.4']

# Release tags can be tags (like `v4.3.0`), commit hashes, or branches.
current_release_tag = 'main'
previous_release_tag = 'v4.3.0'

current_version_number = '4.4'
previous_version_numer = '4.3'

# The numbers of the active backport tracking tasks.
# The backport tasks can be found on the Blender milestones page: https://projects.blender.org/blender/blender/milestones
# Note: Should we add corrective releases tasks like the 4.3.1 task when processing 4.4 release notes?
backport_tasks = ['124452', '109399']

# Use caching to speed up the fetching of information for the reports that need manual sorting.
# Turn this off when generating the final release notes as the cache may be out of date.
use_caching = False
