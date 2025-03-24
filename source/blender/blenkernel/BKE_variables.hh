/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 *
 * \brief Functions and classes for working with variables (Blender variables,
 * Project variables, etc.).
 */
#pragma once

#include <cmath>
#include <optional>

#include "BLI_map.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_string_utils.hh"

#include "DNA_scene_types.h"

/* -------------------------------------------------------------------- */
/** \name Blender Variables
 * \{ */

struct PathVariables {
  blender::Map<std::string, std::string> strings;
  blender::Map<std::string, int64_t> integers;
  blender::Map<std::string, double> floats;
};

/**
 * Build path variables based on available information.
 *
 * All parameters are allowed to be null, in which case the variables derived
 * from those parameters will simply not be included.
 *
 * This is generally used to create the variables passed to
 * `BKE_path_apply_variables()`.
 *
 * Note: this does not and *shouldn't* include adding a variable for the
 * absolute path to the current blend file. That is handled by `BLI_path_abs()`
 * (with the special "//" syntax), which is called in specific ways for e.g.
 * cache paths such that corner cases are handled properly. We specifically
 * avoid that here, since the use-case is already addressed and it would be easy
 * to mess up the specifics.
 *
 *
 * \param blend_file_path: full path to the blend file, including the file name
 * (a directory-only path--ending with a slash--will also be accepted, but then
 * no "file_name" variable will be created). Typically you should fetch this
 * with `ID_BLEND_PATH()`, but there are plenty of exceptions. Note that this
 * should be the blend file that the path you're going to generate with the
 * variables "belongs" to.
 *
 * \param frame_number: the current frame.
 *
 * \param render_data: start/end frame, output resolution, and fps. Note: the
 * current frame in this is *not* used. Use the `frame_number` parameter for
 * that.
 *
 *
 * \see BKE_path_apply_variables()
 *
 * \see BLI_path_abs()
 */
PathVariables BKE_build_blender_variables(const char *blend_file_path,
                                       std::optional<uint64_t> frame_number,
                                       const RenderData *render_data);

bool BKE_path_apply_variables(char path[FILE_MAX], const PathVariables &variables);
