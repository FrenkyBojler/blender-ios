/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_span.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_math_vector.hh"

namespace blender::geometry::bounding {

/**
 * Ritter's bounding sphere algorithm.
 **/
std::pair<float3, float> min_packing_sphere(const Span<float3> points);

std::pair<float3, float> concatenate_spheres(float3 a_centre, float a_radius, float3 b_centre, float b_radius);

void joints_packing_spheres(OffsetIndices<int> buckets_offsets,
                            int total_depth,
                            Span<float3> src_bucket_points,
                            MutableSpan<float3> dst_joints_centre,
                            MutableSpan<float> dst_joints_radii);

void joints_packing_spheres_fast(OffsetIndices<int> buckets_offsets,
                                 int total_depth,
                                 Span<float3> src_bucket_points,
                                 MutableSpan<float3> dst_joints_centre,
                                 MutableSpan<float> dst_joints_radii);

}  // namespace blender::geometry::bounding
