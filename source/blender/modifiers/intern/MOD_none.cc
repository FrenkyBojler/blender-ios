/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include "MEM_guardedalloc.h"

#include "MOD_modifiertypes.hh"

#include "UI_resources.hh"

#include "RNA_prototypes.hh"

namespace blender {

/* We only need to define is_disabled; because it always returns 1,
 * no other functions will be called
 */

static ModifierData *new_data()
{
  return MEM_new<ModifierData>("ModifierData");
}

static bool is_disabled(const Scene * /*scene*/, ModifierData * /*md*/, bool /*use_render_params*/)
{
  return true;
}

ModifierTypeInfo modifierType_None = {
    /*idname*/ "None",
    /*name*/ "None",
    /*struct_name*/ "ModifierData",
    /*struct_size*/ sizeof(ModifierData),
    /*srna*/ &RNA_Modifier,
    /*type*/ ModifierTypeType::None,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_AcceptsCVs,
    /*icon*/ ICON_NONE,

    /*copy_data*/ modifier_copy_data<ModifierData>,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ nullptr,
    /*modify_geometry_set*/ nullptr,

    /*new_data*/ new_data,
    /*required_data_mask*/ nullptr,
    /*free_data*/ modifier_free_data<ModifierData>,
    /*is_disabled*/ is_disabled,
    /*update_depsgraph*/ nullptr,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ nullptr,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ nullptr,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
