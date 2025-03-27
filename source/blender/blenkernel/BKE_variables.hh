/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 *
 * \brief Functions and classes for working with variables (Blender variables,
 * Project variables, etc.).
 */
#pragma once

#include <cmath>
#include <optional>

#include "BLI_map.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utils.hh"

#include "DNA_scene_types.h"

/* -------------------------------------------------------------------- */
/** \name Blender Variables
 * \{ */

/**
 * A store for the values of variables, addressed by variable name.
 *
 * Note that this is not intended to be a persistent store for variables, but
 * rather a transient one for collecting the values of variables that are
 * relevant/available in a given context. This is typically passed to functions
 * that use those variables for processing of some kind.
 *
 * There are currently three types of variables: string, integer, and float.
 * There can only be a single variable with a given name across all variable
 * types. For example, you can't have both a string *and* integer variable both
 * with the name "bob".
 */
class VariableMap {
  blender::Map<std::string, std::string> strings;
  blender::Map<std::string, int64_t> integers;
  blender::Map<std::string, double> floats;

 public:
  /**
   * Check if a variable of the given name exists.
   */
  bool contains(blender::StringRef name) const;

  /**
   * Remove the variable with the given name.
   *
   * \return True if the variable existed and was removed, false if it didn't
   * exist in the first place.
   */
  bool remove(blender::StringRef name);

  /**
   * Add a string variable with the given name and value.
   *
   * If there is already a variable with that name, regardless of type, the new
   * variable is *not* added (no overwriting).
   *
   * \return True if the variable was successfully added, false if there was
   * already a variable with that name.
   */
  bool add_string(blender::StringRef name, blender::StringRef value);

  /**
   * Add an integer variable with the given name and value.
   *
   * If there is already a variable with that name, regardless of type, the new
   * variable is *not* added (no overwriting).
   *
   * \return True if the variable was successfully added, false if there was
   * already a variable with that name.
   */
  bool add_integer(blender::StringRef name, int64_t value);

  /**
   * Add a float variable with the given name and value.
   *
   * If there is already a variable with that name, regardless of type, the new
   * variable is *not* added (no overwriting).
   *
   * \return True if the variable was successfully added, false if there was
   * already a variable with that name.
   */
  bool add_float(blender::StringRef name, double value);

  /**
   * Fetch the value of the string variable with the given name.
   *
   * \return The value if a string variable with that name exists, nullopt
   * otherwise.
   */
  std::optional<blender::StringRefNull> get_string(blender::StringRef name) const;

  /**
   * Fetch the value of the integer variable with the given name.
   *
   * \return The value if a integer variable with that name exists, nullopt
   * otherwise.
   */
  std::optional<int64_t> get_integer(blender::StringRef name) const;

  /**
   * Fetch the value of the float variable with the given name.
   *
   * \return The value if a float variable with that name exists, nullopt
   * otherwise.
   */
  std::optional<double> get_float(blender::StringRef name) const;
};

/**
 * Build path variables based on available information.
 *
 * All parameters are allowed to be null, in which case the variables derived
 * from those parameters will simply not be included.
 *
 * This is generally used to create the variables passed to
 * `BKE_path_apply_variables()`.
 *
 * Note: this does not and *shouldn't* include adding a variable for the
 * absolute path to the current blend file. That is handled by `BLI_path_abs()`
 * (with the special "//" syntax), which is called in specific ways for e.g.
 * cache paths such that corner cases are handled properly. We specifically
 * avoid that here, since the use-case is already addressed and it would be easy
 * to mess up the specifics.
 *
 *
 * \param blend_file_path: full path to the blend file, including the file name
 * (a directory-only path--ending with a slash--will also be accepted, but then
 * no "file_name" variable will be created). Typically you should fetch this
 * with `ID_BLEND_PATH()`, but there are plenty of exceptions. Note that this
 * should be the blend file that the path you're going to generate with the
 * variables "belongs" to.
 *
 * \param frame_number: the current frame.
 *
 * \param render_data: start/end frame, output resolution, and fps. Note: the
 * current frame in this is *not* used. Use the `frame_number` parameter for
 * that.
 *
 *
 * \see BKE_path_apply_variables()
 *
 * \see BLI_path_abs()
 */
VariableMap BKE_build_blender_variables(const char *blend_file_path,
                                        std::optional<uint64_t> frame_number,
                                        const RenderData *render_data);

/**
 * Substitutes `${variable_name}` syntax with the value of the named variable in
 * the given path.
 *
 * Note that this mutates the path in-place. The path should be a
 * null-terminated string.
 *
 * For integer and float variables, there is additional syntax to perform
 * formatting.
 *
 * TODO: document the formatting syntax once it's settled and agreed upon.
 */
bool BKE_path_apply_variables(char path[FILE_MAX], const VariableMap &variables);
