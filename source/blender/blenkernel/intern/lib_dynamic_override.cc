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
#include "BLI_resource_scope.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"
#include "DNA_dynamic_override_types.h"
#include "DNA_scene_types.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
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
#include "BKE_main_invariants.hh"

#include "WM_types.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "BLO_read_write.hh"

#include "CLG_log.h"

namespace blender {

namespace bke::dynoverride {

struct GeneratedRuleSrnaData;

struct Runtime {};

struct RuleIDDataRuntime {
  /** Contains RNA types generated for the ID data rule interface. */
  std::shared_ptr<GeneratedRuleSrnaData> rule_srna_data = {};
};

static void rule_copy(DynamicOverrideRule &dynoverride_rule_dst,
                      DynamicOverrideRule &dynoverride_rule_src,
                      const int flag);
static void rule_free(DynamicOverrideRule &dynoverride_rule);

static void rule_foreach_id(DynamicOverrideRule &dynoverride_rule, LibraryForeachIDData &data);

static void rule_write(BlendWriter &writer, DynamicOverrideRule &dynoverride_rule);

static void rule_read_data(BlendDataReader &reader, DynamicOverrideRule &dynoverride_rule);

static void init_data(ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  dynoverride->runtime = MEM_new<Runtime>(__func__);
}

static void copy_data(Main * /*bmain*/,
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
    rule_copy(*rule_dst, *rule_src, flag);
  }

  dynoverride_dst->runtime = MEM_new<Runtime>(__func__);
}

static void free_data(ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules) {
    rule_free(dynoverride_rule);
  }
  BLI_freelistN(&dynoverride->rules);

  MEM_delete(dynoverride->runtime);
}

static void foreach_id(ID *id, LibraryForeachIDData *data)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules) {
    rule_foreach_id(dynoverride_rule, *data);
  }
}

static void blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  /* Clean up, important in undo case to reduce false detection of changed datablocks. */
  dynoverride->runtime = nullptr;

  /* Write ID itself. */
  writer->write_id_struct(id_address, dynoverride);
  BKE_id_blend_write(writer, &dynoverride->id);

  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules) {
    rule_write(*writer, dynoverride_rule);
  }
}

static void blend_read_data(BlendDataReader *reader, ID *id)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(id);

  dynoverride->runtime = MEM_new<Runtime>(__func__);

  BLO_read_struct_list(reader, DynamicOverrideRule, &dynoverride->rules);
  for (DynamicOverrideRule &dynoverride_rule : dynoverride->rules) {
    rule_read_data(*reader, dynoverride_rule);
  }
}

}  // namespace bke::dynoverride

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

    .init_data = bke::dynoverride::init_data,
    .copy_data = bke::dynoverride::copy_data,
    .free_data = bke::dynoverride::free_data,
    .make_local = nullptr,
    .foreach_id = bke::dynoverride::foreach_id,
    .foreach_cache = nullptr,
    .foreach_path = nullptr,
    .foreach_working_space_color = nullptr,
    .owner_pointer_get = nullptr,

    .blend_write = bke::dynoverride::blend_write,
    .blend_read_data = bke::dynoverride::blend_read_data,
    .blend_read_after_liblink = nullptr,

    .blend_read_undo_preserve = nullptr,

    .lib_override_apply_post = nullptr,
};

namespace bke::dynoverride {

/* -------------------------------------------------------------------- */
/** \name Helpers for IDTypeInfo callbacks.
 * \{ */

static void rule_property_copy(DynamicOverrideRuleProperty &dynoverride_rule_property_dst,
                               DynamicOverrideRuleProperty &dynoverride_rule_property_src,
                               const int flag)
{
  /* NOTE: A flat copy is assumed to have already happened before calling this function (e.g. by
   * using `BLI_duplicatelist`). */

  dynoverride_rule_property_dst.rna_path = MEM_dupalloc(dynoverride_rule_property_src.rna_path);
  dynoverride_rule_property_dst.sub_item_name = MEM_dupalloc(
      dynoverride_rule_property_src.sub_item_name);
  dynoverride_rule_property_dst.sub_item_index = dynoverride_rule_property_src.sub_item_index;
}

static void rule_copy(DynamicOverrideRule &dynoverride_rule_dst,
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
      rule_dst.runtime = MEM_new<RuleIDDataRuntime>(__func__);
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
        rule_property_copy(*property_dst, *property_src, flag);
      }
      break;
    }
    default:
      BLI_assert_unreachable();
  }
}

