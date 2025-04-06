/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "BKE_curves.hh"

/**
 * This header file contains a C++ interface to the 2D boolean clipping, using a modified version
 * of the Greiner-Hormann clipping algorithm.
 */

namespace blender::geometry::boolean {

enum class Operation : int8_t {
  /* Intersection of the Subject and the Clipping. */
  Intersect,
  /* Union of Subject and Clipping. */
  Union,
  /* Differences of Subject with Clipping. */
  Difference,
};

enum class FillRule : int8_t {
  /* Treat odd winding order as fill. */
  EvenOdd,
  /* Treat non zero winding order as fill. */
  NonZero,
  /* Treat non zero winding order as fill but without holes. */
  NoHoles,
};

struct CurveBooleanOpParameters {
  Operation boolean_mode;

  FillRule subject_rule;
  FillRule clipping_rule;
  FillRule output_rule;
};

bke::CurvesGeometry curve_boolean(const CurveBooleanOpParameters op_params,
                                  const bke::CurvesGeometry &curves,
                                  const IndexMask &mask_shapes,
                                  const IndexMask &clipping_shapes);

}  // namespace blender::geometry::boolean
