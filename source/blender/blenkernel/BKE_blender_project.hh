/* SPDX-FileCopyrightText: 2025 Blender Authors
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

namespace blender::bke {

enum class ProjectVarType {
  INTEGER = 0,
  FLOAT = 1,
  STRING = 2,
  FILEPATH = 3,
};

struct ProjectVariable {
  std::string name;
  ProjectVarType type;

  /* For INTEGER type. */
  int32_t value_int;

  /* For FLOAT type. */
  float value_float;

  /* For STRING and FILEPATH types. */
  std::string value_string;
};

/**
 * The actual data of a project.
 *
 * Effectively, this is the actual project, and `BlenderProject` below is a
 * container that allows this to either exist or not depending on whether a
 * project is loaded or not.
 */
class BlenderProjectData {
 public:
  /* The name and root path should never be empty. */
  std::string name_;
  std::string root_path_;

  Vector<std::unique_ptr<ProjectVariable>> variables;
  int active_variable = 0;

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

/**
 * Container for `BlenderProjectData` that always exists.
 *
 * Also contains metadata about the state of project data, such as whether it's
 * dirty or not.
 */
class BlenderProject {
 public:
  /* Actual project data. When this is null, it means there is currently no
   * project. */
  std::optional<BlenderProjectData> data = std::nullopt;

  /* Whether the project has unsaved changes. */
  bool is_dirty = false;

  /**
   * Initialize a new Blender Project.
   *
   * If either `name` or `root_path` are empty (which is invalid), the current
   * project (if any) will remain as-is and false is returned.  Otherwise the
   * existing project (if any) is cleared, the project is initialized with the
   * given values, and true is returned.
   */
  bool init(blender::StringRef name, blender::StringRef root_path);

  /**
   * Clear the current Blender Project.
   */
  void clear();
};

}  // namespace blender::bke

/**
 * Fetch the current Blender Project.
 */
blender::bke::BlenderProject &BKE_blender_project();
