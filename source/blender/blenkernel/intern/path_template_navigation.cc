/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenkernel
 * \brief Path template handling implementation - preserves template variables during file browser
 * navigation.
 */

#include <cstring>

#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_path_templates.hh"

#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "RNA_access.hh"

#include "WM_types.hh"

namespace blender::bke::path_templates {

/* -------------------------------------------------------------------- */
/** \name Internal Helper Functions
 * \{ */

static void resolve_template_variables(char *path,
                                       size_t path_maxlen,
                                       const VariableMap *variables)
{
  if (!BKE_path_contains_template_syntax(path)) {
    return;
  }

  if (!variables) {
    return;
  }

  BKE_path_apply_template(path, path_maxlen, *variables);
}

/* Helper to normalize a path in place */
static void normalize_path(char *path)
{
  BLI_path_normalize(path);
  BLI_path_slash_rstrip(path);
}

/* Helper to resolve and normalize paths for comparison */
static void resolve_and_normalize_paths(const char *template_path,
                                        const char *current_path,
                                        char *resolved_template,
                                        char *normalized_current,
                                        size_t buffer_size,
                                        const VariableMap *variables)
{
  BLI_strncpy(resolved_template, template_path, buffer_size);
  resolve_template_variables(resolved_template, buffer_size, variables);
  normalize_path(resolved_template);

  BLI_strncpy(normalized_current, current_path, buffer_size);
  normalize_path(normalized_current);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Template Boundary and Navigation Helpers
 * \{ */

/* Check if current path is within a resolved template directory */
static bool is_within_template_bounds(const char *template_path,
                                      const char *current_path,
                                      const VariableMap *variables)
{
  if (!BKE_path_contains_template_syntax(template_path)) {
    return false;
  }

  char resolved_template[FILE_MAX];
  char normalized_current[FILE_MAX];
  resolve_and_normalize_paths(
      template_path, current_path, resolved_template, normalized_current, FILE_MAX, variables);

  const size_t resolved_len = strlen(resolved_template);
  const size_t current_len = strlen(normalized_current);

  /* Exact match */
  if (resolved_len == current_len) {
    return STREQ(normalized_current, resolved_template);
  }

  /* Check if one path is prefix of the other (parent/child relationship) */
  if (resolved_len < current_len) {
    return STREQLEN(resolved_template, normalized_current, resolved_len) &&
           normalized_current[resolved_len] == '/';
  }

  return STREQLEN(resolved_template, normalized_current, current_len) &&
         resolved_template[current_len] == '/';
}

/**
 * Update template path when navigating within template directory.
 *
 * Algorithm: Compares resolved template with current path to determine if navigation
 * stayed within template bounds, then reconstructs appropriate template syntax:
 * - Exact match: Return original template unchanged
 * - Going deeper: Append extra path components to template
 * - Going up: Remove directory levels from template path
 */
static bool update_template_on_navigation(const char *original_template,
                                          const char *current_path,
                                          char *result,
                                          size_t result_maxlen,
                                          const VariableMap *variables)
{
  if (!BKE_path_contains_template_syntax(original_template)) {
    return false;
  }

  char resolved_template[FILE_MAX];
  char normalized_current[FILE_MAX];
  resolve_and_normalize_paths(
      original_template, current_path, resolved_template, normalized_current, FILE_MAX, variables);

  const size_t resolved_len = strlen(resolved_template);
  const size_t current_len = strlen(normalized_current);

  /* Case 1: Exact match */
  if (current_len == resolved_len && STREQ(normalized_current, resolved_template)) {
    BLI_strncpy(result, original_template, result_maxlen);
    return true;
  }

  /* Case 2: Going deeper into template directory */
  if (current_len > resolved_len &&
      STREQLEN(normalized_current, resolved_template, resolved_len) &&
      normalized_current[resolved_len] == '/')
  {
    const char *extra_path = normalized_current + resolved_len + 1;
    char template_base[FILE_MAX];
    BLI_strncpy(template_base, original_template, sizeof(template_base));
    BLI_path_slash_rstrip(template_base);

    BLI_path_join(result, result_maxlen, template_base, extra_path);
    return true;
  }

  /* Case 3: Going up from template directory */
  if (resolved_len > current_len && STREQLEN(resolved_template, normalized_current, current_len) &&
      (current_len == 0 || resolved_template[current_len] == '/'))
  {
    /* Go up from template path */
    BLI_strncpy(result, original_template, result_maxlen);
    BLI_path_slash_rstrip(result);

    BLI_path_parent_dir(result);

    return true;
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

void path_template_nav_initialize(FileSelectParams *params, const VariableMap *variables)
{
  if (BKE_path_contains_template_syntax(params->dir)) {
    char resolved_path[FILE_MAX];
    BLI_strncpy(resolved_path, params->dir, sizeof(resolved_path));
    resolve_template_variables(resolved_path, sizeof(resolved_path), variables);

    BLI_strncpy(params->dir_template, params->dir, sizeof(params->dir_template));
    BLI_strncpy(params->dir, resolved_path, sizeof(params->dir));
  }
  else {
    /* No templates - initialize empty fields with current dir */
    if (params->dir_template[0] == '\0') {
      BLI_strncpy(params->dir_template, params->dir, sizeof(params->dir_template));
    }
  }
}

void path_template_nav_handle_text(FileSelectParams *params,
                                   const char *input_path,
                                   const VariableMap *variables)
{
  char resolved_path[FILE_MAX];
  BLI_strncpy(resolved_path, input_path, sizeof(resolved_path));

  if (BKE_path_contains_template_syntax(resolved_path)) {
    resolve_template_variables(resolved_path, sizeof(resolved_path), variables);
  }

  /* Update both path fields:
   * - dir_template: stores the original input with template variables
   * - dir: the resolved/evaluated path for display and file operations */
  BLI_strncpy(params->dir_template, input_path, sizeof(params->dir_template));
  BLI_strncpy(params->dir, resolved_path, sizeof(params->dir));
}

void path_template_nav_handle_browse(FileSelectParams *params,
                                     const char *new_directory,
                                     const VariableMap *variables)
{
  /* Try to preserve template if we were using one */
  if (BKE_path_contains_template_syntax(params->dir_template) &&
      is_within_template_bounds(params->dir_template, new_directory, variables))
  {
    char updated_template[FILE_MAX];
    if (update_template_on_navigation(params->dir_template,
                                      new_directory,
                                      updated_template,
                                      sizeof(updated_template),
                                      variables))
    {
      /* Resolve template for display */
      char resolved[FILE_MAX];
      BLI_strncpy(resolved, updated_template, sizeof(resolved));
      if (BKE_path_contains_template_syntax(resolved)) {
        resolve_template_variables(resolved, sizeof(resolved), variables);
      }

      BLI_strncpy(params->dir_template, updated_template, sizeof(params->dir_template));
      BLI_strncpy(params->dir, resolved, sizeof(params->dir));
      return;
    }
  }

  /* Template not preserved - set both paths to the new directory */
  BLI_strncpy(params->dir_template, new_directory, sizeof(params->dir_template));
  BLI_strncpy(params->dir, new_directory, sizeof(params->dir));
}

void path_template_nav_set_operator_property(bContext *C,
                                             wmOperator *op,
                                             const char *prop_name,
                                             const char *new_value)
{
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, prop_name);
  if (!prop) {
    return;
  }

  char value[FILE_MAX];
  RNA_property_string_get(op->ptr, prop, value);
  RNA_property_string_set(op->ptr, prop, new_value);

  if (RNA_property_update_check(prop) && !STREQ(new_value, value)) {
    RNA_property_update(C, op->ptr, prop);
  }
}

/** \} */

}  // namespace blender::bke::path_templates
