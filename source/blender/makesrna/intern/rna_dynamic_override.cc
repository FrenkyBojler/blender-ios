/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <cstdlib>

#include "DNA_dynamic_override_types.h"

#include "BKE_icons.hh"

#include "RNA_define.hh"

#include "rna_internal.hh"

#ifdef RNA_RUNTIME

#  include "DNA_object_types.h"
#  include "DNA_vfont_types.h"

#  include "BKE_library.hh"
#  include "BKE_vfont.hh"

#  include "DEG_depsgraph.hh"

#  include "WM_api.hh"

namespace blender {

/* Matching function in rna_ID.cc */
static int rna_VectorFont_filepath_editable(const PointerRNA *ptr, const char ** /*r_info*/)
{
  VFont *vfont = id_cast<VFont *>(ptr->owner_id);
  if (BKE_vfont_is_builtin(vfont)) {
    return 0;
  }
  return PROP_EDITABLE;
}

static void rna_VectorFont_reload_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  VFont *vf = id_cast<VFont *>(ptr->owner_id);
  BKE_vfont_data_free(vf);

  /* update */
  WM_main_add_notifier(NC_GEOM | ND_DATA, nullptr);
  DEG_id_tag_update(&vf->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
}

}  // namespace blender

#else

namespace blender {

static void rna_def_dynamic_override(BlenderRNA *brna)
{
  StructRNA *srna;
  // PropertyRNA *prop;

  srna = RNA_def_struct(brna, "DynamicOverride", "ID");
  RNA_def_struct_ui_text(
      srna, "Dynamic Override", "Set of rules overriding values of data at evaluation time");
  RNA_def_struct_sdna(srna, "DynamicOverride");
  RNA_def_struct_ui_icon(srna, ICON_DATA_ID);
}

void RNA_def_dynamic_override(BlenderRNA *brna)
{
  rna_def_dynamic_override(brna);
}

}  // namespace blender

#endif
