/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_attribute_enums.hh"
#include "BKE_attribute_filters.hh"
#include "BKE_attribute_math.hh"
#include "BKE_deform.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"

#include "BLI_array_utils.hh"
#include "BLI_index_mask.hh"
#include "BLI_offset_indices.hh"
#include "BLI_task.hh"
#include "FN_field_evaluation.hh"
#include "GEO_mesh_replace_faces.hh"

namespace blender::geometry {

Mesh *replace_faces(const Mesh &base,
                    const fn::Field<bool> &selection_field,
                    const fn::Field<int> &indices_field,
                    const fn::Field<float> &height,
                    const Span<const Mesh *> meshes)
{
  const bke::MeshFieldContext field_context(base, bke::AttrDomain::Face);
  fn::FieldEvaluator field_evaluator(field_context, base.faces_num);
  field_evaluator.set_selection(selection_field);
  field_evaluator.add(indices_field);
  field_evaluator.evaluate();
  IndexMask selection = field_evaluator.get_evaluated_selection_as_mask();
  const VArraySpan<int> indices = field_evaluator.get_evaluated<int>(0);
  IndexMaskMemory memory;
  selection = array_utils::indices_in_range(selection, indices, meshes.index_range(), memory);
  const IndexMask unselected = selection.complement(IndexMask(base.faces_num), memory);

  Array<int> mesh_vert_nums(meshes.size());
  for (const int i : meshes.index_range()) {
    mesh_vert_nums[i] = meshes[i]->verts_num;
  }

  Array<int> verts_num_per_face(base.faces_num);
  array_utils::gather<int>(mesh_vert_nums, indices, selection, verts_num_per_face);
  // offset_indices::copy
  const std::optional<OffsetIndices<int>> face_vert_offsets =
      offset_indices::accumulate_counts_to_offsets_with_overflow_check(verts_num_per_face);
  if (!face_vert_offsets) {
    return BKE_mesh_copy_for_eval(base);
  }

  Mesh *result = BKE_mesh_new_nomain(face_vert_offsets->total_size(), 0, 0, 0);  // TODO

  // EXISTING LOGIC BORKED

  // Build the position of every single vertex on the new face meshes. For quads, it might be best
  // to do this with a transform per face. The transform should move 0,0,0 to the first corner of
  // the base face, and 1,1,0 to the third corner. Alternatively we coudl just always do quad
  // interpolation of the mesh vertices based on their location in the XY space. The height should
  // come from multiplying the mesh vertex Z position with a height input for the each selected
  // face. For N-gons, we can use the triangulation and do a UV style interpolation from the base
  // face location in the same triangulation of a standard N-gon shape to the new positions. The
  // height mixing would be the same.

  // With a disjoint set of all the vertices, including original vertices. The set will have to be
  // sized to include unselected original vertices too. For each face, find the neighboring base
  // face or new face meshes. For each neighbor edge, build a kdtree of the vertices in the face
  // and the neighboring vertices. Merge the vertices of neighbors within the threshold. We only
  // ever want to merge vertices of boundary edges with existing vertices of the base mesh or
  // boundary vertices of neighboring mesh parts; we should never merge vertices within a part.
  // This implies only the vertices that are candidates for merging should be added to the KDtree.

  // For building edges, it's crucial we don't use a VectorSet for the entire result mesh's edges.
  // Ideally we'd avoid nested containers with bad allocation patterns as well. We need to make
  // sure we don't duplicate an existing mesh edge, but we can do that by searching through the
  // neighboring edges of the base vertices we're connecting to, rather than building a full
  // VectorSet. Also we never need to deduplicate edges that are added in each part, because we
  // never merged vertices within a part.

  // Building faces is simple, we just copy over the faces from the part meshes, remapping the
  // vertex indices and setting the indices for the newly created edges or existing edges.

  // Attribute merging. When there is only a single input mesh this should end up as a single or
  // multiple calls to attribute_math::gather, i.e. just an index-based copy. That should also be
  // the case for attributes that only exist on one of the input meshes (i.e. no mixing is
  // necessary). For attributes that exist on multiple input meshes (including the base mesh), it
  // gets more complicated.

  return result;
}

}  // namespace blender::geometry
