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

#include "RNA_access.hh"
#include "RNA_path.hh"

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

static void dynamic_override_rule_copy(DynamicOverrideRule &dynoverride_rule_dst,
                                       DynamicOverrideRule &dynoverride_rule_src,
                                       const int flag);
static void dynamic_override_rule_free(DynamicOverrideRule &dynoverride_rule);

static void dynamic_override_rule_foreach_id(DynamicOverrideRule &dynoverride_rule,
                                             LibraryForeachIDData &data);

static void dynamic_override_rule_write(BlendWriter &writer,
                                        DynamicOverrideRule &dynoverride_rule);

static void dynamic_override_rule_read_data(BlendDataReader &reader,
                                            DynamicOverrideRule &dynoverride_rule);

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
    dynamic_override_rule_copy(*dynoverride_rule_dst, *dynoverride_rule_src, flag);
  }

  dynoverride_dst->runtime = MEM_new<bke::DynamicOverrideRuntime>(__func__);
}

static void dynoverride_free_data(ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules) {
    dynamic_override_rule_free(dynoverride_rule);
  }

  MEM_delete(dynoverride->runtime);
}

static void dynoverride_foreach_id(ID *id, LibraryForeachIDData *data)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules) {
    dynamic_override_rule_foreach_id(dynoverride_rule, *data);
  }
}

static void dynoverride_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  /* Clean up, important in undo case to reduce false detection of changed datablocks. */
  dynoverride->runtime = nullptr;

  /* Write ID itself. */
  writer->write_id_struct(id_address, dynoverride);
  BKE_id_blend_write(writer, &dynoverride->id);

  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules) {
    dynamic_override_rule_write(*writer, dynoverride_rule);
  }
}

static void dynoverride_blend_read_data(BlendDataReader *reader, ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  dynoverride->runtime = MEM_new<bke::DynamicOverrideRuntime>(__func__);

  BLO_read_struct_list(reader, DynamicOverrideRule, &dynoverride->rules);
  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules) {
    dynamic_override_rule_read_data(*reader, dynoverride_rule);
  }
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
    /*foreach_id*/ dynoverride_foreach_id,
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

/* -------------------------------------------------------------------- */
/** \name Helpers for IDTypeInfo callbacks.
 * \{ */

static void dynamic_override_rule_property_copy(
    DynamicOverrideRuleProperty &dynoverride_rule_property_dst,
    DynamicOverrideRuleProperty &dynoverride_rule_property_src,
    const int flag)
{
  /* NOTE: A flat copy is assumed to have already happened before calling this function (e.g. by
   * using `BLI_duplicatelist`). */

  dynoverride_rule_property_dst.rna_path = MEM_dupalloc(dynoverride_rule_property_src.rna_path);
  dynoverride_rule_property_dst.sub_item_name = MEM_dupalloc(
      dynoverride_rule_property_src.sub_item_name);
  dynoverride_rule_property_dst.sub_item_index = dynoverride_rule_property_src.sub_item_index;

  dynoverride_rule_property_dst.new_value = IDP_CopyProperty_ex(
      dynoverride_rule_property_src.new_value, flag);
  dynoverride_rule_property_dst.orig_value = IDP_CopyProperty_ex(
      dynoverride_rule_property_src.orig_value, flag);
}

static void dynamic_override_rule_copy(DynamicOverrideRule &dynoverride_rule_dst,
                                       DynamicOverrideRule &dynoverride_rule_src,
                                       const int flag)
{
  /* NOTE: A flat copy is assumed to have already happened before calling this function (e.g. by
   * using `BLI_duplicatelist`). */

  BLI_assert(dynoverride_rule_dst.type == dynoverride_rule_src.type);
  BLI_assert(dynoverride_rule_dst.type != DynamicOverrideRuleType::UNKNOWN);

  switch (dynoverride_rule_dst.type) {
    case DynamicOverrideRuleType::IDDATA: {
      DynamicOverrideRuleIDData &rule_dst = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule_dst);
      DynamicOverrideRuleIDData &rule_src = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule_src);

      if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) != 0) {
        id_us_plus(rule_dst.owner_id);
      }

      BLI_duplicatelist(&rule_dst.properties, &rule_src.properties);
      for (DynamicOverrideRuleProperty *
               property_dst =
                  static_cast<DynamicOverrideRuleProperty *>(rule_dst.properties.first),
              *property_src =
                  static_cast<DynamicOverrideRuleProperty *>(rule_src.properties.first);
           property_dst;
           property_dst = property_dst->next, property_src = property_src->next)
      {
        dynamic_override_rule_property_copy(*property_dst, *property_src, flag);
      }
      break;
    }
    default:
      BLI_assert_unreachable();
  }
}

