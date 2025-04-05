/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Brush-specific methods for calculating generic operations.
 *
 * E.g. Custom strength adjustments, index mask calculations, etc...
 */

#pragma once

#include <optional>

#include "BLI_index_mask.hh"
#include "BLI_math_vector.hh"

struct Brush;
struct Depsgraph;
struct Object;

namespace blender::ed::sculpt_paint {

/** Represents the result of one or more bvh queries to find a brush's affected nodes. */
struct NodeMaskResult {
  IndexMask node_mask;

  /* For planar brushes, the plane center and normal are calculated based on the original cursor
   * position and needed for further calculations when performing brush strokes.
   */
  std::optional<float3> plane_center;
  std::optional<float3> plane_normal;
};

namespace brushes::plane {
NodeMaskResult calc_node_mask(const Depsgraph &depsgraph,
                              Object &ob,
                              const Brush &brush,
                              IndexMaskMemory &memory);
}
}
