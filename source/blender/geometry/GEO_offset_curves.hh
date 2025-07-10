/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_curves.hh"

#include "BLI_index_mask.hh"
#include "BLI_span.hh"

namespace blender::geometry {

bke::CurvesGeometry offset_curves(const bke::CurvesGeometry &src_curves,
                                  Span<float3> normals,
                                  const IndexMask &curve_selection,
                                  float offset_distance,
                                  float miter_angle);

}  // namespace blender::geometry
