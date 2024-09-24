/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_offset_indices.hh"
#include "BLI_span.hh"

#include "BKE_curves.hh"

namespace blender::geometry {

enum class FitMethod { Refit, Split };

bke::CurvesGeometry fit_curves(Span<float3> positions,
                               OffsetIndices<int> src_offsets,
                               const IndexMask &curve_selection,
                               const VArray<bool> &cyclic,
                               const VArray<float> &thresholds,
                               FitMethod method,
                               Array<int> &r_old_to_new_map);

}  // namespace blender::geometry
