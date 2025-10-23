/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 * \brief Path template handling for file browser with variable preservation and resolution.
 */

#pragma once

#include "BLI_string_ref.hh"

#include "DNA_space_types.h"

struct FileSelectParams;
struct Scene;

namespace blender::editor::file {

/**
 * PathTemplateHandler manages path template functionality for the file browser.
 * 
 * It handles:
 * - Template variable resolution and preservation
 * - Navigation within template directory hierarchies  
 * - Two-way synchronization between template and resolved paths
 * - State management for dir, dir_variable, and dir_preview fields
 */
class PathTemplateHandler {
 private:
  /** Current unresolved template path */
  char template_path_[FILE_MAX];
  /** Current resolved path for navigation */
  char resolved_path_[FILE_MAX];
  /** Preview path showing resolved template */
  char preview_path_[FILE_MAX];
  /** Previous template state for comparison */
  char previous_template_[FILE_MAX];
  /** Previous resolved state for comparison */
  char previous_resolved_[FILE_MAX];

  /**
   * Resolve template variables in the given path.
   * Uses current scene context for variable resolution.
   */
  void resolve_template_variables(char *path, size_t path_maxlen) const;

  /**
   * Check if current path is within a resolved template directory.
   * Returns true if navigation is within template bounds.
   */
  bool is_within_template_bounds(const char *template_path, const char *current_path) const;

  /**
   * Reconstruct template path when navigating within template directory.
   * Preserves template variables while allowing subdirectory navigation.
   */
  bool reconstruct_template_for_navigation(const char *original_template,
                                           const char *current_path,
                                           char *result,
                                           size_t result_maxlen) const;

  /**
   * Get the common path prefix between two paths.
   * Used to determine navigation direction and scope.
   */
  int get_common_path_length(const char *path1, const char *path2) const;

  /**
   * Extract relative path difference between base and target paths.
   * Used for appending navigation changes to template paths.
   */
  void get_path_difference(const char *base_path,
                          const char *target_path,
                          char *result,
                          size_t result_maxlen) const;

 public:
  PathTemplateHandler();

  /**
   * Initialize handler with current FileSelectParams state.
   * Sets up internal tracking for template and resolved paths.
   */
  void initialize_from_params(const FileSelectParams *params);

  /**
   * Handle user input of a new path (potentially with templates).
   * Updates all internal state and determines navigation target.
   * 
   * \param input_path: New path entered by user (may contain templates)
   * \return: Resolved path to navigate to
   */
  StringRefNull handle_user_input(const char *input_path);

  /**
   * Handle navigation change from file browser.
   * Attempts to preserve template variables when navigating within template bounds.
   * 
   * \param new_directory: Directory navigated to by file browser
   * \return: True if template state was preserved, false if templates were cleared
   */
  bool handle_navigation_change(const char *new_directory);

  /**
   * Update FileSelectParams with current handler state.
   * Synchronizes dir, dir_variable, and dir_preview fields.
   */
  void update_params(FileSelectParams *params) const;

  /**
   * Get current template path (unresolved).
   */
  StringRefNull get_template_path() const { return template_path_; }

  /**
   * Get current resolved path (for navigation).
   */
  StringRefNull get_resolved_path() const { return resolved_path_; }

  /**
   * Get current preview path (resolved template display).
   */
  StringRefNull get_preview_path() const { return preview_path_; }

  /**
   * Check if current state contains template variables.
   */
  bool has_template_variables() const;

  /**
   * Reset handler to non-template state with given path.
   */
  void reset_to_simple_path(const char *path);

  /**
   * Debug function to print current handler state.
   */
  void debug_print_state(const char *context) const;
};

}  // namespace blender::editor::file