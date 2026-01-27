/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <optional>

#include "MEM_guardedalloc.h"

#include "BLI_build_config.h"
#include "BLI_enum_flags.hh"
#include "BLI_listbase.h"
#include "BLI_string.h"

#include "BLT_translation.hh"

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"
#include "DNA_dynamic_override_types.h"

#include "BKE_idprop.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "BLO_read_write.hh"

#include "CLG_log.h"

namespace blender {

namespace bke {

struct DynamicOverrideRuntime {};

}  // namespace bke

static void dynamic_override_rule_copy(DynamicOverrideRule *dynoverride_rule_dst,
                                       DynamicOverrideRule *dynoverride_rule_src,
                                       const int flag);

static void dynoverride_init_data(ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  dynoverride->runtime = MEM_new<bke::DynamicOverrideRuntime>(__func__);
}

static void dynoverride_copy_data(Main * /*bmain*/,
                                  std::optional<Library *> /*owner_library*/,
                                  ID *id_dst,
                                  const ID *id_src,
                                  const int flag)
{
  DynamicOverride *dynoverride_dst = id_cast<DynamicOverride *>(id_dst);
  const DynamicOverride *dynoverride_src = id_cast<const DynamicOverride *>(id_src);

  BLI_duplicatelist(&dynoverride_dst->rules, &dynoverride_src->rules);
  for (DynamicOverrideRule *
           dynoverride_rule_dst = static_cast<DynamicOverrideRule *>(dynoverride_dst->rules.first),
          *dynoverride_rule_src = static_cast<DynamicOverrideRule *>(dynoverride_src->rules.first);
       dynoverride_rule_dst;
       dynoverride_rule_dst = dynoverride_rule_dst->next,
          dynoverride_rule_src = dynoverride_rule_src->next)
  {
    dynamic_override_rule_copy(dynoverride_rule_dst, dynoverride_rule_src, flag);
  }

  dynoverride_dst->runtime = MEM_new<bke::DynamicOverrideRuntime>(__func__);
}

static void dynoverride_free_data(ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  MEM_delete(dynoverride->runtime);
}

static void dynoverride_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  /* Clean up, important in undo case to reduce false detection of changed datablocks. */
  dynoverride->runtime = nullptr;

  /* write LibData */
  writer->write_id_struct(id_address, dynoverride);
  BKE_id_blend_write(writer, &dynoverride->id);
}

static void dynoverride_blend_read_data(BlendDataReader * /*reader*/, ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  dynoverride->runtime = MEM_new<bke::DynamicOverrideRuntime>(__func__);
}

IDTypeInfo IDType_ID_OV = {
    /*id_code*/ DynamicOverride::id_type,
    /*id_filter*/ FILTER_ID_OV,
    /*dependencies_id_types*/ FILTER_ID_ALL,
    /*main_listbase_index*/ INDEX_ID_OV,
    /*struct_size*/ sizeof(DynamicOverride),
    /*name*/ "Dynamic Override",
    /*name_plural*/ N_("dynamic_overrides"),
    /*translation_context*/ BLT_I18NCONTEXT_ID_DYNAMIC_OVERRIDE,
    /*flags*/ 0,
    /*asset_type_info*/ nullptr,

    /*init_data*/ dynoverride_init_data,
    /*copy_data*/ dynoverride_copy_data,
    /*free_data*/ dynoverride_free_data,
    /*make_local*/ nullptr,
    /*foreach_id*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_path*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
    /*owner_pointer_get*/ nullptr,

    /*blend_write*/ dynoverride_blend_write,
    /*blend_read_data*/ dynoverride_blend_read_data,
    /*blend_read_after_liblink*/ nullptr,

    /*blend_read_undo_preserve*/ nullptr,

    /*lib_override_apply_post*/ nullptr,
};

static void dynamic_override_rule_property_copy(
    DynamicOverrideRuleProperty *dynoverride_rule_property_dst,
    DynamicOverrideRuleProperty *dynoverride_rule_property_src,
    const int flag)
{
  /* NOTE: A flat copy is assumed to have already happened before calling this function (e.g. by
   * using `BLI_duplicatelist`). */

  dynoverride_rule_property_dst->rna_path = MEM_dupalloc(dynoverride_rule_property_src->rna_path);
  dynoverride_rule_property_dst->sub_item_name = MEM_dupalloc(
      dynoverride_rule_property_src->sub_item_name);
  dynoverride_rule_property_dst->sub_item_index = dynoverride_rule_property_src->sub_item_index;

  dynoverride_rule_property_dst->new_value = IDP_CopyProperty_ex(
      dynoverride_rule_property_src->new_value, flag);
  dynoverride_rule_property_dst->orig_value = IDP_CopyProperty_ex(
      dynoverride_rule_property_src->orig_value, flag);
}

static void dynamic_override_rule_copy(DynamicOverrideRule *dynoverride_rule_dst,
                                       DynamicOverrideRule *dynoverride_rule_src,
                                       const int flag)
{
  /* NOTE: A flat copy is assumed to have already happened before calling this function (e.g. by
   * using `BLI_duplicatelist`). */

  BLI_assert(dynoverride_rule_dst->type == dynoverride_rule_src->type);
  BLI_assert(dynoverride_rule_dst->type != DynamicOverrideRuleType::UNKNOWN);

  switch (dynoverride_rule_dst->type) {
    case DynamicOverrideRuleType::IDDATA: {
      DynamicOverrideRuleIDData *rule_dst = reinterpret_cast<DynamicOverrideRuleIDData *>(
          dynoverride_rule_dst);
      DynamicOverrideRuleIDData *rule_src = reinterpret_cast<DynamicOverrideRuleIDData *>(
          dynoverride_rule_src);

      if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) != 0) {
        id_us_plus(rule_dst->id_owner);
      }

      BLI_duplicatelist(&rule_dst->properties, &rule_src->properties);
      for (DynamicOverrideRuleProperty *
               property_dst =
                  static_cast<DynamicOverrideRuleProperty *>(rule_dst->properties.first),
              *property_src =
                  static_cast<DynamicOverrideRuleProperty *>(rule_src->properties.first);
           property_dst;
           property_dst = property_dst->next, property_src = property_src->next)
      {
        dynamic_override_rule_property_copy(property_dst, property_src, flag);
      }
      break;
    }
    default:
      BLI_assert_unreachable();
  }
}

}  // namespace blender
