/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "BLI_math_vector_types.hh"
#include "BLI_virtual_array.hh"

#include "BKE_attribute_filter.hh"

namespace blender {

struct Mesh;
namespace fn {
template<typename T> class Field;
}
namespace bke {
enum class AttrDomain : int8_t;
}  // namespace bke

namespace geometry {

std::optional<Mesh *> mesh_copy_selection(const Mesh &src_mesh,
                                          const VArray<bool> &selection,
                                          bke::AttrDomain selection_domain,
                                          const bke::AttributeFilter &attribute_filter = {});

std::optional<Mesh *> mesh_copy_selection_keep_verts(
    const Mesh &src_mesh,
    const VArray<bool> &selection,
    bke::AttrDomain selection_domain,
    const bke::AttributeFilter &attribute_filter = {});

std::optional<Mesh *> mesh_copy_selection_keep_edges(
    const Mesh &mesh,
    const VArray<bool> &selection,
    bke::AttrDomain selection_domain,
    const bke::AttributeFilter &attribute_filter = {});

void mesh_gather_elements_and_remap_verts(OffsetIndices<int> src_faces,
                                          OffsetIndices<int> dst_faces,
                                          Span<int> vert_map,
                                          const IndexMask &edge_mask,
                                          const IndexMask &face_mask,
                                          Span<int2> src_edges,
                                          Span<int> src_corner_verts,
                                          MutableSpan<int2> dst_edges,
                                          MutableSpan<int> dst_corner_verts);

void mesh_gather_elements_and_remap_edges(OffsetIndices<int> src_faces,
                                          OffsetIndices<int> dst_faces,
                                          Span<int> edge_map,
                                          const IndexMask &face_mask,
                                          Span<int> src_corner_edges,
                                          MutableSpan<int> dst_corner_edges);

}  // namespace geometry
}  // namespace blender
