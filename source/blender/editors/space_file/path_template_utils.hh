/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 * \brief Path template handling utilities for file browser.
 */

#pragma once

struct FileSelectParams;

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

}  // namespace blender::editor::file