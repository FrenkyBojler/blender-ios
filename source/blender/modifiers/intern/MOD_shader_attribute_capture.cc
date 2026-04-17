/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include "BLI_math_matrix.h"

#include "BLT_translation.hh"

#include "DNA_key_types.h"
#include "DNA_object_types.h"

#include "BKE_key.hh"

#include "RNA_prototypes.hh"

#include "MOD_modifiertypes.hh"

#include "UI_resources.hh"

namespace blender::mod_shader_attribute_capture {

static void modify_geometry_set(ModifierData *md,
                                const ModifierEvalContext *ctx,
                                bke::GeometrySet *geometry_set)
{
  printf("%s;\n", AT);
}

}

namespace blender {

ModifierTypeInfo modifierType_CaptureShaderAttribute = {
    /*idname*/ "AttributeCapture",
    /*name*/ N_("AttributeCapture"),
    /*struct_name*/ "AttributeCaptureModifierData",
    /*struct_size*/ sizeof(AttributeCaptureModifierData),
    /*srna*/ &RNA_Modifier,
    /*type*/ ModifierTypeType::NonGeometrical,
    /*flags*/ eModifierTypeFlag_AcceptsCVs | eModifierTypeFlag_AcceptsVertexCosOnly |
        eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_DOT,

    /*copy_data*/ nullptr,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ nullptr,
    /*modify_geometry_set*/ mod_shader_attribute_capture::modify_geometry_set,

    /*init_data*/ nullptr,
    /*required_data_mask*/ nullptr,
    /*free_data*/ nullptr,
    /*is_disabled*/ nullptr,
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

}  // namespace blender::mod_shader_attribute_capture
