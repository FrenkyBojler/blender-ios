/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 * \brief Path template handling utilities for file browser.
 */

#pragma once

#include <cstddef>

struct bContext;
struct FileSelectParams;
struct PointerRNA;
struct PropertyRNA;
struct wmOperator;

namespace blender::editor::file {

/**
 * Handle user input of a path (potentially with templates) and update FileSelectParams.
 * This is a utility function that can be called from RNA setters and UI callbacks.
 * 
 * \param params: FileSelectParams to update
 * \param input_path: New path entered by user (may contain templates)
 */
void handle_template_path_input(FileSelectParams *params, const char *input_path);

/**
 * Handle navigation change and attempt to preserve template variables.
 * This is called when the file browser navigates to a new directory.
 * 
 * \param params: FileSelectParams to update
 * \param new_directory: Directory navigated to by file browser
 */
void handle_template_navigation(FileSelectParams *params, const char *new_directory);

/**
 * Initialize template paths in FileSelectParams.
 * Sets up dir_variable and dir_preview based on current dir.
 * 
 * \param params: FileSelectParams to initialize
 */
void initialize_template_paths(FileSelectParams *params);

/**
 * Resolve template variables in a path string.
 * Utility function for direct template resolution.
 * 
 * \param path: Path string to resolve (modified in place)
 * \param path_maxlen: Maximum length of path buffer
 */
void resolve_path_templates(char *path, size_t path_maxlen);

/**
 * Check if params should use template path (has non-empty dir_variable with template syntax).
 * Consolidates common conditional pattern used throughout the codebase.
 */
bool should_use_template_path(const FileSelectParams *params);

/**
 * Helper function to set RNA string property with update notification if changed.
 * Consolidates common pattern used throughout file operations.
 * 
 * \param C: Blender context for property updates
 * \param op: Operator pointer to set property on  
 * \param prop_name: Name of the property to set
 * \param new_value: New string value to set
 */
void set_operator_string_property(bContext *C, wmOperator *op, const char *prop_name, const char *new_value);

}  // namespace blender::editor::file