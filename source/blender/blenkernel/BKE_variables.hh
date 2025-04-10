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

#include "BKE_report.hh"

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
 * Build a variable map based on available information.
 *
 * All parameters are allowed to be null, in which case the variables derived
 * from those parameters will simply not be included.
 *
 * This is typically used to create the variables passed to
 * `BKE_path_apply_variables()`.
 *
 * \param blend_file_path: full path to the blend file, including the file name.
 * Typically you should fetch this with `ID_BLEND_PATH()`, but there are
 * exceptions. The key thing is that this should be the path to the *relevant*
 * blend file for the context that the variables are going to be used in. For
 * example, if the context is a linked ID then this path should (very likely) be
 * the path to that ID's library blend file, not the currently opened one.
 *
 * \param render_data: used for output resolution and fps. Note for the future:
 * when we add a "current frame number" variable it should *not* come from this
 * parameter, but be passed separately. This is because the callers of this
 * function sometimes have the current frame defined separately from the
 * available RenderData (see e.g. `do_makepicstring()`).
 *
 * \see BKE_path_apply_variables()
 *
 * \see BLI_path_abs()
 */
VariableMap BKE_build_blender_variables(const char *blend_file_path,
                                        const RenderData *render_data);

enum class VariableParseErrorType {
  UNESCAPED_CURLY_BRACE,
  VARIABLE_SYNTAX,
  FORMAT_SPECIFIER,
  UNKNOWN_VARIABLE,
};

struct VariableParseError {
  VariableParseErrorType type;
  blender::IndexRange byte_range;
};

bool operator==(const VariableParseError &left, const VariableParseError &right);

/**
 * Validate the variable syntax in the given path.
 *
 * This does *not* validate whether the variables referenced in the given path
 * exist or not, nor whether the formatting specification in a variable
 * reference is appropriate for its type. This only validates what can be
 * validated without knowing anything about the variables themselves.
 *
 * \return An empty vector if valid, or a vector of the parse errors if invalid.
 */
blender::Vector<VariableParseError> BKE_validate_variable_syntax(char path[FILE_MAX]);

/**
 * Perform variable substitution on the given path.
 *
 * This mutates the path in-place. The path must be a null-terminated string
 * with a total allocation size of at least `FILE_MAX` bytes.
 *
 * The syntax for variables is `{variable_name}` or
 * {variable_name:format_spec}`. They will be substituted with the respective
 * variable value if and only if both of the following hold true:
 *
 * - A variable with that name exists in the passed VariableMap.
 * - The format spec (if any is provided) is syntactically correct and applies
 *   to the variable's type.
 *
 * Otherwise it will be skipped and left as-is, as an indication that it
 * couldn't be processed.
 *
 * The format specification syntax currently only applies to numerical variables
 * (integer or float), and uses hash symbols (#) to indicate the number of
 * digits to print the number with.  It can be in any of the following forms:
 *
 * - `####`: format as an integer with at least 4 digits, padding with zeros as
 *   needed.
 * - `.###`: format as a float with precisely 3 fractional digits.
 * - `##.###`: format as a float with at least 2 integer-part digits (padded
 *   with zeros as necessary) and precisely 3 fractional-part digits.
 *
 * This function also processes a simple escape sequence for writing literal "{"
 * and "}": like Python format strings, double braces "{{" and "}}" are treated
 * as escape sequences for "{" and "}", and are substituted appropriately. Note
 * that this substitution only happens *outside* of the variable syntax, and
 * therefore cannot e.g. be used inside variable names.
 *
 * \return A vector of any errors encountered. If the vector is empty, that
 * means success. Otherwise the path is left unaltered and the errors are
 * returned as a vector.
 */
blender::Vector<VariableParseError> BKE_path_apply_variables(char path[FILE_MAX],
                                                             const VariableMap &variables);

void BKE_report_path_variable_errors(ReportList *reports,
                                     eReportType report_type,
                                     const char path[FILE_MAX],
                                     blender::Span<VariableParseError> errors);

/** \} */
