/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <optional>

#include "BLI_string_ref.hh"

namespace blender {

namespace bke {

/**
 * A Blender project.
 *
 * There is at most one active project at a time in Blender.
 */
class BlenderProject {
  /** The project name. Should never be empty. */
  std::string name_;

  /**
   * The project root path. Should never be empty.
   *
   * This should generally be a directory that exists, is accessible, and
   * contains a ".blender_project" directory with the project's config in it.
   * This is not, however, guaranteed because via Python a project can be
   * initialized with an arbitrary path.
   */
  std::string root_path_;

 public:
  /** Whether the project has unsaved changes. */
  bool is_dirty = false;

  /**
   * Set the project's name.
   *
   * Also marks the project as dirty.
   *
   * The passed `name` should never be empty (which is invalid).
   */
  void set_name(StringRef name);

  /**
   * Set the project's root path.
   *
   * Also marks the project as dirty.
   *
   * The passed `root_path` should never be empty (which is invalid).
   */
  void set_root_path(StringRef root_path);

  StringRefNull get_name() const;
  StringRefNull get_root_path() const;
};

}  // namespace bke

/**
 * Initialize a new active Blender Project.
 *
 * If either `name` or `root_path` are empty (which is invalid), the current
 * project (if any) will remain as-is and false is returned.  Otherwise the
 * existing project (if any) is cleared, the project is initialized with the
 * given values, and true is returned.
 *
 * NOTE: the active Blender Project (which this operates on) lives in the global
 * Main (a.k.a. `G_MAIN`).
 */
bool BKE_blender_project_init(blender::StringRef name, blender::StringRef root_path);

/**
 * Clears and unloads the current active project, if any.
 *
 * NOTE: the active Blender Project (which this operates on) lives in the global
 * Main (a.k.a. `G_MAIN`).
 */
void BKE_blender_project_clear();

}  // namespace blender
