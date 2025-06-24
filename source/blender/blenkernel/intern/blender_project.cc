/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_blender_project.hh"

#include "BLI_string_ref.hh"

static blender::bke::BlenderProject global_blender_project_;

namespace blender::bke {

bool BlenderProject::is_initialized()
{
  return !this->get_name().is_empty() && !this->get_root_path().is_empty();
}

void BlenderProject::init(StringRef name, StringRef root_path)
{
  BLI_assert(!name.is_empty());
  BLI_assert(!root_path.is_empty());

  /* TODO: root path validation. */

  this->clear();

  this->name_ = name;
  this->root_path_ = root_path;
}

void BlenderProject::clear()
{
  this->name_.clear();
  this->root_path_.clear();
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

blender::bke::BlenderProject &BKE_blender_project()
{
  return global_blender_project_;
}
