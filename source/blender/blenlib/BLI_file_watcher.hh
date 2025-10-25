/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender::file_watcher {

void add_file(StringRef filepath);
void remove_file(StringRef filepath);
Vector<std::string> poll_changed_files();

}  // namespace blender::file_watcher
