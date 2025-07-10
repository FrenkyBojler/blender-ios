/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_blender_project.hh"

#include "BLI_string_ref.hh"

static std::optional<blender::bke::BlenderProject> global_blender_project_;

namespace blender::bke {

bool BlenderProject::set_name(StringRef name)
{
  if (name.is_empty()) {
    return false;
  }

  this->name_ = name;
  return true;
}

bool BlenderProject::set_root_path(StringRef root_path)
{
  if (root_path.is_empty()) {
    return false;
  }

  this->root_path_ = root_path;
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

bool BKE_blender_project_init(blender::StringRef name, blender::StringRef root_path)
{
  if (name.is_empty() || root_path.is_empty()) {
    return false;
  }

  BKE_blender_project_clear();
  global_blender_project_ = blender::bke::BlenderProject();

  global_blender_project_->set_name(name);
  global_blender_project_->set_root_path(root_path);

  return true;
}

void BKE_blender_project_clear()
{
  global_blender_project_ = std::nullopt;
}

std::optional<blender::bke::BlenderProject> &BKE_blender_project()
{
  return global_blender_project_;
}
