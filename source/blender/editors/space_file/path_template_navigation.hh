/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 * \brief Path template handling for file browser - preserves template variables during navigation.
 */

#pragma once

#include <cstddef>

struct bContext;
struct FileSelectParams;
struct PointerRNA;
struct PropertyRNA;
struct wmOperator;

namespace blender::editor::file::path_templates {

/**
 * Handle user input of a path (potentially with template variables).
 *
 * Processes user-entered paths that may contain template syntax (e.g., `//render/<frame>/`).
 * Resolves template variables and updates all path fields in FileSelectParams accordingly.
 *
 * \param params: FileSelectParams structure to update with new path information.
 * \param input_path: User-entered path string (may contain template variable syntax).
 */
void handle_input(FileSelectParams *params, const char *input_path);

/**
 * Handle directory navigation while preserving template variables.
 *
 * Called when the file browser navigates to a new directory. Attempts to maintain template
 * variable syntax when navigating within template-based directory structures.
 *
 * \param params: FileSelectParams structure to update with new navigation state.
 * \param new_directory: Target directory path after navigation.
 */
void handle_navigation(FileSelectParams *params, const char *new_directory);

/**
 * Initialize path template fields in FileSelectParams.
 *
 * Sets up `dir_template` and `dir_resolved` fields based on the current directory.
 * Should be called when initializing file browser state.
 *
 * \param params: FileSelectParams structure to initialize.
 */
void initialize(FileSelectParams *params);

/**
 * Check if FileSelectParams contains an active template path.
 *
 * \return true if `dir_template` is non-empty and contains template variable syntax.
 */
bool has_template(const FileSelectParams *params);

/**
 * Set an RNA string property on an operator with update notification.
 *
 * Convenience function that sets a string property and triggers RNA update
 * callbacks if the value changed.
 *
 * \param C: Blender context for property update notifications.
 * \param op: Operator whose property should be modified.
 * \param prop_name: Name of the string property to set.
 * \param new_value: New string value for the property.
 */
void set_operator_string_property(bContext *C,
                                  wmOperator *op,
                                  const char *prop_name,
                                  const char *new_value);

}  // namespace blender::editor::file::path_templates