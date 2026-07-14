/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <mutex>
#include <shared_mutex>

#include "RNA_types.hh"

#include "BKE_blender_project.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"

#include "BLI_function_ref.hh"
#include "BLI_string.hh"
#include "BLI_string_ref.hh"

namespace blender {

/**
 * Get a reference to the global Blender project.
 *
 * As a general rule, the project's mutex should be held while accessing this to
 * prevent data races. The public APIs `BKE_blender_project_read_callback()` and
 * `BKE_blender_project_write_callback()` enforce this (if not abused) and should be
 * used where possible.
 *
 * \see get_project_mutex()
 *
 * \see BKE_blender_project_read_callback()
 *
 * \see BKE_blender_project_write_callback()
 */
static std::optional<bke::BlenderProject> &get_project()
{
  /* Construct on First Use idiom. */
  static std::optional<bke::BlenderProject> project;

  return project;
}

/**
 * Get a reference to the global Blender project's mutex.
 *
 * \see get_project()
 */
static std::shared_mutex &get_project_mutex()
{
  /* Construct on First Use idiom. */
  static std::shared_mutex project_mutex;

  return project_mutex;
}

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

IDProperty *BlenderProject::new_variable(StringRef name, eIDPropertyType type)
{
  std::unique_ptr<IDProperty, idprop::IDPropertyDeleter> prop;
  switch (type) {
    case eIDPropertyType::IDP_INT: {
      prop = idprop::create(name, int32_t(0), eIDPropertyFlag(0));
      break;
    }

    case eIDPropertyType::IDP_FLOAT: {
      prop = idprop::create(name, 0.0f, eIDPropertyFlag(0));
      break;
    }

    case eIDPropertyType::IDP_STRING: {
      prop = idprop::create(name, "", eIDPropertyFlag(0));
      break;
    }

    default:
      /* Other IDProperty types not yet supported by project variables. */
      BLI_assert_unreachable();
      return nullptr;
  }

  IDP_ui_data_ensure(prop.get());
  prop->ui_data->description = BLI_strdup("");

  this->variables.append(std::move(prop));

  this->is_dirty = true;

  return this->variables.last().get();
}

int BlenderProject::remove_variable(IDProperty *var)
{
  for (int i : this->variables.index_range()) {
    if (this->variables[i].get() == var) {
      this->variables.remove(i);
      this->is_dirty = true;
      return i;
    }
  }

  return -1;
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

void with_blender_project_read_lock(FunctionRef<void()> lambda)
{
  std::shared_lock<std::shared_mutex> lock(get_project_mutex());
  lambda();
}

void with_blender_project_write_lock(FunctionRef<void()> lambda)
{
  std::unique_lock<std::shared_mutex> lock(get_project_mutex());
  lambda();
}

bool is_valid_project_variable_name(StringRef name)
{
  /* Shouldn't be empty. */
  if (name.is_empty()) {
    return false;
  }

  /* Shouldn't start with a numerical digit. */
  if (name[0] >= '0' && name[0] <= '9') {
    return false;
  }

  /* All characters should be alphanumeric or underscore. */
  for (char c : name) {
    const bool is_valid_identifier_char = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                          (c >= '0' && c <= '9') || c == '_';
    if (!is_valid_identifier_char) {
      return false;
    }
  }

  return true;
}

}  // namespace bke

bke::BlenderProject *BKE_blender_project_get(const Main *bmain)
{
  if (bmain == nullptr || !bmain->is_part_of_project) {
    return nullptr;
  }

  std::optional<bke::BlenderProject> &project = get_project();
  if (!project.has_value()) {
    return nullptr;
  }

  return &*project;
}

bool BKE_blender_project_init(blender::StringRef name, blender::StringRef root_path)
{
  if (name.is_empty() || root_path.is_empty()) {
    return false;
  }

  BKE_blender_project_clear();

  bke::with_blender_project_write_lock([&] {
    std::optional<bke::BlenderProject> &project = get_project();

    project = blender::bke::BlenderProject();

    project->set_name(name);
    project->set_root_path(root_path);
  });

  return true;
}

void BKE_blender_project_clear()
{
  /* At the moment this function is quite anemic, and doesn't really justify
   * being a separate function. However, as future milestones like
   * project-specific addons and asset libraries are added, this will collect in
   * one place the code for ensuring those things are properly unloaded when the
   * active project is cleared. */

  bke::with_blender_project_write_lock([&] {
    std::optional<bke::BlenderProject> &project = get_project();

    project = std::nullopt;
  });
}

}  // namespace blender
