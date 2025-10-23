/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 * \brief Path variable handling implementation - supports template syntax in file browser paths.
 */

#include "file_path_variables.hh"

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

/* Helper to normalize a path in place */
static void normalize_path(char *path, size_t buffer_size)
{
  BLI_path_normalize(path);
  BLI_path_slash_rstrip(path);
}

/* Helper to resolve and normalize paths for comparison */
static void resolve_and_normalize_paths(const char *template_path,
                                        const char *current_path,
                                        char *resolved_template,
                                        char *normalized_current,
                                        size_t buffer_size)
{
  BLI_strncpy(resolved_template, template_path, buffer_size);
  resolve_template_variables(resolved_template, buffer_size);
  normalize_path(resolved_template, buffer_size);

  BLI_strncpy(normalized_current, current_path, buffer_size);
  normalize_path(normalized_current, buffer_size);
}

/* Check if current path is within a resolved template directory */
static bool is_within_template_bounds(const char *template_path, const char *current_path)
{
  if (!BKE_path_contains_template_syntax(template_path)) {
    return false;
  }

  char resolved_template[FILE_MAX];
  char normalized_current[FILE_MAX];
  resolve_and_normalize_paths(
      template_path, current_path, resolved_template, normalized_current, FILE_MAX);

  const size_t resolved_len = strlen(resolved_template);
  const size_t current_len = strlen(normalized_current);

  /* Exact match */
  if (resolved_len == current_len) {
    return strcmp(normalized_current, resolved_template) == 0;
  }
  
  /* Check if one path is prefix of the other (parent/child relationship) */
  const size_t min_len = (resolved_len < current_len) ? resolved_len : current_len;
  return strncmp(resolved_template, normalized_current, min_len) == 0 && 
         (resolved_template[min_len] == '/' || normalized_current[min_len] == '/');
}

/* Update template path when navigating within template directory */
static bool update_template_on_navigation(const char *original_template,
                                          const char *current_path,
                                          char *result,
                                          size_t result_maxlen)
{
  if (!BKE_path_contains_template_syntax(original_template)) {
    return false;
  }

  char resolved_template[FILE_MAX];
  char normalized_current[FILE_MAX];
  resolve_and_normalize_paths(
      original_template, current_path, resolved_template, normalized_current, FILE_MAX);

  const size_t resolved_len = strlen(resolved_template);
  const size_t current_len = strlen(normalized_current);

  /* Case 1: Exact match */
  if (current_len == resolved_len && strcmp(normalized_current, resolved_template) == 0) {
    BLI_strncpy(result, original_template, result_maxlen);
    return true;
  }

  /* Case 2: Going deeper into template directory */
  if (current_len > resolved_len && 
      strncmp(normalized_current, resolved_template, resolved_len) == 0 &&
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
  if (resolved_len > current_len &&
      strncmp(resolved_template, normalized_current, current_len) == 0 &&
      (current_len == 0 || resolved_template[current_len] == '/'))
  {
    /* Count directory levels to go up */
    const char *remaining_path = resolved_template + current_len;
    if (*remaining_path == '/') {
      remaining_path++;
    }
    
    int levels_up = (*remaining_path != '\0') ? 1 : 0;
    for (const char *p = remaining_path; *p; p++) {
      if (*p == '/') {
        levels_up++;
      }
    }

    /* Go up from template path */
    BLI_strncpy(result, original_template, result_maxlen);
    BLI_path_slash_rstrip(result);
    for (int i = 0; i < levels_up && result[0]; i++) {
      BLI_path_parent_dir(result);
    }
    
    return true;
  }

  return false;
}

void handle_path_input(FileSelectParams *params, const char *input_path)
{
  char resolved_path[FILE_MAX];
  BLI_strncpy(resolved_path, input_path, sizeof(resolved_path));
  
  if (BKE_path_contains_template_syntax(resolved_path)) {
    resolve_template_variables(resolved_path, sizeof(resolved_path));
  }
  
  /* Update all three path fields directly */
  BLI_strncpy(params->dir_template, input_path, sizeof(params->dir_template));
  BLI_strncpy(params->dir, resolved_path, sizeof(params->dir));
  BLI_strncpy(params->dir_resolved, resolved_path, sizeof(params->dir_resolved));
}

void handle_navigation(FileSelectParams *params, const char *new_directory)
{
  /* Try to preserve template if we were using one */
  if (has_template_path(params) &&
      is_within_template_bounds(params->dir_template, new_directory))
  {
    char updated_template[FILE_MAX];
    if (update_template_on_navigation(
            params->dir_template, new_directory, updated_template, sizeof(updated_template)))
    {
      /* Resolve template for display */
      char resolved[FILE_MAX];
      BLI_strncpy(resolved, updated_template, sizeof(resolved));
      if (BKE_path_contains_template_syntax(resolved)) {
        resolve_template_variables(resolved, sizeof(resolved));
      }

      BLI_strncpy(params->dir_template, updated_template, sizeof(params->dir_template));
      BLI_strncpy(params->dir, new_directory, sizeof(params->dir));
      BLI_strncpy(params->dir_resolved, resolved, sizeof(params->dir_resolved));
      return;
    }
  }

  /* Template not preserved - all paths are the same */
  BLI_strncpy(params->dir_template, new_directory, sizeof(params->dir_template));
  BLI_strncpy(params->dir, new_directory, sizeof(params->dir));
  BLI_strncpy(params->dir_resolved, new_directory, sizeof(params->dir_resolved));
}

void initialize_path_fields(FileSelectParams *params)
{
  if (BKE_path_contains_template_syntax(params->dir)) {
    char resolved_path[FILE_MAX];
    BLI_strncpy(resolved_path, params->dir, sizeof(resolved_path));
    resolve_template_variables(resolved_path, sizeof(resolved_path));

    BLI_strncpy(params->dir_template, params->dir, sizeof(params->dir_template));
    BLI_strncpy(params->dir, resolved_path, sizeof(params->dir));
    BLI_strncpy(params->dir_resolved, resolved_path, sizeof(params->dir_resolved));
  }
  else {
    /* No templates - initialize empty fields with current dir */
    const char *dir_to_use = params->dir;
    if (params->dir_template[0] == '\0') {
      BLI_strncpy(params->dir_template, dir_to_use, sizeof(params->dir_template));
    }
    if (params->dir_resolved[0] == '\0') {
      BLI_strncpy(params->dir_resolved, dir_to_use, sizeof(params->dir_resolved));
    }
  }
}

bool has_template_path(const FileSelectParams *params)
{
  return params->dir_template[0] != '\0' &&
         BKE_path_contains_template_syntax(params->dir_template);
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