/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <cstdlib>

#include "RNA_define.hh"

#include "rna_internal.hh"

#include "WM_types.hh"

#ifdef RNA_RUNTIME

#  include "DNA_curve_types.h"
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

static void rna_VectorFont_overlap_removal_update(Main *bmain, Scene * /*scene*/, PointerRNA *ptr)
{
  VFont *vf = id_cast<VFont *>(ptr->owner_id);
  BKE_vfont_data_free(vf);

  /* update */
  WM_main_add_notifier(NC_GEOM | ND_DATA, nullptr);
  DEG_id_tag_update(&vf->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);

  /* FIXME: blender should already refresh curves in the viewport when the depsgraph is tagged
   * but it doesn't. */
  for (Curve &cu : bmain->curves) {
    if (cu.ob_type != OB_FONT) {
      continue;
    }
    if (!ELEM(vf, cu.vfont, cu.vfontb, cu.vfonti, cu.vfontbi)) {
      continue;
    }
    DEG_id_tag_update(&cu.id, ID_RECALC_GEOMETRY);
  }

  /*  */
}

}  // namespace blender

#else

namespace blender {

void RNA_def_vfont(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "VectorFont", "ID");
  RNA_def_struct_ui_text(srna, "Vector Font", "Vector font for Text objects");
  RNA_def_struct_sdna(srna, "VFont");
  RNA_def_struct_ui_icon(srna, ICON_FILE_FONT);

  prop = RNA_def_property(srna, "filepath", PROP_STRING, PROP_FILEPATH);
  RNA_def_property_string_sdna(prop, nullptr, "filepath");
  RNA_def_property_flag(prop, PROP_PATH_SUPPORTS_BLEND_RELATIVE);
  RNA_def_property_editable_func(prop, "rna_VectorFont_filepath_editable");
  RNA_def_property_ui_text(prop, "File Path", "");
  RNA_def_property_update(prop, NC_GEOM | ND_DATA, "rna_VectorFont_reload_update");

  prop = RNA_def_property(srna, "packed_file", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "packedfile");
  RNA_def_property_ui_text(prop, "Packed File", "");

  prop = RNA_def_property(srna, "use_overlap_removal", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "use_overlap_removal", 0);
  RNA_def_property_ui_text(
      prop, "Remove Overlaps", "Remove overlapping regions from glyph curves when loading");
  RNA_def_property_update(prop, NC_GEOM | ND_DATA, "rna_VectorFont_overlap_removal_update");

  RNA_api_vfont(srna);
}

}  // namespace blender

#endif