static void rule_property_free(DynamicOverrideRuleProperty &dynoverride_rule_property)
{
  MEM_delete(dynoverride_rule_property.rna_path);
  MEM_delete(dynoverride_rule_property.sub_item_name);
}

static void rule_free(DynamicOverrideRule &dynoverride_rule)
{
  switch (dynoverride_rule.type) {
    case DynamicOverrideRuleType::IDData: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);

      for (DynamicOverrideRuleProperty &property : rule.properties) {
        rule_property_free(property);
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

static void rule_foreach_id(DynamicOverrideRule &dynoverride_rule, LibraryForeachIDData &data)
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
      break;
    }
    default:
      break;
  }
}

static void rule_property_write(BlendWriter &writer,
                                DynamicOverrideRuleProperty &dynoverride_rule_property)
{
  writer.write_struct(&dynoverride_rule_property);

  writer.write_string(dynoverride_rule_property.rna_path);
  writer.write_string(dynoverride_rule_property.sub_item_name);
}

static void rule_write(BlendWriter &writer, DynamicOverrideRule &dynoverride_rule)
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
        rule_property_write(writer, property);
      }
      break;
    }
    default:
      writer.write_struct(&dynoverride_rule);
      break;
  }
}

static void rule_property_read_data(BlendDataReader &reader,
                                    DynamicOverrideRuleProperty &dynoverride_rule_property)
{
  BLO_read_string(&reader, &dynoverride_rule_property.rna_path);
  BLO_read_string(&reader, &dynoverride_rule_property.sub_item_name);
}

