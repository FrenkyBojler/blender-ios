/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"

#include "BLI_span.hh"

struct Mesh;

namespace blender::geometry::boolean {

/**
 * Holds cumulative offsets for the given elements of a number
 * of concatenated Meshes. The sizes are one greater than the
 * number of meshes, so that the last value of each gives the
 * total number of elements.
 */
struct MeshOffsets : NonCopyable, NonMovable {
  Array<int> vert_start;
  Array<int> face_start;
  Array<int> edge_start;
  Array<int> corner_start;
  OffsetIndices<int> vert_offsets;
  OffsetIndices<int> face_offsets;
  OffsetIndices<int> edge_offsets;
  OffsetIndices<int> corner_offsets;

  MeshOffsets() = default;
  explicit MeshOffsets(Span<const Mesh *> meshes);
};

void interpolate_corner_attributes(bke::MutableAttributeAccessor output_attrs,
                                   bke::AttributeAccessor input_attrs,
                                   Mesh *output_mesh,
                                   const Mesh *input_mesh,
                                   Span<int> out_to_in_corner_map,
                                   Span<int> out_to_in_face_map);

void copy_attribute_using_map(const GSpan src, const Span<int> out_to_in_map, GMutableSpan dst);

/**
 * The \a dst span should be the material_index property of the result.
 * Rather than using the attribute from the joined mesh, we want to take
 * the original face and map it using \a material_remaps.
 */
void set_material_from_map(const Span<int> out_to_in_map,
                           const Span<Array<short>> material_remaps,
                           const Span<const Mesh *> meshes,
                           const MeshOffsets &mesh_offsets,
                           const MutableSpan<int> dst);

bke::GeometrySet join_meshes_with_transforms(const Span<const Mesh *> meshes,
                                             const Span<float4x4> transforms);

}  // namespace blender::geometry::boolean
