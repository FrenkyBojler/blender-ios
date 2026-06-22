/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_ID.h"
#include "DNA_object_types.h"

#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"

#include "DEG_depsgraph_query.hh"

#include "IO_mesh_utils.hh"

namespace blender::io {

const Mesh *mesh_coerce_for_export_begin(MeshCoerceForExport &coerce,
                                         Depsgraph *depsgraph,
                                         Object *obj_eval,
                                         const bool apply_modifiers)
{
  /* Curves and NURBS surfaces have no mesh in their pre-modified state,
   * convert one on demand. */
  bool is_original_mesh_type = true;
  if (const ID *data_orig = obj_eval->runtime->data_orig) {
    if (GS(data_orig->name) != ID_ME) {
      is_original_mesh_type = false;
      if (!apply_modifiers) {
        coerce.owned = BKE_mesh_new_from_object(
            depsgraph, DEG_get_original(obj_eval), true, true, true);
      }
    }
  }
  coerce.mesh = apply_modifiers       ? BKE_object_get_evaluated_mesh(obj_eval) :
                is_original_mesh_type ? BKE_object_get_pre_modified_mesh(obj_eval) :
                                        coerce.owned;
  return coerce.mesh;
}

void mesh_coerce_for_export_end(MeshCoerceForExport &coerce)
{
  if (coerce.owned) {
    BKE_id_free(nullptr, coerce.owned);
    coerce.owned = nullptr;
  }
}

}  // namespace blender::io
