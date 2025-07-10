/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <optional>

#include "BLI_string_ref.hh"

namespace blender::bke {

class BlenderProject {
  /* The name and root path should never be empty. */
  std::string name_;
  std::string root_path_;

 public:
  bool set_name(StringRef name);
  bool set_root_path(StringRef root_path);

  StringRefNull get_name() const;
  StringRefNull get_root_path() const;
};

}  // namespace blender::bke

/**
 * Initialize a new Blender Project.
 */
bool BKE_blender_project_init(blender::StringRef name, blender::StringRef root_path);

/**
 * Clear the current Blender Project.
 */
void BKE_blender_project_clear();

/**
 * Fetch the current Blender Project, if one exists.
 */
std::optional<blender::bke::BlenderProject> &BKE_blender_project();
