/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include <fmt/format.h>

#include "DNA_anim_types.h"
#include "DNA_brush_types.h"
#include "DNA_defaults.h"
#include "DNA_light_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_force_types.h"
#include "DNA_sequence_types.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"
#include "BLI_sys_types.h"

#include "BKE_anim_data.hh"
#include "BKE_animsys.h"
#include "BKE_armature.hh"
#include "BKE_fcurve.hh"
#include "BKE_main.hh"
#include "BKE_mesh_legacy_convert.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint.hh"

#include "SEQ_iterator.hh"
#include "SEQ_sequencer.hh"

#include "ANIM_action.hh"
#include "ANIM_action_iterators.hh"
#include "ANIM_armature_iter.hh"

#include "RNA_access.hh"

#include "readfile.hh"

#include "versioning_common.hh"

void blo_do_versions_500(FileData * /*fd*/, Library * /*lib*/, Main *bmain)
{

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 500, 0)) {
    LISTBASE_FOREACH (Mesh *, mesh, &bmain->meshes) {
      blender::bke::mesh_sculpt_mask_to_generic(*mesh);
      blender::bke::mesh_custom_normals_to_generic(*mesh);
      rename_mesh_uv_seam_attribute(*mesh);
    }
  }

  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}
