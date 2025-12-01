/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenkernel
 * \brief Path template handling implementation - preserves template variables during file browser
 * navigation.
 */

#include <cstring>

#include "BKE_path_templates.hh"

#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "DNA_space_types.h"

namespace blender::bke::path_templates {

/* -------------------------------------------------------------------- */
/** \name Internal Helper Functions
 * \{ */

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
  if (variables) {
    BKE_path_apply_template(resolved_template, buffer_size, *variables);
  }
  normalize_path(resolved_template);

  BLI_strncpy(normalized_current, current_path, buffer_size);
  normalize_path(normalized_current);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Template Boundary and Navigation Helpers
 * \{ */

/* Check if current path is within a resolved template directory.
 * NOTE: Assumes variables is non-null - caller must check before calling. */
static bool is_within_template_bounds(const char *template_path,
                                      const char *current_path,
                                      const VariableMap &variables)
{
  char resolved_template[FILE_MAX];
  char normalized_current[FILE_MAX];
  resolve_and_normalize_paths(
      template_path, current_path, resolved_template, normalized_current, FILE_MAX, &variables);

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
 *
 * NOTE: Assumes variables is non-null - caller must check before calling.
 */
static bool update_template_on_navigation(const char *original_template,
                                          const char *current_path,
                                          char *result,
                                          size_t result_maxlen,
                                          const VariableMap &variables)
{
  char resolved_template[FILE_MAX];
  char normalized_current[FILE_MAX];
  resolve_and_normalize_paths(original_template,
                              current_path,
                              resolved_template,
                              normalized_current,
                              FILE_MAX,
                              &variables);

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

void nav_initialize(FileSelectParams *params, const VariableMap &variables)
{
  /* Store original path as template */
  BLI_strncpy(params->dir_template, params->dir, sizeof(params->dir_template));
  BKE_path_apply_template(params->dir, sizeof(params->dir), variables);
}

void nav_handle_text(FileSelectParams *params,
                     const char *input_path,
                     const VariableMap &variables)
{
  /* Store original input with template variables */
  BLI_strncpy(params->dir_template, input_path, sizeof(params->dir_template));
  BLI_strncpy(params->dir, input_path, sizeof(params->dir));
  BKE_path_apply_template(params->dir, sizeof(params->dir), variables);
}

void nav_handle_browse(FileSelectParams *params,
                       const char *new_directory,
                       const VariableMap &variables)
{
  /* Try to preserve template if we were using one */
  if (is_within_template_bounds(params->dir_template, new_directory, variables)) {
    char updated_template[FILE_MAX];
    if (update_template_on_navigation(params->dir_template,
                                      new_directory,
                                      updated_template,
                                      sizeof(updated_template),
                                      variables))
    {
      /* Store updated template */
      BLI_strncpy(params->dir_template, updated_template, sizeof(params->dir_template));
      BLI_strncpy(params->dir, updated_template, sizeof(params->dir));
      BKE_path_apply_template(params->dir, sizeof(params->dir), variables);
      return;
    }
  }

  /* Template not preserved - set both paths to the new directory */
  BLI_strncpy(params->dir_template, new_directory, sizeof(params->dir_template));
  BLI_strncpy(params->dir, new_directory, sizeof(params->dir));
}

void nav_sync_template_to_resolved(FileSelectParams *params, const VariableMap &variables)
{
  char updated_template[FILE_MAX];
  if (update_template_on_navigation(params->dir_template,
                                    params->dir,
                                    updated_template,
                                    sizeof(updated_template),
                                    variables))
  {
    BLI_strncpy(params->dir_template, updated_template, sizeof(params->dir_template));
  }
}

/** \} */

}  // namespace blender::bke::path_templates