static void dynamic_override_rule_property_free(
    DynamicOverrideRuleProperty &dynoverride_rule_property)
{
  MEM_delete(dynoverride_rule_property.rna_path);
  MEM_delete(dynoverride_rule_property.sub_item_name);

  IDP_FreeProperty(dynoverride_rule_property.new_value);
  IDP_FreeProperty(dynoverride_rule_property.orig_value);
}

static void dynamic_override_rule_free(DynamicOverrideRule &dynoverride_rule)
{
  switch (dynoverride_rule.type) {
    case DynamicOverrideRuleType::IDDATA: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);

      for (DynamicOverrideRuleProperty &property : rule.properties) {
        dynamic_override_rule_property_free(property);
      }
      BLI_freelistN(&rule.properties);
      break;
    }
    default:
      break;
  }
}

static void dynamic_override_rule_property_foreach_id(
    DynamicOverrideRuleProperty &dynoverride_rule_property, LibraryForeachIDData &data)
{
  IDP_foreach_property(
      dynoverride_rule_property.new_value, IDP_TYPE_FILTER_ID, [&](IDProperty *prop) {
        BKE_lib_query_idpropertiesForeachIDLink_callback(prop, &data);
      });
  IDP_foreach_property(
      dynoverride_rule_property.orig_value, IDP_TYPE_FILTER_ID, [&](IDProperty *prop) {
        BKE_lib_query_idpropertiesForeachIDLink_callback(prop, &data);
      });
}

static void dynamic_override_rule_foreach_id(DynamicOverrideRule &dynoverride_rule,
                                             LibraryForeachIDData &data)
{
  switch (dynoverride_rule.type) {
    case DynamicOverrideRuleType::IDDATA: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);

      BKE_LIB_FOREACHID_PROCESS_ID(&data, rule.owner_id, IDWALK_CB_NOP);

      for (DynamicOverrideRuleProperty &property : rule.properties) {
        dynamic_override_rule_property_foreach_id(property, data);
      }
      break;
    }
    default:
      break;
  }
}

static void dynamic_override_rule_property_write(
    BlendWriter &writer, DynamicOverrideRuleProperty &dynoverride_rule_property)
{
  writer.write_struct(&dynoverride_rule_property);

  BLO_write_string(&writer, dynoverride_rule_property.rna_path);
  BLO_write_string(&writer, dynoverride_rule_property.sub_item_name);

  if (dynoverride_rule_property.new_value) {
    IDP_BlendWrite(&writer, dynoverride_rule_property.new_value);
  }
  if (dynoverride_rule_property.orig_value) {
    IDP_BlendWrite(&writer, dynoverride_rule_property.orig_value);
  }
}

static void dynamic_override_rule_write(BlendWriter &writer, DynamicOverrideRule &dynoverride_rule)
{
  switch (dynoverride_rule.type) {
    case DynamicOverrideRuleType::IDDATA: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);

      writer.write_struct(&rule);

      for (DynamicOverrideRuleProperty &property : rule.properties) {
        dynamic_override_rule_property_write(writer, property);
      }
      break;
    }
    default:
      writer.write_struct(&dynoverride_rule);
      break;
  }
}

static void dynamic_override_rule_property_read_data(
    BlendDataReader &reader, DynamicOverrideRuleProperty &dynoverride_rule_property)
{
  BLO_read_string(&reader, &dynoverride_rule_property.rna_path);
  BLO_read_string(&reader, &dynoverride_rule_property.sub_item_name);

  if (dynoverride_rule_property.new_value) {
    IDP_BlendDataRead(&reader, &dynoverride_rule_property.new_value);
  }
  if (dynoverride_rule_property.orig_value) {
    IDP_BlendDataRead(&reader, &dynoverride_rule_property.orig_value);
  }
}

static void dynamic_override_rule_read_data(BlendDataReader &reader,
                                            DynamicOverrideRule &dynoverride_rule)
{
  switch (dynoverride_rule.type) {
    case DynamicOverrideRuleType::IDDATA: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);

      for (DynamicOverrideRuleProperty &property : rule.properties) {
        dynamic_override_rule_property_read_data(reader, property);
      }
      break;
    }
    default:
      break;
  }
}

/** \} */

