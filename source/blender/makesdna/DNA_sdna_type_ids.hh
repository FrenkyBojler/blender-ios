/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

namespace blender::dna {

/**
 * Each DNA struct has an integer identifier which is unique within a specific Blender build, but
 * not necessarily across different builds. The identifier can be used to index into
 * `SDNA.structs`.
 */
template<typename T> int get_sdna_struct_id();

/**
 * The maximum identifier that will be returned by #get_sdna_struct_id in this Blender build.
 */
int get_sdna_struct_id_max();

}  // namespace blender::dna
