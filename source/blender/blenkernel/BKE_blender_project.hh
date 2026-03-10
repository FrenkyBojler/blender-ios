/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <optional>

#include "BLI_string_ref.hh"

namespace blender::bke {

/**
 * A Blender project.
 *
 * There is at most one active project at a time in Blender.
 */
class BlenderProject {
  /* Whether the project has been modified since the last time it was saved. */

  /* The name and root path should never be empty. */
  std::string name_;
  std::string root_path_;

 public:
  /* Whether the project has unsaved changes. */
  bool is_dirty = false;

  /**
   * Set the project's name.
   *
   * If `name` is empty (which is invalid), the project's name remains as-is and
   * false is returned.  Otherwise the name is set and true is returned.
   */
  bool set_name(StringRef name);

  /**
   * Set the project's root path.
   *
   * If `root_path` is empty (which is invalid), the project's root path remains
   * as-is and false is returned.  Otherwise the name is set and true is
   * returned.
   */
  bool set_root_path(StringRef root_path);

  StringRefNull get_name() const;
  StringRefNull get_root_path() const;
};

}  // namespace blender::bke

/**
 * Fetch the current Blender Project, if any.
 *
 * Returns nullptr if there is no project.
 */
blender::bke::BlenderProject *BKE_blender_project();

/**
 * Initialize a new Blender Project.
 *
 * If either `name` or `root_path` are empty (which is invalid), the current
 * project (if any) will remain as-is and false is returned.  Otherwise the
 * existing project (if any) is cleared, the project is initialized with the
 * given values, and true is returned.
 */
bool BKE_blender_project_init(blender::StringRef name, blender::StringRef root_path);

/**
 * Clears and unloads the current project, if any.
 */
void BKE_blender_project_clear();
