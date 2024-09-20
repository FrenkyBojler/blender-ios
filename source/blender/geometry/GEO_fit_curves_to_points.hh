/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_offset_indices.hh"
#include "BLI_span.hh"

#include "BKE_curves.hh"

namespace blender::geometry {

bke::CurvesGeometry fit_curves_to_points(Span<float3> positions,
                                         OffsetIndices<int> points_by_curve,
                                         Span<bool> cyclic,
                                         float epsilon,
                                         Array<int> &r_new_to_old_map);

}  // namespace blender::geometry
