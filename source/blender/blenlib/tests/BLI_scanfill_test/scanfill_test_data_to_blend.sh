#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2024 Campbell Barton
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Create a blend file from JSON test data files.

BASE_DIR="$(dirname "$(realpath "$0")")"
BLENDER_BIN="${BLENDER_BIN:-blender}"

cd "$BASE_DIR" && \
env ASAN_OPTIONS=check_initialization_order=0:leak_check_at_exit=0 \
"$BLENDER_BIN" -q -b --factory-startup --python ./scanfill_test_data_to_blend.py
