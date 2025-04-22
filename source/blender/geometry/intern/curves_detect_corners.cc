/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"

#include "GEO_curves_detect_corners.hh"

extern "C" {
#include "curve_fit_nd.h"
}

namespace blender::geometry {

Array<bool> curves_detect_corners(const bke::CurvesGeometry &curves,
                                  const VArray<float> &angles_min,
                                  const VArray<float> &radii_min,
                                  const VArray<float> &radii_max,
                                  const VArray<int> &samples_max)
{
  Array<bool> corner_selection(curves.points_num(), false);

  IndexMaskMemory poly_curves_memory;
  const IndexMask poly_curves = curves.indices_for_curve_type(CURVE_TYPE_POLY, poly_curves_memory);

  const Span<float3> positions = curves.positions();
  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  poly_curves.foreach_index(GrainSize(1024), [&](const int64_t curve) {
    const IndexRange points = points_by_curve[curve];
    const Span<float3> curve_positions = positions.slice(points);
    const float radius_min = radii_min[curve];
    const float radius_max = radii_max[curve];

    const int curve_samples_max = samples_max[curve];
    const float angle_threshold = angles_min[curve];

    uint32_t *r_corners;
    uint32_t r_corners_len;

    const int error = curve_fit_corners_detect_fl(curve_positions.cast<float>().data(),
                                                  curve_positions.size(),
                                                  3,
                                                  radius_min,
                                                  radius_max,
                                                  curve_samples_max,
                                                  angle_threshold,
                                                  &r_corners,
                                                  &r_corners_len);
    if (error) {
      /* Some error occured. Couldn't detect corners. */
      return;
    }

    const Span<int> corner_indices(reinterpret_cast<int *>(r_corners), r_corners_len);

    IndexMaskMemory corner_memory;
    const IndexMask corner_mask = IndexMask::from_indices(corner_indices, corner_memory);
    corner_mask.to_bools(corner_selection.as_mutable_span().slice(points));
  });

  return corner_selection;
}

}  // namespace blender::geometry
