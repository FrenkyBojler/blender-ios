/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "DNA_userdef_types.h"

#include "BKE_blender_project.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"

#include "BLI_string_ref.hh"

namespace blender {

namespace bke {

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

ProjectVariable *BlenderProject::new_variable()
{
  this->variables.append(std::make_unique<ProjectVariable>());
  return this->variables.last().get();
}

bool BlenderProject::remove_variable(ProjectVariable *var)
{
  int index = -1;
  for (int i = 0; i < this->variables.size(); i++) {
    if (this->variables[i].get() == var) {
      index = i;
      break;
    }
  }

  if (index == -1) {
    return false;
  }

  this->variables.remove(index);

  return true;
}

}  // namespace bke

bool BKE_blender_project_init(blender::StringRef name, blender::StringRef root_path)
{
  if (name.is_empty() || root_path.is_empty()) {
    return false;
  }

  BKE_blender_project_clear();

  G_MAIN->project = blender::bke::BlenderProject();

  G_MAIN->project->set_name(name);
  G_MAIN->project->set_root_path(root_path);

  /* Initializing the in-memory project does not save to disk, so it's dirty by
   * default. */
  G_MAIN->project->is_dirty = true;

  return true;
}

/* At the moment this is quite anemic, and doesn't really justify being a
 * separate function. However, as future milestones like project-specific addons
 * and asset libraries are added, this will collect in one place the code for
 * ensuring those things are properly unloaded when the active project is
 * cleared. */
void BKE_blender_project_clear()
{
  if (!G_MAIN->project.has_value()) {
    return;
  }

  G_MAIN->project = std::nullopt;
}

}  // namespace blender
