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
#include "DNA_scene_types.h"

#include "RNA_access.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_dynamic_override.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"

#include "WM_types.hh"

/* TODO: Make this a generic RNA API instead, if it works as-is also for non-nodes data... */
#include "NOD_nodes_srna.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "BLO_read_write.hh"

#include "CLG_log.h"

namespace blender {

namespace bke {

struct DynamicOverrideRuntime {};

struct DynamicOverrideRuleIDDataRuntime {
  /** Contains RNA types generated for the ID data rule interface. */
  std::shared_ptr<nodes::GeneratedTreeSrnaData> rule_srna_data = {};
};

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
           rule_dst = static_cast<DynamicOverrideRule *>(dynoverride_dst->rules.first),
          *rule_src = static_cast<DynamicOverrideRule *>(dynoverride_src->rules.first);
       rule_dst;
       rule_dst = rule_dst->next, rule_src = rule_src->next)
  {
    dynamic_override_rule_copy(*rule_dst, *rule_src, flag);
  }

  dynoverride_dst->runtime = MEM_new<bke::DynamicOverrideRuntime>(__func__);
}

static void dynoverride_free_data(ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules) {
    dynamic_override_rule_free(dynoverride_rule);
  }
  BLI_freelistN(&dynoverride->rules);

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
    .id_code = DynamicOverride::id_type,
    .id_filter = FILTER_ID_OV,
    .dependencies_id_types = FILTER_ID_ALL,
    .main_listbase_index = INDEX_ID_OV,
    .struct_size = sizeof(DynamicOverride),
    .name = "Dynamic Override",
    .name_plural = N_("dynamic_overrides"),
    .translation_context = BLT_I18NCONTEXT_ID_DYNAMIC_OVERRIDE,
    .flags = 0,
    .asset_type_info = nullptr,

    .init_data = dynoverride_init_data,
    .copy_data = dynoverride_copy_data,
    .free_data = dynoverride_free_data,
    .make_local = nullptr,
    .foreach_id = dynoverride_foreach_id,
    .foreach_cache = nullptr,
    .foreach_path = nullptr,
    .foreach_working_space_color = nullptr,
    .owner_pointer_get = nullptr,

    .blend_write = dynoverride_blend_write,
    .blend_read_data = dynoverride_blend_read_data,
    .blend_read_after_liblink = nullptr,

    .blend_read_undo_preserve = nullptr,

    .lib_override_apply_post = nullptr,
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

  if (dynoverride_rule_property_src.new_value) {
    dynoverride_rule_property_dst.new_value = IDP_CopyProperty_ex(
        dynoverride_rule_property_src.new_value, flag);
  }
  if (dynoverride_rule_property_src.orig_value) {
    dynoverride_rule_property_dst.orig_value = IDP_CopyProperty_ex(
        dynoverride_rule_property_src.orig_value, flag);
  }
}

static void dynamic_override_rule_copy(DynamicOverrideRule &dynoverride_rule_dst,
                                       DynamicOverrideRule &dynoverride_rule_src,
                                       const int flag)
{
  /* NOTE: A flat copy is assumed to have already happened before calling this function (e.g. by
   * using `BLI_duplicatelist`). */

  BLI_assert(dynoverride_rule_dst.type == dynoverride_rule_src.type);
  BLI_assert(dynoverride_rule_dst.type != DynamicOverrideRuleType::Unknown);

  switch (dynoverride_rule_dst.type) {
    case DynamicOverrideRuleType::IDData: {
      DynamicOverrideRuleIDData &rule_dst = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule_dst);
      DynamicOverrideRuleIDData &rule_src = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule_src);
      rule_dst.runtime = MEM_new<bke::DynamicOverrideRuleIDDataRuntime>(__func__);
      rule_dst.runtime->rule_srna_data = rule_src.runtime->rule_srna_data;

      if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
        id_us_plus(rule_dst.base.target_filter.target_id);
      }

      if (rule_src.new_values) {
        rule_dst.new_values = IDP_CopyProperty_ex(rule_src.new_values, flag);
      }
      if (rule_src.orig_values) {
        rule_dst.orig_values = IDP_CopyProperty_ex(rule_src.orig_values, flag);
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

  if (dynoverride_rule_property.new_value) {
    IDP_FreeProperty(dynoverride_rule_property.new_value);
  }
  if (dynoverride_rule_property.orig_value) {
    IDP_FreeProperty(dynoverride_rule_property.orig_value);
  }
}

