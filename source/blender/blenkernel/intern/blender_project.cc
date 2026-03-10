/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "DNA_userdef_types.h"

#include "BKE_blender_project.hh"

#include "BLI_string_ref.hh"

namespace blender::bke {

bool BlenderProject::set_name(StringRef name)
{
  if (name.is_empty()) {
    return false;
  }

  this->name_ = name;

  this->is_dirty = true;

  return true;
}

bool BlenderProject::set_root_path(StringRef root_path)
{
  if (root_path.is_empty()) {
    return false;
  }

  this->root_path_ = root_path;

  this->is_dirty = true;

  return true;
}

StringRefNull BlenderProject::get_name() const
{
  return StringRefNull(this->name_);
}

StringRefNull BlenderProject::get_root_path() const
{
  return StringRefNull(this->root_path_);
}

}  // namespace blender::bke

/**
 * Uses the Construct on First Use idiom for the global BlenderProject.
 */
static std::optional<blender::bke::BlenderProject> &get_global_blender_project()
{
  static std::optional<blender::bke::BlenderProject> blender_project;

  return blender_project;
}

/* Access the global project outside of this source file.
 *
 * We have this function rather than exposing `get_global_blender_project()`
 * directly to ensure that initialization and clearing of the project have to go
 * through `BKE_blender_project_init()` and `BKE_blender_project_clear()` below.
 *
 * That in turn allows us to enforce invariants about the project state, such as
 * project asset libraries being unloaded when the project is cleared. */
blender::bke::BlenderProject *BKE_blender_project()
{
  std::optional<blender::bke::BlenderProject> &blender_project = get_global_blender_project();
  if (!blender_project.has_value()) {
    return nullptr;
  }

  return &blender_project.value();
}

bool BKE_blender_project_init(blender::StringRef name, blender::StringRef root_path)
{
  if (name.is_empty() || root_path.is_empty()) {
    return false;
  }

  BKE_blender_project_clear();

  std::optional<blender::bke::BlenderProject> &blender_project = get_global_blender_project();

  blender_project = blender::bke::BlenderProject();

  blender_project->set_name(name);
  blender_project->set_root_path(root_path);

  /* Initializing the in-memory project does not save to disk, so it's dirty by
   * default. */
  blender_project->is_dirty = true;

  return true;
}

void BKE_blender_project_clear()
{
  std::optional<blender::bke::BlenderProject> &blender_project = get_global_blender_project();
  if (!blender_project.has_value()) {
    return;
  }

  blender_project = std::nullopt;
}
