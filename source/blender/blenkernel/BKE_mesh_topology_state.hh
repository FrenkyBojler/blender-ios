/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_implicit_sharing_ptr.hh"

struct Mesh;

namespace blender::bke {

/**
 * Simplifies checking if the topology of a mesh before and after an operation is the same.
 *
 * It does so by adding an owner to the mesh topology attributes, which requires them to be
 * re-allocated for modifications. This allows checking for changes in constant time by simply
 * comparing the sharing info.
 */
class MeshTopologyState {
 private:
  ImplicitSharingPtr<> edge_verts_;
  ImplicitSharingPtr<> corner_verts_;
  ImplicitSharingPtr<> corner_edges_;
  ImplicitSharingPtr<> face_offset_indices_;

 public:
  MeshTopologyState(const Mesh &mesh);

  /**
   * True when the topology of the given mesh is the same as the topology of the mesh the
   * constructor was called with.
   */
  bool same_topology_as(const Mesh &mesh) const;
};

}  // namespace blender::bke
