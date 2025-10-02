/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_string_ref.hh"

namespace blender::file_watcher {

void add_file(StringRef filepath);
void remove_file(StringRef filepath);
bool poll();

}  // namespace blender::file_watcher
