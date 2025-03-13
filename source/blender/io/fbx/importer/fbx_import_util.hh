/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#pragma once

#include "BLI_map.hh"
#include "BLI_set.hh"

#include "ufbx.h"

struct Object;
struct Key;
struct Material;

namespace blender::io::fbx {

const char *get_fbx_name(const ufbx_string &name, const char *def = "Untitled");

struct FbxElementMapping {
  Map<const ufbx_element *, Object *> el_to_object;
  Map<const ufbx_element *, Key *> el_to_shape_key;
  Map<const ufbx_material *, Material *> mat_to_material;
  Map<const ufbx_node *, Object *> bone_to_armature;
  /** Bone node to "bind matrix", i.e. matrix that transforms from bone (in skin bind pose) local
   * space to world space. */
  Map<const ufbx_node *, ufbx_matrix> bone_to_bind_matrix;
  /** Which bone actually have pose or skin cluster bind matrices in the FBX file (the others
   * would just use their world transform). */
  Set<const ufbx_node *> bone_has_pose_or_skin_matrix;
  Set<const Object *> armatures_created_at_root;
  ufbx_matrix global_conv_matrix;

  //@TODO: these could be precalculated once
  ufbx_matrix calc_local_bind_matrix(const ufbx_node *bone_node,
                                     const ufbx_matrix &world_to_arm,
                                     bool &r_found) const
  {
    r_found = false;
    const ufbx_matrix *bind_mtx = this->bone_to_bind_matrix.lookup_ptr(bone_node);
    if (bind_mtx == nullptr) {
      return ufbx_identity_matrix;
    }
    r_found = true;
    ufbx_matrix res = *bind_mtx;

    const ufbx_matrix *parent_mtx = nullptr;
    if (bone_node->parent != nullptr) {
      parent_mtx = this->bone_to_bind_matrix.lookup_ptr(bone_node->parent);
    }

    ufbx_matrix parent_inv_mtx;
    if (parent_mtx) {
      parent_inv_mtx = ufbx_matrix_invert(parent_mtx);
    }
    else {
      parent_inv_mtx = world_to_arm;
    }
    res = ufbx_matrix_mul(&parent_inv_mtx, &res);
    return res;
  }
};

}  // namespace blender::io::fbx