static void rule_read_data(BlendDataReader &reader, DynamicOverrideRule &dynoverride_rule)
{
  switch (dynoverride_rule.type) {
    case DynamicOverrideRuleType::IDData: {
      DynamicOverrideRuleIDData &rule = reinterpret_cast<DynamicOverrideRuleIDData &>(
          dynoverride_rule);
      rule.runtime = MEM_new<RuleIDDataRuntime>(__func__);

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
        rule_property_read_data(reader, property);
      }
      break;
    }
    default:
      break;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Basic Rule Management.
 * \{ */

static DynamicOverrideRuleIDData *rule_get_for_id(DynamicOverride &dynamic_override, ID &owner_id)
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

static DynamicOverrideRuleIDData &rule_add_for_id(DynamicOverride &dynamic_override, ID &owner_id)
{
  BLI_assert(!rule_get_for_id(dynamic_override, owner_id));

  DynamicOverrideRuleIDData *rule_id_data = MEM_new<DynamicOverrideRuleIDData>(__func__);
  rule_id_data->runtime = MEM_new<bke::dynoverride::RuleIDDataRuntime>(__func__);
  rule_id_data->base.type = DynamicOverrideRuleType::IDData;
  rule_id_data->base.target_filter.type = DynamicOverrideRuleTargetFilterType::IDSingle;
  rule_id_data->base.target_filter.target_id = &owner_id;
  BLI_addtail(&dynamic_override.rules, rule_id_data);

  DEG_id_tag_update(&dynamic_override.id, ID_RECALC_PARAMETERS);
  DEG_id_tag_update(&owner_id, ID_RECALC_DYNAMIC_OVERRIDE);
  DEG_relations_tag_update(G_MAIN);

  return *rule_id_data;
}

DynamicOverrideRuleIDData &rule_ensure_for_id(DynamicOverride &dynamic_override, ID &owner_id)
{
  DynamicOverrideRuleIDData *existing_rule = rule_get_for_id(dynamic_override, owner_id);

  if (existing_rule) {
    return *existing_rule;
  }
  return rule_add_for_id(dynamic_override, owner_id);
}

void rule_remove(DynamicOverride &dynamic_override, DynamicOverrideRule *existing_rule)
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

  rule_free(*existing_rule);
  MEM_delete(existing_rule);
}

void rule_remove_for_id(DynamicOverride &dynamic_override, ID &owner_id)
{
  DynamicOverrideRuleIDData *existing_rule = rule_get_for_id(dynamic_override, owner_id);

  if (existing_rule) {
    rule_remove(dynamic_override, &existing_rule->base);
  }
}

DynamicOverrideRuleProperty *rule_rna_property_add(Main &bmain,
                                                   DynamicOverride &dynamic_override,
                                                   DynamicOverrideRule &rule,
                                                   RNAPath &rna_path)
{
  if (rule.type != DynamicOverrideRuleType::IDData) {
    return nullptr;
  }

  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
  PointerRNA target_id_ptr, target_data_ptr;
  PropertyRNA *target_prop;

  target_id_ptr = RNA_id_pointer_create(rule_iddata.base.target_filter.target_id);
  RNA_path_resolve(&target_id_ptr, rna_path.path.c_str(), &target_data_ptr, &target_prop);

  if (!target_data_ptr.data || !target_prop) {
    return nullptr;
  }

  /* TODO: search and deduplicate in case of existing property. */
  DynamicOverrideRuleProperty *rule_property = MEM_new<DynamicOverrideRuleProperty>(__func__);
  rule_property->rna_path = BLI_strdup(rna_path.path.c_str());
  if (rna_path.key) {
    rule_property->sub_item_name = BLI_strdup(rna_path.key->c_str());
  }
  rule_property->sub_item_index = rna_path.index.value_or(-1);
  BLI_addtail(&rule_iddata.properties, rule_property);

  BKE_main_ensure_invariants(bmain, dynamic_override.id);

  PointerRNA override_data_ptr = RNA_pointer_create_id_subdata(
      dynamic_override.id, RNA_DynamicOverrideRuleIDDataInterface, &rule_iddata);
  PropertyRNA *override_rna_prop = RNA_struct_find_property(
      &override_data_ptr, rule_property_rna_identifier(*rule_property).c_str());

  if (!override_data_ptr.data || !override_rna_prop) {
    BLI_assert_unreachable();
    return nullptr;
  }

  RNA_property_copy(nullptr, override_data_ptr, target_data_ptr, override_rna_prop, target_prop);

  return rule_property;
}

void rule_property_remove(DynamicOverrideRule &rule,
                          DynamicOverrideRuleProperty *existing_property)
{
  BLI_assert(rule.type == DynamicOverrideRuleType::IDData);

  DynamicOverrideRuleIDData &rule_iddata = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
  BLI_assert(BLI_findindex(&rule_iddata.properties, existing_property) != -1);
  BLI_remlink(&rule_iddata.properties, existing_property);
  rule_property_free(*existing_property);
  MEM_delete(existing_property);
}

std::string rule_property_rna_identifier(DynamicOverrideRuleProperty &rule_property)
{
  std::string safe_property_name = rule_property.rna_path ? rule_property.rna_path : "";
  RNA_identifier_sanitize(safe_property_name, true);
  return safe_property_name;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invariants and runtime data updates.
 * \{ */

/**
 * Contains a runtime registration of RNA types generated based on the DynamicOverrideRule's
 * current properties. These RNA types are not registered in the global list, but owned by the rule
 * so that RNA access to the properties works as long as the rule exists.
 */
struct GeneratedRuleSrnaData {
  ResourceScope scope;
  StructRNA *new_values_struct;
  BlenderRNA *generated_rna;
  GeneratedRuleSrnaData()
  {
    generated_rna = RNA_create_runtime();
  }
  ~GeneratedRuleSrnaData()
  {
    RNA_free(generated_rna);
  }
};

static void dynamic_override_update_rules_srna(Main & /*bmain*/, DynamicOverride &dynamic_override)
{
  for (DynamicOverrideRule &rule : dynamic_override.rules) {
    if (rule.type != DynamicOverrideRuleType::IDData) {
      continue;
    }

    DynamicOverrideRuleIDData &iddata_rule = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
    auto generated = std::make_unique<GeneratedRuleSrnaData>();
    StructRNA *srna = RNA_def_struct_ptr(generated->generated_rna,
                                         "DynamicOverrideRuleIDDataInterfaceRT",
                                         RNA_DynamicOverrideRuleIDDataInterface);
    generated->new_values_struct = srna;

    for (DynamicOverrideRuleProperty &rule_property : iddata_rule.properties) {
      PointerRNA owner_id_ptr, ptr;
      PropertyRNA *prop_orig, *prop;

      owner_id_ptr = RNA_id_pointer_create(rule.target_filter.target_id);
      RNA_path_resolve(&owner_id_ptr, rule_property.rna_path, &ptr, &prop_orig);

      if (!ptr.data || !prop_orig) {
        continue;
      }

      const StringRefNull property_identifier = generated->scope.allocator().copy_string(
          rule_property_rna_identifier(rule_property));
      const PropertyType prop_type = RNA_property_type(prop_orig);
      StructRNA *prop_ptr_type = RNA_property_pointer_type(&ptr, prop_orig);

      prop = RNA_def_property(
          srna, property_identifier.c_str(), prop_type, RNA_property_subtype(prop_orig));
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

static void dynamic_override_update_rules_system_idprops(Main & /*bmain*/,
                                                         DynamicOverride &dynamic_override)
{
  for (DynamicOverrideRule &rule : dynamic_override.rules) {
    if (rule.type != DynamicOverrideRuleType::IDData) {
      continue;
    }

    DynamicOverrideRuleIDData &iddata_rule = reinterpret_cast<DynamicOverrideRuleIDData &>(rule);
    StructRNA *srna = iddata_rule.runtime->rule_srna_data->new_values_struct;
    if (iddata_rule.new_values) {
      PointerRNA properties_ptr = RNA_pointer_create_discrete(&dynamic_override.id, srna, &rule);
      RNA_sync_system_properties(properties_ptr, *iddata_rule.new_values);
    }

    DEG_id_tag_update(rule.target_filter.target_id, ID_RECALC_DYNAMIC_OVERRIDE);
  }
}

void update(Main &bmain, std::optional<Span<DynamicOverride *>> modified_dynamic_overrides)
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
    dynamic_override_update_rules_srna(bmain, *dynamic_override);
    dynamic_override_update_rules_system_idprops(bmain, *dynamic_override);
  }
}

StructRNA *rule_get_runtime_properties_rna_struct(DynamicOverrideRuleIDData &iddata_rule)
{
  if (iddata_rule.runtime->rule_srna_data &&
      iddata_rule.runtime->rule_srna_data->new_values_struct)
  {
    return iddata_rule.runtime->rule_srna_data->new_values_struct;
  }
  return RNA_DynamicOverrideRuleIDDataInterface;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Runtime/depgraph building & evaluation context.
 * \{ */

void DepsgraphCtx::gather_dynamic_overrides(const bool force_reset)
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

void DepsgraphCtx::gather_id_targets(const bool force_reset)
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

DynamicOverride *DepsgraphCtx::get_override_for_id(ID &id) const
{
  BLI_assert(dynamic_overrides_are_gathered_ && id_targets_are_gathered_);
  /* TODO once there are several dynoverride IDs composed together, should be a mapping returning
   * the 'root' override ID for a given ID. */
  if (id_targets_.contains(&id)) {
    return dynamic_overrides_[0];
  }
  return nullptr;
}

Span<const DynamicOverrideRule *> DepsgraphCtx::get_override_rules_for_id(ID &id) const
{
  BLI_assert(dynamic_overrides_are_gathered_ && id_targets_are_gathered_);
  /* TODO once there are several dynoverride IDs composed together, should be a mapping returning
   * the 'root' override ID for a given ID. */
  if (id_targets_.contains(&id)) {
    return id_targets_.lookup_as(&id);
  }
  return {};
}

DynamicOverride *DepsgraphCtx::get_evaluated_override_for_id(Depsgraph &depsgraph, ID &id) const
{
  DynamicOverride *dynamic_override_orig = this->get_override_for_id(id);
  if (dynamic_override_orig) {
    return id_cast<DynamicOverride *>(
        DEG_get_evaluated_id(&depsgraph, id_cast<ID *>(dynamic_override_orig)));
  }
  return nullptr;
}

void eval_for_id(Depsgraph &depsgraph, DepsgraphCtx &eval_context, ID &id_cow)
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
    for (DynamicOverrideRuleProperty &rule_prop : rule_iddata.properties) {
      PointerRNA data_cow_ptr;
      PropertyRNA *data_cow_rna_prop;
      RNA_path_resolve(&id_cow_ptr, rule_prop.rna_path, &data_cow_ptr, &data_cow_rna_prop);

      if (!data_cow_ptr.data || !data_cow_rna_prop) {
        continue;
      }

      PointerRNA override_data_ptr = RNA_pointer_create_id_subdata(
          dynamic_override->id, RNA_DynamicOverrideRuleIDDataInterface, &rule_iddata);
      PropertyRNA *override_rna_prop = RNA_struct_find_property(
          &override_data_ptr, rule_property_rna_identifier(rule_prop).c_str());

      if (!override_data_ptr.data || !override_rna_prop) {
        BLI_assert_unreachable();
        continue;
      }

      RNA_property_copy(
          nullptr, data_cow_ptr, override_data_ptr, data_cow_rna_prop, override_rna_prop);
    }
  }
}

/** \} */

}  // namespace bke::dynoverride

}  // namespace blender
