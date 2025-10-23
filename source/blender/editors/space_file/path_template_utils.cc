/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 * \brief Path template handling utilities implementation - consolidated template logic.
 */

#include "path_template_utils.hh"

#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_path_templates.hh"

#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "RNA_access.hh"

#include "WM_types.hh"

#include <cstring>

namespace blender::editor::file {

/* Centralized template variable resolution function - used by all template operations */
static void resolve_template_variables(char *path, size_t path_maxlen)
{
  if (!BKE_path_contains_template_syntax(path)) {
    return;
  }

  blender::bke::path_templates::VariableMap variables;
  const Scene *scene = G.main ? static_cast<const Scene *>(G.main->scenes.first) : nullptr;
  BKE_add_template_variables_general(variables, scene ? &scene->id : nullptr);
  if (scene) {
    BKE_add_template_variables_for_render_path(variables, *scene);
  }
  BKE_path_apply_template(path, path_maxlen, variables);
}

/* Helper function to normalize template and current paths for comparison */
static void normalize_paths_for_comparison(const char *template_path,
                                           const char *current_path,
                                           char *resolved_template,
                                           char *normalized_current,
                                           size_t buffer_size,
                                           size_t *resolved_len = nullptr,
                                           size_t *current_len = nullptr)
{
  BLI_strncpy(resolved_template, template_path, buffer_size);
  resolve_template_variables(resolved_template, buffer_size);
  BLI_path_normalize(resolved_template);
  BLI_path_slash_rstrip(resolved_template);

  BLI_strncpy(normalized_current, current_path, buffer_size);
  BLI_path_normalize(normalized_current);
  BLI_path_slash_rstrip(normalized_current);

  if (resolved_len) {
    *resolved_len = strlen(resolved_template);
  }
  if (current_len) {
    *current_len = strlen(normalized_current);
  }
}

/* Check if current path is within a resolved template directory */
static bool is_within_template_bounds(const char *template_path, const char *current_path)
{
  if (!BKE_path_contains_template_syntax(template_path)) {
    return false;
  }

  char resolved_template[FILE_MAX];
  char normalized_current[FILE_MAX];
  size_t resolved_len, current_len;
  normalize_paths_for_comparison(template_path,
                                 current_path,
                                 resolved_template,
                                 normalized_current,
                                 FILE_MAX,
                                 &resolved_len,
                                 &current_len);

  /* Check if current path starts with resolved template path (current is deeper) */
  if (current_len >= resolved_len &&
      strncmp(normalized_current, resolved_template, resolved_len) == 0)
  {
    bool is_within = (current_len == resolved_len ||
                      (current_len > resolved_len && normalized_current[resolved_len] == '/'));
    return is_within;
  }

  /* Check if resolved template starts with current path (current is parent) */
  if (resolved_len >= current_len &&
      strncmp(resolved_template, normalized_current, current_len) == 0)
  {
    bool is_within = (resolved_len == current_len ||
                      (resolved_len > current_len && resolved_template[current_len] == '/'));
    return is_within;
  }

  return false;
}

/* Reconstruct template path when navigating within template directory */
static bool reconstruct_template_for_navigation(const char *original_template,
                                                const char *current_path,
                                                char *result,
                                                size_t result_maxlen)
{
  if (!BKE_path_contains_template_syntax(original_template)) {
    return false;
  }

  char resolved_template[FILE_MAX];
  char normalized_current[FILE_MAX];
  size_t resolved_len, current_len;
  normalize_paths_for_comparison(original_template,
                                 current_path,
                                 resolved_template,
                                 normalized_current,
                                 FILE_MAX,
                                 &resolved_len,
                                 &current_len);

  /* Case 1: Exact match - current path matches resolved template exactly */
  if (current_len == resolved_len && strcmp(normalized_current, resolved_template) == 0) {
    BLI_strncpy(result, original_template, result_maxlen);
    return true;
  }

  /* Case 2: Current path is within resolved template (going deeper) */
  if (current_len > resolved_len &&
      strncmp(normalized_current, resolved_template, resolved_len) == 0 &&
      normalized_current[resolved_len] == '/')
  {

    /* Get the additional path beyond the resolved template */
    const char *extra_path = normalized_current + resolved_len + 1;

    /* Create template path + extra path */
    char template_base[FILE_MAX];
    BLI_strncpy(template_base, original_template, sizeof(template_base));
    BLI_path_slash_rstrip(template_base); /* Remove trailing slash */

    if (*extra_path != '\0') {
      BLI_path_join(result, result_maxlen, template_base, extra_path);
    }
    else {
      BLI_strncpy(result, template_base, result_maxlen);
    }
    return true;
  }

  /* Case 3: Current path is parent of resolved template (going up) */
  if (resolved_len > current_len &&
      strncmp(resolved_template, normalized_current, current_len) == 0 &&
      (current_len == 0 || resolved_template[current_len] == '/'))
  {

    /* We need to go up from the template path */
    char template_base[FILE_MAX];
    BLI_strncpy(template_base, original_template, sizeof(template_base));
    BLI_path_slash_rstrip(template_base);

    /* Count how many levels we need to go up */
    const char *remaining_path = resolved_template + current_len;
    if (remaining_path[0] == '/') {
      remaining_path++;
    }

    int levels_up = 0;
    for (const char *p = remaining_path; *p; p++) {
      if (*p == '/') {
        levels_up++;
      }
    }
    /* Count the final directory if there's no trailing slash */
    if (*remaining_path != '\0') {
      levels_up++;
    }

    /* Remove levels from template path */
    for (int i = 0; i < levels_up && template_base[0]; i++) {
      BLI_path_parent_dir(template_base);
    }

    BLI_strncpy(result, template_base, result_maxlen);
    return true;
  }

  return false;
}

/* Helper to update all three path fields */
static void update_path_fields(FileSelectParams *params,
                               const char *variable_path,
                               const char *resolved_path,
                               const char *preview_path)
{
  BLI_strncpy(params->dir_variable, variable_path, sizeof(params->dir_variable));
  BLI_strncpy(params->dir, resolved_path, sizeof(params->dir));
  BLI_strncpy(params->dir_preview, preview_path, sizeof(params->dir_preview));
}

void handle_template_path_input(FileSelectParams *params, const char *input_path)
{
  if (BKE_path_contains_template_syntax(input_path)) {
    /* Resolve template for navigation and preview */
    char resolved_path[FILE_MAX];
    BLI_strncpy(resolved_path, input_path, sizeof(resolved_path));
    resolve_template_variables(resolved_path, sizeof(resolved_path));

    update_path_fields(params, input_path, resolved_path, resolved_path);
  }
  else {
    /* No templates - all paths are the same */
    update_path_fields(params, input_path, input_path, input_path);
  }
}

void handle_template_navigation(FileSelectParams *params, const char *new_directory)
{
  /* Try to preserve template if we were using one */
  if (should_use_template_path(params) &&
      is_within_template_bounds(params->dir_variable, new_directory))
  {
    char reconstructed[FILE_MAX];
    if (reconstruct_template_for_navigation(
            params->dir_variable, new_directory, reconstructed, sizeof(reconstructed)))
    {
      /* Template preserved - update preview with resolved template */
      char preview[FILE_MAX];
      BLI_strncpy(preview, reconstructed, sizeof(preview));
      if (BKE_path_contains_template_syntax(preview)) {
        resolve_template_variables(preview, sizeof(preview));
      }

      update_path_fields(params, reconstructed, new_directory, preview);
      return;
    }
  }

  /* Template not preserved or no template - reset to simple path */
  update_path_fields(params, new_directory, new_directory, new_directory);
}

void initialize_template_paths(FileSelectParams *params)
{
  if (BKE_path_contains_template_syntax(params->dir)) {
    /* Store original template, resolve for navigation and preview */
    char resolved_path[FILE_MAX];
    BLI_strncpy(resolved_path, params->dir, sizeof(resolved_path));
    resolve_template_variables(resolved_path, sizeof(resolved_path));

    update_path_fields(params, params->dir, resolved_path, resolved_path);
  }
  else {
    /* No templates - use current dir for empty fields */
    const char *var_path = params->dir_variable[0] ? params->dir_variable : params->dir;
    const char *prev_path = params->dir_preview[0] ? params->dir_preview : params->dir;

    update_path_fields(params, var_path, params->dir, prev_path);
  }
}

void resolve_path_templates(char *path, size_t path_maxlen)
{
  resolve_template_variables(path, path_maxlen);
}

bool should_use_template_path(const FileSelectParams *params)
{
  return params->dir_variable[0] != '\0' &&
         BKE_path_contains_template_syntax(params->dir_variable);
}

void set_operator_string_property(bContext *C,
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

}  // namespace blender::editor::file