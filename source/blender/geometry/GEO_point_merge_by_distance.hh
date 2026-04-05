/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_index_mask.hh"

#include "BKE_attribute_filter.hh"

namespace blender {

struct PointCloud;

/** \file
 * \ingroup geo
 */

namespace geometry {

/**
 * Merge selected points into other selected points within the \a merge_distance. The merged
 * indices favor speed over accuracy, since the results will depend on the order of the points.
 */
PointCloud *point_merge_by_distance(const PointCloud &src_points,
                                    float merge_distance,
                                    const IndexMask &selection,
                                    const bke::AttributeFilter &attribute_filter);

/**
 * The same as above but merging roots can be provided explicitly.
 */
PointCloud *point_merge_indices(const PointCloud &src_points,
                                Span<int> root_indices,
                                int total_roots,
                                const bke::AttributeFilter &attribute_filter);

}  // namespace geometry
}  // namespace blender
