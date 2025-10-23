/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 * \brief Path template handling implementation for file browser.
 */

#include "path_template_handler.hh"

#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_path_templates.hh"

#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "DNA_listBase.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include <cstdio>
#include <cstring>

namespace blender::editor::file {

PathTemplateHandler::PathTemplateHandler()
{
  template_path_[0] = '\0';
  resolved_path_[0] = '\0';
  preview_path_[0] = '\0';
  previous_template_[0] = '\0';
  previous_resolved_[0] = '\0';
}

void PathTemplateHandler::resolve_template_variables(char *path, size_t path_maxlen) const
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

bool PathTemplateHandler::is_within_template_bounds(const char *template_path,
                                                   const char *current_path) const
{
  if (!BKE_path_contains_template_syntax(template_path)) {
    printf("DEBUG: is_within_template_bounds - no template syntax in '%s'\n", template_path);
    return false;
  }

  char resolved_template[FILE_MAX];
  BLI_strncpy(resolved_template, template_path, sizeof(resolved_template));
  resolve_template_variables(resolved_template, sizeof(resolved_template));

  /* Remove trailing slashes for consistent comparison */
  BLI_path_normalize(resolved_template);
  BLI_path_slash_rstrip(resolved_template);
  
  char normalized_current[FILE_MAX];
  BLI_strncpy(normalized_current, current_path, sizeof(normalized_current));
  BLI_path_normalize(normalized_current);
  BLI_path_slash_rstrip(normalized_current);

  printf("DEBUG: is_within_template_bounds:\n");
  printf("  template_path: '%s'\n", template_path);
  printf("  resolved_template: '%s'\n", resolved_template);
  printf("  current_path: '%s'\n", current_path);
  printf("  normalized_current: '%s'\n", normalized_current);

  const size_t resolved_len = strlen(resolved_template);
  const size_t current_len = strlen(normalized_current);

  /* Check if current path starts with resolved template path (current is deeper) */
  if (current_len >= resolved_len && 
      strncmp(normalized_current, resolved_template, resolved_len) == 0) {
    bool is_within = (current_len == resolved_len || 
                      (current_len > resolved_len && normalized_current[resolved_len] == '/'));
    printf("  Case 1 (current deeper): is_within = %s\n", is_within ? "true" : "false");
    return is_within;
  }

  /* Check if resolved template starts with current path (current is parent) */
  if (resolved_len >= current_len &&
      strncmp(resolved_template, normalized_current, current_len) == 0) {
    bool is_within = (resolved_len == current_len || 
                      (resolved_len > current_len && resolved_template[current_len] == '/'));
    printf("  Case 2 (current parent): is_within = %s\n", is_within ? "true" : "false");
    return is_within;
  }

  printf("  No match: returning false\n");
  return false;
}

int PathTemplateHandler::get_common_path_length(const char *path1, const char *path2) const
{
  int common_len = 0;
  int last_separator = -1;

  while (path1[common_len] && path2[common_len] && path1[common_len] == path2[common_len]) {
    if (path1[common_len] == '/') {
      last_separator = common_len;
    }
    common_len++;
  }

  /* Return length up to last common directory separator */
  return (last_separator >= 0) ? last_separator : 0;
}

void PathTemplateHandler::get_path_difference(const char *base_path,
                                             const char *target_path,
                                             char *result,
                                             size_t result_maxlen) const
{
  const size_t base_len = strlen(base_path);
  const size_t target_len = strlen(target_path);

  result[0] = '\0';

  if (target_len <= base_len) {
    return; /* Target is not longer than base */
  }

  if (strncmp(base_path, target_path, base_len) != 0) {
    return; /* Paths don't match at base */
  }

  /* Skip past base path and separator */
  const char *diff_start = target_path + base_len;
  if (diff_start[0] == '/') {
    diff_start++;
  }

  BLI_strncpy(result, diff_start, result_maxlen);
}

bool PathTemplateHandler::reconstruct_template_for_navigation(const char *original_template,
                                                             const char *current_path,
                                                             char *result,
                                                             size_t result_maxlen) const
{
  if (!BKE_path_contains_template_syntax(original_template)) {
    return false;
  }

  /* Resolve the original template to get the base resolved path */
  char resolved_template[FILE_MAX];
  BLI_strncpy(resolved_template, original_template, sizeof(resolved_template));
  resolve_template_variables(resolved_template, sizeof(resolved_template));

  /* Remove trailing slashes for consistent comparison */
  BLI_path_normalize(resolved_template);
  BLI_path_slash_rstrip(resolved_template);
  
  char normalized_current[FILE_MAX];
  BLI_strncpy(normalized_current, current_path, sizeof(normalized_current));
  BLI_path_normalize(normalized_current);
  BLI_path_slash_rstrip(normalized_current);

  const size_t resolved_len = strlen(resolved_template);
  const size_t current_len = strlen(normalized_current);

  /* Case 1: Exact match - current path matches resolved template exactly */
  if (current_len == resolved_len && strcmp(normalized_current, resolved_template) == 0) {
    BLI_strncpy(result, original_template, result_maxlen);
    return true;
  }

  /* Case 2: Current path is within resolved template (going deeper) */
  if (current_len > resolved_len && 
      strncmp(normalized_current, resolved_template, resolved_len) == 0 &&
      normalized_current[resolved_len] == '/') {
    
    /* Get the additional path beyond the resolved template */
    const char *extra_path = normalized_current + resolved_len + 1;
    
    /* Create template path + extra path */
    char template_base[FILE_MAX];
    BLI_strncpy(template_base, original_template, sizeof(template_base));
    BLI_path_slash_rstrip(template_base); /* Remove trailing slash */
    
    if (strlen(extra_path) > 0) {
      BLI_path_join(result, result_maxlen, template_base, extra_path);
    } else {
      BLI_strncpy(result, template_base, result_maxlen);
    }
    return true;
  }

  /* Case 3: Current path is parent of resolved template (going up) */
  if (resolved_len > current_len && 
      strncmp(resolved_template, normalized_current, current_len) == 0 &&
      (current_len == 0 || resolved_template[current_len] == '/')) {
    
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
    if (strlen(remaining_path) > 0) {
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

void PathTemplateHandler::initialize_from_params(const FileSelectParams *params)
{
  BLI_strncpy(template_path_, params->dir_variable, sizeof(template_path_));
  BLI_strncpy(resolved_path_, params->dir, sizeof(resolved_path_));
  BLI_strncpy(preview_path_, params->dir_preview, sizeof(preview_path_));
  BLI_strncpy(previous_template_, params->dir_variable, sizeof(previous_template_));
  BLI_strncpy(previous_resolved_, params->dir, sizeof(previous_resolved_));
}

StringRefNull PathTemplateHandler::handle_user_input(const char *input_path)
{
  /* Store previous state */
  BLI_strncpy(previous_template_, template_path_, sizeof(previous_template_));
  BLI_strncpy(previous_resolved_, resolved_path_, sizeof(previous_resolved_));

  /* Set new template path */
  BLI_strncpy(template_path_, input_path, sizeof(template_path_));

  if (BKE_path_contains_template_syntax(input_path)) {
    /* Resolve template for navigation */
    BLI_strncpy(resolved_path_, input_path, sizeof(resolved_path_));
    resolve_template_variables(resolved_path_, sizeof(resolved_path_));
    
    /* Set preview to resolved path */
    BLI_strncpy(preview_path_, resolved_path_, sizeof(preview_path_));
  }
  else {
    /* No templates - all paths are the same */
    BLI_strncpy(resolved_path_, input_path, sizeof(resolved_path_));
    BLI_strncpy(preview_path_, input_path, sizeof(preview_path_));
  }

  return resolved_path_;
}

bool PathTemplateHandler::handle_navigation_change(const char *new_directory)
{
  /* Store previous state */
  BLI_strncpy(previous_resolved_, resolved_path_, sizeof(previous_resolved_));

  /* Update resolved path */
  BLI_strncpy(resolved_path_, new_directory, sizeof(resolved_path_));

  /* Try to preserve template if we were using one */
  if (has_template_variables() && is_within_template_bounds(template_path_, new_directory)) {
    char reconstructed[FILE_MAX];
    if (reconstruct_template_for_navigation(template_path_, new_directory, 
                                           reconstructed, sizeof(reconstructed))) {
      BLI_strncpy(template_path_, reconstructed, sizeof(template_path_));
      
      /* Update preview with resolved template */
      BLI_strncpy(preview_path_, reconstructed, sizeof(preview_path_));
      if (BKE_path_contains_template_syntax(preview_path_)) {
        resolve_template_variables(preview_path_, sizeof(preview_path_));
      }
      
      return true; /* Template preserved */
    }
  }

  /* Template not preserved - reset to simple path */
  reset_to_simple_path(new_directory);
  return false;
}

void PathTemplateHandler::update_params(FileSelectParams *params) const
{
  BLI_strncpy(params->dir_variable, template_path_, sizeof(params->dir_variable));
  BLI_strncpy(params->dir, resolved_path_, sizeof(params->dir));
  BLI_strncpy(params->dir_preview, preview_path_, sizeof(params->dir_preview));
}

bool PathTemplateHandler::has_template_variables() const
{
  return template_path_[0] != '\0' && BKE_path_contains_template_syntax(template_path_);
}

void PathTemplateHandler::reset_to_simple_path(const char *path)
{
  BLI_strncpy(template_path_, path, sizeof(template_path_));
  BLI_strncpy(resolved_path_, path, sizeof(resolved_path_));
  BLI_strncpy(preview_path_, path, sizeof(preview_path_));
}

void PathTemplateHandler::debug_print_state(const char *context) const
{
  printf("DEBUG PathTemplateHandler [%s]:\n", context);
  printf("  template_path_: '%s'\n", template_path_);
  printf("  resolved_path_: '%s'\n", resolved_path_);
  printf("  preview_path_: '%s'\n", preview_path_);
  printf("  has_template_variables: %s\n", has_template_variables() ? "true" : "false");
}

}  // namespace blender::editor::file