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

void BlenderProject::set_name(StringRef name)
{
  BLI_assert(!name.is_empty());

  this->name_ = name;

  this->is_dirty = true;
}

void BlenderProject::set_root_path(StringRef root_path)
{
  BLI_assert(!root_path.is_empty());

  this->root_path_ = root_path;

  this->is_dirty = true;
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

int BlenderProject::remove_variable(ProjectVariable *var)
{
  int index = -1;
  for (int i = 0; i < this->variables.size(); i++) {
    if (this->variables[i].get() == var) {
      index = i;
      break;
    }
  }

  if (index != -1) {
    this->variables.remove(index);
  }

  return index;
}

}  // namespace bke

bool BKE_blender_project_init(blender::StringRef name, blender::StringRef root_path, Main *bmain)
{
  BLI_assert(bmain->is_global_main);
  if (!bmain->is_global_main) {
    return false;
  }

  if (name.is_empty() || root_path.is_empty()) {
    return false;
  }

  BKE_blender_project_clear(bmain);

  bmain->project = blender::bke::BlenderProject();

  bmain->project->set_name(name);
  bmain->project->set_root_path(root_path);

  return true;
}

void BKE_blender_project_clear(Main *bmain)
{
  /* At the moment this function is quite anemic, and doesn't really justify
   * being a separate function. However, as future milestones like
   * project-specific addons and asset libraries are added, this will collect in
   * one place the code for ensuring those things are properly unloaded when the
   * active project is cleared. */

  BLI_assert(bmain->is_global_main);
  if (!bmain->is_global_main) {
    return;
  }

  if (!bmain->project.has_value()) {
    return;
  }

  bmain->project = std::nullopt;
}

}  // namespace blender
