/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

/**
 * The struct index representing type-less bytes buffers.
 *
 * Although code has historically (pre-4.3) be fairly unreliable (logically incorrect, see inline
 * code-comments for #DNA_struct_get_compareflags regarding this), most of read/write blend-file
 * code would assume that the `0` value was raw data, so keep it at this value.
 */
#define SDNA_RAW_DATA_STRUCT_INDEX 0

namespace blender::dna {

/**
 * Each DNA struct has an integer identifier which is unique within a specific Blender build, but
 * not necessarily across different builds. The identifier can be used to index into
 * `SDNA.structs`.
 */
template<typename T> int sdna_struct_id_get();

/**
 * The maximum identifier that will be returned by #sdna_struct_id_get in this Blender build.
 */
int sdna_struct_id_get_max();

}  // namespace blender::dna
