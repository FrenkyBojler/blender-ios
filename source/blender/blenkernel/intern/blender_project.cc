/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <mutex>
#include <shared_mutex>

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

  this->is_dirty = true;

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

    this->is_dirty = true;
  }

  return index;
}

void BlenderProject::move_variable(int from_index, int to_index)
{
  BLI_assert(from_index < this->variables.size());
  BLI_assert(to_index < this->variables.size());

  if (from_index < to_index) {
    std::rotate(this->variables.data() + from_index,
                this->variables.data() + from_index + 1,
                this->variables.data() + to_index + 1);
  }
  else if (from_index > to_index) {
    std::rotate(this->variables.data() + to_index,
                this->variables.data() + from_index,
                this->variables.data() + from_index + 1);
  }

  this->is_dirty = true;
}

}  // namespace bke

static std::optional<bke::BlenderProject> &get_project()
{
  static std::optional<bke::BlenderProject> project;

  return project;
}

static std::shared_mutex &get_project_mutex()
{
  static std::shared_mutex project_mutex;

  return project_mutex;
}

bke::BlenderProject *BKE_blender_project_get(const Main *bmain)
{
  if (bmain == nullptr) {
    return nullptr;
  }

  std::optional<bke::BlenderProject> &project = get_project();
  if (!project.has_value()) {
    return nullptr;
  }

  return &*project;
}

void BKE_with_blender_project(const Main *bmain,
                              std::function<void(const bke::BlenderProject *)> lambda)
{
  std::shared_lock<std::shared_mutex> lock(get_project_mutex());
  const bke::BlenderProject *project = BKE_blender_project_get(bmain);

  lambda(project);
}

void BKE_with_blender_project_write(const Main *bmain,
                                    std::function<void(bke::BlenderProject *)> lambda)
{
  std::unique_lock<std::shared_mutex> lock(get_project_mutex());
  bke::BlenderProject *project = BKE_blender_project_get(bmain);

  lambda(project);
}

bool BKE_blender_project_init(blender::StringRef name, blender::StringRef root_path)
{
  if (name.is_empty() || root_path.is_empty()) {
    return false;
  }

  BKE_blender_project_clear();

  std::unique_lock<std::shared_mutex> lock(get_project_mutex());
  std::optional<bke::BlenderProject> &project = get_project();

  project = blender::bke::BlenderProject();

  project->set_name(name);
  project->set_root_path(root_path);

  return true;
}

void BKE_blender_project_clear()
{
  /* At the moment this function is quite anemic, and doesn't really justify
   * being a separate function. However, as future milestones like
   * project-specific addons and asset libraries are added, this will collect in
   * one place the code for ensuring those things are properly unloaded when the
   * active project is cleared. */

  std::unique_lock<std::shared_mutex> lock(get_project_mutex());
  std::optional<bke::BlenderProject> &project = get_project();

  if (project.has_value()) {
    return;
  }

  project = std::nullopt;
}

}  // namespace blender
