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

struct Main;

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
  Vector<std::unique_ptr<ProjectVariable>> variables;
  int active_variable_index = 0;

  /**
   * Whether the project has unsaved changes.
   *
   * Default initializes to `true` because a freshly constructed
   * `BlenderProject` is unsaved by definition.
   */
  bool is_dirty = true;

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

  ProjectVariable *new_variable();

  /**
   * Remove the given variable.
   *
   * Returns the index that the removed variable had, or -1 if the variable
   * wasn't found.
   */
  int remove_variable(ProjectVariable *var);
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
 * WARNING: this should only ever be called with the global Main (a.k.a.
 * `G_MAIN`) passed as `bmain`.  Projects on Mains other than the global one,
 * and more generally more than one simultaneously active project, ARE NOT
 * CURRENTLY SUPPORTED and you are likely to break things if you naively try.
 */
bool BKE_blender_project_init(blender::StringRef name, blender::StringRef root_path, Main *bmain);

/**
 * Clears and unloads the current active project, if any.
 *
 * WARNING: this should only ever be called with the global Main (a.k.a.
 * `G_MAIN`) passed as `bmain`.  Projects on Mains other than the global one,
 * and more generally more than one simultaneously active project, ARE NOT
 * CURRENTLY SUPPORTED and you are likely to break things if you naively try.
 */
void BKE_blender_project_clear(Main *bmain);

}  // namespace blender
