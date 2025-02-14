/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh_topology_state.hh"
#include "BKE_mesh_types.hh"

namespace blender::bke {

static ImplicitSharingPtr<> attribute_to_sharing_ptr(const GAttributeReader &attr)
{
  if (!attr) {
    return {};
  }
  attr.sharing_info->add_user();
  return ImplicitSharingPtr<>(attr.sharing_info);
}

static bool attribute_matches_state(const ImplicitSharingPtr<> &array_state,
                                    const GAttributeReader &attr)
{
  return array_state.get() == attr.sharing_info;
}

MeshTopologyState::MeshTopologyState(const Mesh &mesh)
{
  const AttributeAccessor attributes = mesh.attributes();
  edge_verts_ = attribute_to_sharing_ptr(attributes.lookup(".edge_verts"));
  corner_verts_ = attribute_to_sharing_ptr(attributes.lookup(".corner_vert"));
  corner_edges_ = attribute_to_sharing_ptr(attributes.lookup(".corner_edge"));
  if (mesh.runtime->face_offsets_sharing_info) {
    mesh.runtime->face_offsets_sharing_info->add_user();
    face_offset_indices_ = ImplicitSharingPtr<>(mesh.runtime->face_offsets_sharing_info);
  }
}

bool MeshTopologyState::same_topology_as(const Mesh &mesh) const
{
  const AttributeAccessor attributes = mesh.attributes();
  if (!attribute_matches_state(edge_verts_, attributes.lookup(".edge_verts"))) {
    return false;
  }
  if (!attribute_matches_state(corner_verts_, attributes.lookup(".corner_vert"))) {
    return false;
  }
  if (!attribute_matches_state(corner_edges_, attributes.lookup(".corner_edge"))) {
    return false;
  }
  if (face_offset_indices_.get() != mesh.runtime->face_offsets_sharing_info) {
    return false;
  }
  return true;
}

}  // namespace blender::bke
