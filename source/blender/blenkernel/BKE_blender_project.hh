/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <concepts>
#include <optional>

#include "BLI_function_ref.hh"
#include "BLI_string_ref.hh"

namespace blender {

struct Main;

namespace bke {

/**
 * A Blender project.
 *
 * There is at most one active project at a time in Blender.
 */
class BlenderProject {
  /** The project name. Should never be empty. */
  std::string name_;

  /**
   * The project root path. Should never be empty.
   *
   * This should generally be a directory that exists, is accessible, and
   * contains a ".blender_project" directory with the project's config in it.
   * This is not, however, guaranteed because via Python a project can be
   * initialized with an arbitrary path.
   */
  std::string root_path_;

 public:
  /**
   * Whether the project has unsaved changes.
   *
   * Default initializes to `true` because a freshly constructed
   * `BlenderProject` is unsaved by definition.
   */
  bool is_dirty = true;

  /**
   * Set the project's name.
   *
   * Also marks the project as dirty.
   *
   * The passed `name` should never be empty (which is invalid).
   */
  void set_name(StringRef name);

  /**
   * Set the project's root path.
   *
   * Also marks the project as dirty.
   *
   * The passed `root_path` should never be empty (which is invalid).
   */
  void set_root_path(StringRef root_path);

  StringRefNull get_name() const;
  StringRefNull get_root_path() const;
};

/**
 * Underlying impl for `BKE_with_blender_project()`.
 *
 * Please see the documentation for and use `BKE_with_blender_project()` instead
 * of this.
 */
void with_blender_project_for_read_impl(const Main *bmain,
                                        FunctionRef<void(const bke::BlenderProject *)> lambda);

/**
 * Underlying impl for `BKE_with_blender_project_write()`.
 *
 * Please see the documentation for and use `BKE_with_blender_project_write()`
 * instead of this.
 */
void with_blender_project_for_write_impl(const Main *bmain,
                                         FunctionRef<void(bke::BlenderProject *)> lambda);

}  // namespace bke

/**
 * Fetch the current active Blender Project, if any.
 *
 * WARNING: this fetches the project without any synchronization for
 * multi-threading, so it is your responsibility to ensure thread safety. Prefer
 * using `BKE_with_blender_project()` and `BKE_with_blender_project_write()`,
 * which handle thread synchronization for you.
 *
 * \param bmain: The `Main` to return the active project for. At the moment,
 * there is just one global project. However, some temporary `Main`s should be
 * treated as not ever being in a project, in which case this will return
 * nullptr.
 *
 * \returns Either the current active project, or nullptr if there is no active
 * project or if the passed bmain is considered projectless.
 *
 * \see BKE_with_blender_project()
 *
 * \see BKE_with_blender_project_write()
 */
bke::BlenderProject *BKE_blender_project_get(const Main *bmain);

/**
 * Run the given lambda with read-only access to the active Blender Project, if
 * any.
 *
 * This follows the same project-fetching semantics as
 * `BKE_blender_project_get()`, but ensures thread safety by holding a shared
 * mutex lock while the lambda is run and only providing the fetched project (if
 * any) to the lambda.
 *
 * The lambda may return a value, in which case this function passes that value
 * through as its own return value.
 *
 * NOTE: the lambda is run even if there is no project, in which case the lambda
 * receives a nullptr.
 *
 * \see BKE_blender_project_get()
 *
 * \see BKE_with_blender_project_write()
 */
template<std::invocable<const bke::BlenderProject *> Fn>
inline auto BKE_with_blender_project(const Main *bmain, Fn lambda)
{
  using T = std::invoke_result_t<Fn, const bke::BlenderProject *>;
  if constexpr (std::is_void_v<T>) {
    bke::with_blender_project_for_read_impl(bmain, lambda);
  }
  else {
    std::optional<T> result;
    bke::with_blender_project_for_read_impl(
        bmain, [&](const bke::BlenderProject *project) { result = lambda(project); });
    BLI_assert(result.has_value());
    return std::move(*result);
  }
}

/**
 * Run the given lambda with write access to the active Blender Project, if any.
 *
 * Same as `BKE_with_blender_project()`, except that it takes an exclusive mutex
 * lock to provide write access to the project.
 *
 * If you only need to read from the project, use `BKE_with_blender_project()`
 * instead of this to reduce thread contention.
 *
 * \see BKE_blender_project_get()
 *
 * \see BKE_with_blender_project()
 */
template<std::invocable<bke::BlenderProject *> Fn>
inline auto BKE_with_blender_project_write(const Main *bmain, Fn lambda)
{
  using T = std::invoke_result_t<Fn, bke::BlenderProject *>;
  if constexpr (std::is_void_v<T>) {
    bke::with_blender_project_for_write_impl(bmain, lambda);
  }
  else {
    std::optional<T> result;
    bke::with_blender_project_for_write_impl(
        bmain, [&](bke::BlenderProject *project) { result = lambda(project); });
    BLI_assert(result.has_value());
    return std::move(*result);
  }
}

/**
 * Initialize a new active Blender Project.
 *
 * If either `name` or `root_path` are empty (which is invalid), the current
 * project (if any) will remain as-is and false is returned.  Otherwise the
 * existing project (if any) is cleared, the project is initialized with the
 * given values, and true is returned.
 */
bool BKE_blender_project_init(blender::StringRef name, blender::StringRef root_path);

/**
 * Clears and unloads the current active project, if any.
 */
void BKE_blender_project_clear();

}  // namespace blender