static void dynamic_override_rule_free(DynamicOverrideRule &dynoverride_rule)
{
  switch (dynoverride_rule.type) {
    case DynamicOverrideRuleType::IDData: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);

      for (DynamicOverrideRuleProperty &property : rule.properties) {
        dynamic_override_rule_property_free(property);
      }
      BLI_freelistN(&rule.properties);

      if (rule.new_values) {
        IDP_FreeProperty(rule.new_values);
      }
      if (rule.orig_values) {
        IDP_FreeProperty(rule.orig_values);
      }

      MEM_delete(rule.runtime);
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
    case DynamicOverrideRuleType::IDData: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);

      BKE_LIB_FOREACHID_PROCESS_ID(&data, rule.base.target_filter.target_id, IDWALK_CB_NOP);

      IDP_foreach_property(rule.new_values, IDP_TYPE_FILTER_ID, [&](IDProperty *prop) {
        BKE_lib_query_idpropertiesForeachIDLink_callback(prop, &data);
      });
      IDP_foreach_property(rule.orig_values, IDP_TYPE_FILTER_ID, [&](IDProperty *prop) {
        BKE_lib_query_idpropertiesForeachIDLink_callback(prop, &data);
      });

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

  writer.write_string(dynoverride_rule_property.rna_path);
  writer.write_string(dynoverride_rule_property.sub_item_name);

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
    case DynamicOverrideRuleType::IDData: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);

      writer.write_struct(&rule);

      if (rule.new_values) {
        IDP_BlendWrite(&writer, rule.new_values);
      }
      if (rule.orig_values) {
        IDP_BlendWrite(&writer, rule.orig_values);
      }

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

  BLO_read_struct(&reader, IDProperty, &dynoverride_rule_property.new_value);
  if (dynoverride_rule_property.new_value) {
    IDP_DirectLinkProperty(&reader, dynoverride_rule_property.new_value);
  }
  BLO_read_struct(&reader, IDProperty, &dynoverride_rule_property.orig_value);
  if (dynoverride_rule_property.orig_value) {
    IDP_DirectLinkProperty(&reader, dynoverride_rule_property.orig_value);
  }
}

static void dynamic_override_rule_read_data(BlendDataReader &reader,
                                            DynamicOverrideRule &dynoverride_rule)
{
  switch (dynoverride_rule.type) {
    case DynamicOverrideRuleType::IDData: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);
      rule.runtime = MEM_new<bke::DynamicOverrideRuleIDDataRuntime>(__func__);

      BLO_read_struct(&reader, IDProperty, &rule.new_values);
      if (rule.new_values) {
        IDP_DirectLinkProperty(&reader, rule.new_values);
      }
      BLO_read_struct(&reader, IDProperty, &rule.orig_values);
      if (rule.orig_values) {
        IDP_DirectLinkProperty(&reader, rule.orig_values);
      }

      BLO_read_struct_list(&reader, DynamicOverrideRuleProperty, &rule.properties);
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
    if (rule.type != DynamicOverrideRuleType::IDData) {
      continue;
    }
    DynamicOverrideRuleIDData &rule_id_data = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
    if (rule_id_data.base.target_filter.target_id == &owner_id) {
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
  rule_id_data->runtime = MEM_new<bke::DynamicOverrideRuleIDDataRuntime>(__func__);
  rule_id_data->base.type = DynamicOverrideRuleType::IDData;
  rule_id_data->base.target_filter.type = DynamicOverrideRuleTargetFilterType::IDSingle;
  rule_id_data->base.target_filter.target_id = &owner_id;
  BLI_addtail(&dynamic_override.rules, rule_id_data);

  DEG_id_tag_update(&dynamic_override.id, ID_RECALC_PARAMETERS);
  DEG_id_tag_update(&owner_id, ID_RECALC_DYNAMIC_OVERRIDE);
  DEG_relations_tag_update(G_MAIN);

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

  if (existing_rule->type == DynamicOverrideRuleType::IDData) {
    DynamicOverrideRuleIDData *iddata_rule = reinterpret_cast<DynamicOverrideRuleIDData *>(
        existing_rule);
    DEG_id_tag_update(&dynamic_override.id, ID_RECALC_PARAMETERS);
    DEG_id_tag_update(iddata_rule->base.target_filter.target_id, ID_RECALC_DYNAMIC_OVERRIDE);
    DEG_relations_tag_update(G_MAIN);
  }

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
        Array<float> values{len};
        RNA_property_float_get_array(&ptr, &prop, values.data());
        return idprop::create(rna_path, {values.data(), len}, IDP_FLAG_STATIC_TYPE).release();
      }
    }
  }
  /* Dummy */
  return idprop::create(rna_path, 1.0f, IDP_FLAG_STATIC_TYPE).release();
}

