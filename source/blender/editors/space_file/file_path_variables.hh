/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 * \brief Path variable handling for file browser - supports template syntax in paths.
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
 * Handle user input of a path (potentially with template variables) and update FileSelectParams.
 * This is a utility function that can be called from RNA setters and UI callbacks.
 * 
 * \param params: FileSelectParams to update
 * \param input_path: New path entered by user (may contain template variables)
 */
void handle_path_input(FileSelectParams *params, const char *input_path);

/**
 * Handle navigation change and attempt to preserve template variables.
 * This is called when the file browser navigates to a new directory.
 * 
 * \param params: FileSelectParams to update
 * \param new_directory: Directory navigated to by file browser
 */
void handle_navigation(FileSelectParams *params, const char *new_directory);

/**
 * Initialize path fields in FileSelectParams.
 * Sets up dir_template and dir_resolved based on current dir.
 * 
 * \param params: FileSelectParams to initialize
 */
void initialize_path_fields(FileSelectParams *params);

/**
 * Check if params should use template path (has non-empty dir_template with template syntax).
 * Consolidates common conditional pattern used throughout the codebase.
 */
bool has_template_path(const FileSelectParams *params);

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