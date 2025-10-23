/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 * \brief Path template handling utilities implementation.
 */

#include "path_template_utils.hh"
#include "path_template_handler.hh"

#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_path_templates.hh"

#include "BLI_string.h"

#include "DNA_scene_types.h"
#include "DNA_space_types.h"

namespace blender::editor::file {

/* Static handler instance for the file browser */
static PathTemplateHandler g_template_handler;

void handle_template_path_input(FileSelectParams *params, const char *input_path)
{
  /* Initialize handler with current params state */
  g_template_handler.initialize_from_params(params);
  
  /* Handle the user input */
  g_template_handler.handle_user_input(input_path);
  
  /* Update params with new state */
  g_template_handler.update_params(params);
  
  /* Debug output */
  g_template_handler.debug_print_state("handle_template_path_input");
}

void handle_template_navigation(FileSelectParams *params, const char *new_directory)
{
  /* Initialize handler with current params state */
  g_template_handler.initialize_from_params(params);
  
  /* Handle the navigation change */
  bool template_preserved = g_template_handler.handle_navigation_change(new_directory);
  
  /* Update params with new state */
  g_template_handler.update_params(params);
  
  /* Debug output */
  g_template_handler.debug_print_state(template_preserved ? 
                                      "handle_template_navigation (preserved)" : 
                                      "handle_template_navigation (cleared)");
}

void initialize_template_paths(FileSelectParams *params)
{
  /* Check if dir contains template variables and preserve them */
  if (BKE_path_contains_template_syntax(params->dir)) {
    /* params->dir has templates - copy to dir_variable */
    BLI_strncpy(params->dir_variable, params->dir, sizeof(params->dir_variable));
    
    /* Resolve templates for preview */
    BLI_strncpy(params->dir_preview, params->dir, sizeof(params->dir_preview));
    blender::bke::path_templates::VariableMap variables;
    const Scene *scene = G.main ? static_cast<const Scene *>(G.main->scenes.first) : nullptr;
    BKE_add_template_variables_general(variables, scene ? &scene->id : nullptr);
    if (scene) {
      BKE_add_template_variables_for_render_path(variables, *scene);
    }
    BKE_path_apply_template(params->dir_preview, sizeof(params->dir_preview), variables);
    
    /* Keep params->dir as the resolved path for navigation */
    BLI_strncpy(params->dir, params->dir_preview, sizeof(params->dir));
  }
  else {
    /* No templates in dir - initialize with current directory if templates are empty */
    if (params->dir_variable[0] == '\0') {
      BLI_strncpy(params->dir_variable, params->dir, sizeof(params->dir_variable));
    }
    if (params->dir_preview[0] == '\0') {
      BLI_strncpy(params->dir_preview, params->dir, sizeof(params->dir_preview));
    }
  }
  
  /* Initialize handler */
  g_template_handler.initialize_from_params(params);
  g_template_handler.debug_print_state("initialize_template_paths");
}

}  // namespace blender::editor::file