DynamicOverrideRuleProperty *dynamic_override_rule_rna_property_add(DynamicOverrideRule &rule,
                                                                    RNAPath &rna_path)
{
  if (rule.type != DynamicOverrideRuleType::IDData) {
    return nullptr;
  }

  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
  PointerRNA owner_id_ptr, ptr;
  PropertyRNA *prop;

  owner_id_ptr = RNA_id_pointer_create(rule_iddata.base.target_filter.target_id);
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
  BLI_assert(rule.type == DynamicOverrideRuleType::IDData);

  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
  BLI_assert(BLI_findindex(&rule_iddata.properties, existing_property) != -1);
  BLI_remlink(&rule_iddata.properties, existing_property);
  dynamic_override_rule_property_free(*existing_property);
  MEM_delete(existing_property);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invariants and runtime data updates.
 * \{ */

static void dynamic_override_update_rules_srna(Main & /*bmain*/, DynamicOverride *dynamic_override)
{
  for (DynamicOverrideRule &rule : dynamic_override->rules) {
    if (rule.type != DynamicOverrideRuleType::IDData) {
      continue;
    }

    DynamicOverrideRuleIDData &iddata_rule = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
    auto generated = std::make_unique<nodes::GeneratedTreeSrnaData>();
    StructRNA *srna = RNA_def_struct_ptr(generated->generated_rna,
                                         "DynamicOverrideRuleIDDataInterfaceRT",
                                         RNA_DynamicOverrideRuleIDDataInterface);
    generated->properties_struct = srna;

    for (DynamicOverrideRuleProperty &rule_property : iddata_rule.properties) {
      PointerRNA owner_id_ptr, ptr;
      PropertyRNA *prop_orig, *prop;

      owner_id_ptr = RNA_id_pointer_create(rule.target_filter.target_id);
      RNA_path_resolve(&owner_id_ptr, rule_property.rna_path, &ptr, &prop_orig);

      if (!ptr.data || !prop_orig) {
        continue;
      }

      std::string safe_property_name = rule_property.rna_path;
      RNA_identifier_sanitize(safe_property_name, true);
      const PropertyType prop_type = RNA_property_type(prop_orig);
      StructRNA *prop_ptr_type = RNA_property_pointer_type(&ptr, prop_orig);

      /* XXX XXXXXXX
       * FIXME
       * Absolute horrible hack to get things working.
       * TODO: Likely:
       *   * Have dedicated version of nodes::GeneratedTreeSrnaData for liboverrides (also needed
       *     propbably if we want to feature access to 'original' values, need another AtructRNA
       *     pointer for these then).
       *   * Use its ResourceScope to e.g. store a Set of all used properties rna path (or a map,
       *     from real rna paths to property-compatible identifier, or... etc.).
       */
      static char name_buff[100000];
      static int name_buff_idx = 0;
      BLI_strncpy(
          &name_buff[name_buff_idx], safe_property_name.c_str(), safe_property_name.size() + 1);
      prop = RNA_def_property(
          srna, &name_buff[name_buff_idx], prop_type, RNA_property_subtype(prop_orig));
      name_buff_idx += safe_property_name.size() + 1;
      if (prop_ptr_type != RNA_UnknownType) {
        RNA_def_property_struct_runtime(srna, prop, prop_ptr_type);
      }
      switch (prop_type) {
        case PROP_FLOAT: {
          float min, max;
          RNA_property_float_range(&ptr, prop_orig, &min, &max);
          RNA_def_property_range(prop, double(min), double(max));

          float softmin, softmax, step, precision;
          RNA_property_float_ui_range(&ptr, prop_orig, &softmin, &softmax, &step, &precision);
          RNA_def_property_ui_range(
              prop, double(softmin), double(softmax), double(step), int(precision));
          break;
        }
        case PROP_INT: {
          int min, max;
          RNA_property_int_range(&ptr, prop_orig, &min, &max);
          RNA_def_property_range(prop, double(min), double(max));

          int softmin, softmax, step;
          RNA_property_int_ui_range(&ptr, prop_orig, &softmin, &softmax, &step);
          RNA_def_property_ui_range(prop, double(softmin), double(softmax), double(step), -1);
          break;
        }
        case PROP_ENUM: {
          /* TODO: enum items. */
          break;
        }
        case PROP_BOOLEAN:
        case PROP_STRING:
        case PROP_POINTER:
        case PROP_COLLECTION:
          break;
      }

      if (RNA_property_array_check(prop_orig)) {
        int arraylength[RNA_MAX_ARRAY_DIMENSION];
        const int dimension = RNA_property_array_dimension(&ptr, prop_orig, arraylength);
        RNA_def_property_multi_array(prop, dimension, arraylength);
      }

      /* TODO: likely also need to copy over default values. */

      RNA_def_property_ui_text(prop,
                               RNA_property_ui_name_raw(prop_orig, &ptr),
                               RNA_property_ui_description_raw(prop_orig, &ptr));

      RNA_def_property_update_runtime(
          prop, [](Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr) {
            DynamicOverrideRule *rule = ptr->data_as<DynamicOverrideRule>();
            BLI_assert(rule->type == DynamicOverrideRuleType::IDData);

            DEG_id_tag_update(rule->target_filter.target_id, ID_RECALC_DYNAMIC_OVERRIDE);
          });
      RNA_def_property_update_notifier(prop, NC_ID | NA_EDITED);
    }

    iddata_rule.runtime->rule_srna_data = std::move(generated);
  }
}

void dynamic_override_update(Main &bmain,
                             std::optional<Span<DynamicOverride *>> modified_dynamic_overrides)
{
  Vector<DynamicOverride *> changed_dynamic_overrides;
  if (!modified_dynamic_overrides.has_value()) {
    for (DynamicOverride &dynoverride : bmain.dynamic_overrides) {
      if (true) {
        changed_dynamic_overrides.append(&dynoverride);
      }
    }
    modified_dynamic_overrides = changed_dynamic_overrides;
  }

  for (DynamicOverride *dynamic_override : *modified_dynamic_overrides) {
    dynamic_override_update_rules_srna(bmain, dynamic_override);
  }
}

StructRNA *dynamic_override_rule_get_runtime_properties_rna_struct(
    DynamicOverrideRuleIDData &iddata_rule)
{
  if (iddata_rule.runtime->rule_srna_data &&
      iddata_rule.runtime->rule_srna_data->properties_struct)
  {
    return iddata_rule.runtime->rule_srna_data->properties_struct;
  }
  return RNA_DynamicOverrideRuleIDDataInterface;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Runtime/depgraph building & evaluation context.
 * \{ */

void DynamicOverrideDepsgraphCtx::gather_dynamic_overrides(const bool force_reset)
{
  if (force_reset) {
    dynamic_overrides_.clear();
    dynamic_overrides_are_gathered_ = false;
  }
  if (dynamic_overrides_are_gathered_) {
    return;
  }

  if (scene_->dynamic_override) {
    dynamic_overrides_.add_as(scene_->dynamic_override);
  }
  dynamic_overrides_are_gathered_ = true;
}

void DynamicOverrideDepsgraphCtx::gather_id_targets(const bool force_reset)
{
  if (force_reset) {
    id_targets_.clear();
    id_targets_are_gathered_ = false;
  }

  if (id_targets_are_gathered_) {
    return;
  }

  gather_dynamic_overrides(force_reset);
  for (const DynamicOverride *dynoverride_iter : dynamic_overrides_) {
    for (const DynamicOverrideRule &rule_iter : dynoverride_iter->rules) {
      if (rule_iter.type == DynamicOverrideRuleType::IDData) {
        const DynamicOverrideRuleIDData &id_rule =
            reinterpret_cast<const DynamicOverrideRuleIDData &>(rule_iter);
        if (id_rule.base.target_filter.target_id) {
          /* Dynamic overrides are not allowed to be overridden by other dynamic overrides!
           * NOTE: Once implemented, dynoverride imports will be a different case. */
          BLI_assert(GS(id_rule.base.target_filter.target_id->name) != ID_OV);
          id_targets_.lookup_or_add(id_rule.base.target_filter.target_id, {}).append(&rule_iter);
        }
      }
    }
  }
  id_targets_are_gathered_ = true;
}

DynamicOverride *DynamicOverrideDepsgraphCtx::get_override_for_id(ID &id) const
{
  BLI_assert(dynamic_overrides_are_gathered_ && id_targets_are_gathered_);
  /* TODO once there are several dynoverride IDs composed together, should be a mapping returning
   * the 'root' override ID for a given ID. */
  if (id_targets_.contains(&id)) {
    return dynamic_overrides_[0];
  }
  return nullptr;
}

Span<const DynamicOverrideRule *> DynamicOverrideDepsgraphCtx::get_override_rules_for_id(
    ID &id) const
{
  BLI_assert(dynamic_overrides_are_gathered_ && id_targets_are_gathered_);
  /* TODO once there are several dynoverride IDs composed together, should be a mapping returning
   * the 'root' override ID for a given ID. */
  if (id_targets_.contains(&id)) {
    return id_targets_.lookup_as(&id);
  }
  return {};
}

DynamicOverride *DynamicOverrideDepsgraphCtx::get_evaluated_override_for_id(Depsgraph &depsgraph,
                                                                            ID &id) const
{
  DynamicOverride *dynamic_override_orig = this->get_override_for_id(id);
  if (dynamic_override_orig) {
    return id_cast<DynamicOverride *>(
        DEG_get_evaluated_id(&depsgraph, id_cast<ID *>(dynamic_override_orig)));
  }
  return nullptr;
}

void dynamic_override_eval_for_id(Depsgraph &depsgraph,
                                  DynamicOverrideDepsgraphCtx &eval_context,
                                  ID &id_cow)
{
  ID *id_orig = DEG_get_original_id(&id_cow);
  DynamicOverride *dynamic_override = eval_context.get_evaluated_override_for_id(depsgraph,
                                                                                 *id_orig);
  BLI_assert(dynamic_override);

  PointerRNA id_cow_ptr = RNA_id_pointer_create(&id_cow);
  for (DynamicOverrideRule &rule : dynamic_override->rules) {
    if (rule.type != DynamicOverrideRuleType::IDData) {
      continue;
    }
    DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
    if (rule_iddata.base.target_filter.target_id != &id_cow) {
      continue;
    }
    for (DynamicOverrideRuleProperty &prop : rule_iddata.properties) {
      PointerRNA ptr;
      PropertyRNA *rna_prop;
      RNA_path_resolve(&id_cow_ptr, prop.rna_path, &ptr, &rna_prop);

      if (!ptr.data || !rna_prop) {
        continue;
      }

      const bool is_array = RNA_property_array_check(rna_prop);
      switch (RNA_property_type(rna_prop)) {
        case PROP_FLOAT:
          if (is_array) {
            int array_len = RNA_property_array_length(&ptr, rna_prop);
            if (prop.new_value->type == IDP_ARRAY && prop.new_value->subtype == IDP_FLOAT &&
                prop.new_value->len == array_len)
            {
              RNA_property_float_set_array(&ptr, rna_prop, IDP_array_float_get(prop.new_value));
            }
          }
          break;
        default:
          break;
      }
    }
  }
}

/** \} */

}  // namespace bke

}  // namespace blender
