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

  return true;
}

void BKE_blender_project_clear()
{
  /* At the moment this function is quite anemic, and doesn't really justify
   * being a separate function. However, as future milestones like
   * project-specific addons and asset libraries are added, this will collect in
   * one place the code for ensuring those things are properly unloaded when the
   * active project is cleared. */

  if (!G_MAIN->project.has_value()) {
    return;
  }

  G_MAIN->project = std::nullopt;
}

}  // namespace blender
