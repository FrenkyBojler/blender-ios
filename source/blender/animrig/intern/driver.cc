/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 */

#include "ANIM_driver.hh"
#include "BKE_animsys.h"
#include "BKE_fcurve_driver.h"
#include "DNA_anim_types.h"
#include "RNA_access.hh"

namespace blender::animrig {

static bool path_resolved_create(PointerRNA *ptr,
                                 PropertyRNA *prop,
                                 const int prop_index,
                                 PathResolvedRNA *r_anim_rna)
{
  int array_len = RNA_property_array_length(ptr, prop);

  if ((array_len == 0) || (prop_index < array_len)) {
    r_anim_rna->ptr = *ptr;
    r_anim_rna->prop = prop;
    r_anim_rna->prop_index = array_len ? prop_index : -1;

    return true;
  }
  return false;
}

float evaluate_driver_from_rna_pointer(const AnimationEvalContext *anim_eval_context,
                                       PointerRNA *ptr,
                                       PropertyRNA *prop,
                                       const FCurve *fcu)
{
  PathResolvedRNA anim_rna;
  if (!path_resolved_create(ptr, prop, fcu->array_index, &anim_rna)) {
    return 0.0f;
  }
  return evaluate_driver(&anim_rna, fcu->driver, fcu->driver, anim_eval_context);
}

}  // namespace blender::animrig
