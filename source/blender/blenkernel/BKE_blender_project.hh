/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender {

namespace bke {

enum class ProjectVarType {
  INTEGER = 0,
  FLOAT = 1,
  STRING = 2,
  FILEPATH = 3,
};

struct ProjectVariable {
  std::string name;
  std::string description;
  ProjectVarType type;

  /* For INTEGER type. */
  int32_t value_int;

  /* For FLOAT type. */
  float value_float;

  /* For STRING and FILEPATH types. */
  std::string value_string;
};

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
  Vector<std::unique_ptr<ProjectVariable>> variables;
  int active_variable = 0;

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

  ProjectVariable *new_variable();
  bool remove_variable(ProjectVariable *var);
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
