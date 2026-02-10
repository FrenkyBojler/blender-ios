/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "DNA_userdef_types.h"

#include "BKE_blender_project.hh"

#include "BLI_string_ref.hh"

namespace blender::bke {

bool BlenderProjectData::set_name(StringRef name)
{
  if (name.is_empty()) {
    return false;
  }

  this->name_ = name;
  return true;
}

bool BlenderProjectData::set_root_path(StringRef root_path)
{
  if (root_path.is_empty()) {
    return false;
  }

  this->root_path_ = root_path;
  return true;
}

StringRefNull BlenderProjectData::get_name() const
{
  return StringRefNull(this->name_);
}

StringRefNull BlenderProjectData::get_root_path() const
{
  return StringRefNull(this->root_path_);
}

ProjectVariable *BlenderProjectData::new_variable()
{
  ProjectVariable *var = MEM_new<ProjectVariable>(__func__);
  this->variables.append(var);
  return var;
}

bool BlenderProjectData::remove_variable(ProjectVariable *var)
{
  const int index = this->variables.first_index_of_try(var);
  if (index == -1) {
    return false;
  }

  this->variables.remove(index);
  MEM_delete(var);

  return true;
}

BlenderProjectData::~BlenderProjectData()
{
  for (ProjectVariable *var : this->variables) {
    MEM_delete(var);
  }
}

bool BlenderProject::init(blender::StringRef name, blender::StringRef root_path)
{
  if (name.is_empty() || root_path.is_empty()) {
    return false;
  }

  this->clear();
  this->data = blender::bke::BlenderProjectData();

  this->data->set_name(name);
  this->data->set_root_path(root_path);

  /* Initializing the in-memory project does not save to disk, so it's dirty by
   * default. */
  this->is_dirty = true;

  return true;
}

void BlenderProject::clear()
{
  this->data = std::nullopt;
  this->is_dirty = false;
}

}  // namespace blender::bke

blender::bke::BlenderProject &BKE_blender_project()
{
  /* "Construct on first use" idiom. */
  static blender::bke::BlenderProject global_blender_project_;
  return global_blender_project_;
}