namespace bke {

/* -------------------------------------------------------------------- */
/** \name Basic Rule Management.
 * \{ */

static DynamicOverrideRuleIDData *dynamic_override_rule_get_for_id(
    DynamicOverride &dynamic_override, ID &owner_id)
{
  /* TODO: use runtime data for mappings etc. */
  for (DynamicOverrideRule &rule : dynamic_override.rules) {
    if (rule.type != DynamicOverrideRuleType::IDDATA) {
      continue;
    }
    DynamicOverrideRuleIDData &rule_id_data = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
    if (rule_id_data.owner_id == &owner_id) {
      return &rule_id_data;
    }
  }

  return nullptr;
}

static DynamicOverrideRuleIDData &dynamic_override_rule_add_for_id(
    DynamicOverride &dynamic_override, ID &owner_id)
{
  BLI_assert(!dynamic_override_rule_get_for_id(dynamic_override, owner_id));

  DynamicOverrideRuleIDData *rule_id_data = MEM_new<DynamicOverrideRuleIDData>(__func__);
  rule_id_data->owner_id = &owner_id;
  rule_id_data->base.type = DynamicOverrideRuleType::IDDATA;
  BLI_addtail(&dynamic_override.rules, rule_id_data);

  return *rule_id_data;
}

DynamicOverrideRuleIDData &dynamic_override_rule_ensure_for_id(DynamicOverride &dynamic_override,
                                                               ID &owner_id)
{
  DynamicOverrideRuleIDData *existing_rule = dynamic_override_rule_get_for_id(dynamic_override,
                                                                              owner_id);

  if (existing_rule) {
    return *existing_rule;
  }
  return dynamic_override_rule_add_for_id(dynamic_override, owner_id);
}

void dynamic_override_rule_remove(DynamicOverride &dynamic_override,
                                  DynamicOverrideRule *existing_rule)
{
  BLI_assert(BLI_findindex(&dynamic_override.rules, existing_rule) != -1);
  BLI_remlink(&dynamic_override.rules, existing_rule);
  dynamic_override_rule_free(*existing_rule);
  MEM_delete(existing_rule);
}

void dynamic_override_rule_remove_for_id(DynamicOverride &dynamic_override, ID &owner_id)
{
  DynamicOverrideRuleIDData *existing_rule = dynamic_override_rule_get_for_id(dynamic_override,
                                                                              owner_id);

  if (existing_rule) {
    dynamic_override_rule_remove(dynamic_override, &existing_rule->base);
  }
}

static IDProperty *idproperty_from_rna_property(PointerRNA &ptr,
                                                PropertyRNA &prop,
                                                StringRef rna_path)
{
  const bool is_array = RNA_property_array_check(&prop);
  const PropertyType prop_type = RNA_property_type(&prop);

  if (is_array) {
    const int64_t len = RNA_property_array_length(&ptr, &prop);
    switch (prop_type) {
      case PROP_FLOAT: {
        float *values = MEM_new_array<float>(size_t(len), __func__);
        RNA_property_float_get_array(&ptr, &prop, values);
        return idprop::create(rna_path, {values, len}, IDP_FLAG_STATIC_TYPE).release();
      }
    }
  }
  /* Dummy */
  return idprop::create(rna_path, 1.0f, IDP_FLAG_STATIC_TYPE).release();
}

DynamicOverrideRuleProperty *dynamic_override_rule_rna_property_add(DynamicOverrideRule &rule,
                                                                    RNAPath &rna_path)
{
  if (rule.type != DynamicOverrideRuleType::IDDATA) {
    return nullptr;
  }

  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
  PointerRNA owner_id_ptr, ptr;
  PropertyRNA *prop;

  owner_id_ptr = RNA_id_pointer_create(rule_iddata.owner_id);
  RNA_path_resolve(&owner_id_ptr, rna_path.path.c_str(), &ptr, &prop);

  if (!ptr.data || !prop) {
    return nullptr;
  }

  /* TODO: search and deduplicate in case of existing property. */
  DynamicOverrideRuleProperty *rule_property = MEM_new<DynamicOverrideRuleProperty>(__func__);
  rule_property->rna_path = BLI_strdup(rna_path.path.c_str());
  if (rna_path.key) {
    rule_property->sub_item_name = BLI_strdup(rna_path.key->c_str());
  }
  rule_property->sub_item_index = rna_path.index.value_or(-1);

  rule_property->orig_value = idproperty_from_rna_property(ptr, *prop, rna_path.path);
  rule_property->new_value = rule_property->orig_value ?
                                 IDP_CopyProperty(rule_property->orig_value) :
                                 nullptr;

  BLI_addtail(&rule_iddata.properties, rule_property);

  return rule_property;
}

void dynamic_override_rule_property_remove(DynamicOverrideRule &rule,
                                           DynamicOverrideRuleProperty *existing_property)
{
  BLI_assert(rule.type == DynamicOverrideRuleType::IDDATA);

  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
  BLI_assert(BLI_findindex(&rule_iddata.properties, existing_property) != -1);
  BLI_remlink(&rule_iddata.properties, existing_property);
  dynamic_override_rule_property_free(*existing_property);
  MEM_delete(existing_property);
}

/** \} */

}  // namespace bke

}  // namespace blender
