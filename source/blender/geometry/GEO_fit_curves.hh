/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_curves.hh"

namespace blender::geometry {

enum class FitMethod { Refit, Split };

bke::CurvesGeometry fit_curves(Span<float3> positions,
                               OffsetIndices<int> src_offsets,
                               const IndexMask &curve_selection,
                               const VArray<float> &thresholds,
                               const VArray<bool> &cyclic,
                               FitMethod method,
                               Array<int> &r_old_to_new_map);
bke::CurvesGeometry fit_curves(const bke::CurvesGeometry &src_curves,
                               const IndexMask &curve_selection,
                               const VArray<float> &thresholds,
                               FitMethod method,
                               const bke::AttributeFilter &attribute_filter);

}  // namespace blender::geometry
