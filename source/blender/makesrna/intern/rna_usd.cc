/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "rna_internal.hh"

#include "WM_types.hh"

#include "BLT_translation.hh"

#ifdef RNA_RUNTIME

#  include "DNA_object_types.h"
#  include "WM_api.hh"
#  include "DNA_usdhook_types.h"
#  include "usd.hh"

using namespace blender::io::usd;

static StructRNA *rna_USDHook_refine(PointerRNA *ptr)
{
  USDHook *hook = (USDHook *)ptr->data;
  return (hook->rna_ext.srna) ? hook->rna_ext.srna : &RNA_USDHook;
}

static bool rna_USDHook_unregister(Main * /*bmain*/, StructRNA *type)
{
  USDHook *hook = static_cast<USDHook *>(RNA_struct_blender_type_get(type));

  if (hook == nullptr) {
    return false;
  }

  /* free RNA data referencing this */
  RNA_struct_free_extension(type, &hook->rna_ext);
  RNA_struct_free(&BLENDER_RNA, type);

  WM_main_add_notifier(NC_WINDOW, nullptr);

  /* unlink Blender-side data */
  USD_unregister_hook(hook);

  return true;
}

static StructRNA *rna_USDHook_register(Main *bmain,
                                       ReportList *reports,
                                       void *data,
                                       const char *identifier,
                                       StructValidateFunc validate,
                                       StructCallbackFunc call,
                                       StructFreeFunc free)
{
  const char *error_prefix = "Registering USD hook class:";
  USDHook dummy_hook{};

  /* setup dummy type info to store static properties in */
  PointerRNA dummy_hook_ptr = RNA_pointer_create_discrete(nullptr, &RNA_USDHook, &dummy_hook);

  /* validate the python class */
  if (validate(&dummy_hook_ptr, data, nullptr) != 0) {
    return nullptr;
  }

  if (strlen(identifier) >= sizeof(dummy_hook.idname)) {
    BKE_reportf(reports,
                RPT_ERROR,
                "%s '%s' is too long, maximum length is %d",
                error_prefix,
                identifier,
                (int)sizeof(dummy_hook.idname));
    return nullptr;
  }

  /* check if we have registered this hook before, and remove it */
  if (USDHook *hook = USD_find_hook_name(dummy_hook.idname)) {
    BKE_reportf(reports,
                RPT_INFO,
                "%s '%s', bl_idname '%s' has been registered before, unregistering previous",
                error_prefix,
                identifier,
                dummy_hook.idname);

    StructRNA *srna = hook->rna_ext.srna;
    if (!rna_USDHook_unregister(bmain, srna)) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "%s '%s', bl_idname '%s' %s",
                  error_prefix,
                  identifier,
                  dummy_hook.idname,
                  "could not be unregistered");
      return nullptr;
    }
  }

  /* create a new KeyingSetInfo type */
  auto hook = std::make_unique<USDHook>();
  *hook = dummy_hook;

  /* set RNA-extensions info */
  hook->rna_ext.srna = RNA_def_struct_ptr(&BLENDER_RNA, hook->idname, &RNA_USDHook);
  hook->rna_ext.data = data;
  hook->rna_ext.call = call;
  hook->rna_ext.free = free;
  RNA_struct_blender_type_set(hook->rna_ext.srna, hook.get());

  /* add and register with other info as needed */
  StructRNA *srna = hook->rna_ext.srna;
  USD_register_hook(std::move(hook));

  WM_main_add_notifier(NC_WINDOW, nullptr);

  /* return the struct-rna added */
  return srna;
}

bool rna_HookHandle_is_valid_get(PointerRNA *ptr)
{
  return USD_is_hook_valid(RNA_string_get(ptr, "identifier").c_str());
}

static const EnumPropertyItem default_hook_enum = {0, "INVALID", 0, "Invalid Handle", "Something broke"};

static const EnumPropertyItem *rna_USDHookHandle_idval_itemf(
  bContext *C, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) 
{
  if (C == nullptr) {
    return rna_enum_dummy_DEFAULT_items;
  }
  *r_free = true;
  // return *default_hook_enum;
  return USD_build_hook_enum();
}

static void rna_USDHookHandle_idval_set(PointerRNA *ptr, int value)
{
  const char *identifier;
  const EnumPropertyItem *items = USD_build_hook_enum();
  bool is_valid = RNA_enum_identifier(items, value, &identifier);

  printf(identifier);
  printf(" ! \n");

  if (is_valid == true) {
    USDHookHandle *handle = static_cast<USDHookHandle *>(ptr->data);
    handle->id_val = value;
    RNA_string_set(ptr, "identifier", identifier);

    const char *name;
    RNA_enum_name(items, value, &name);
    RNA_string_set(ptr, "name", name);
  }
}

static void rna_USDHookHandle_identifier_get(PointerRNA *ptr, char *value) {
  USDHookHandle *handle = static_cast<USDHookHandle *>(ptr->data);
  strcpy(value, handle->identifier);
}

static void rna_USDHookHandle_name_get(PointerRNA *ptr, char *value) {
  USDHookHandle *handle = static_cast<USDHookHandle *>(ptr->data);
  strcpy(value, handle->name);
}
#else

static void rna_def_usd_hook(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "USDHook", nullptr);
  RNA_def_struct_ui_text(srna, "USD Hook", "Defines callback functions to extend USD IO");
  RNA_def_struct_sdna(srna, "USDHook");
  RNA_def_struct_refine_func(srna, "rna_USDHook_refine");
  RNA_def_struct_register_funcs(srna, "rna_USDHook_register", "rna_USDHook_unregister", nullptr);

  ///* Properties --------------------- */

  RNA_define_verify_sdna(false); /* not in sdna */

  prop = RNA_def_property(srna, "bl_idname", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "idname");
  RNA_def_property_flag(prop, PROP_REGISTER);
  RNA_def_property_ui_text(prop, "ID Name", "");

  prop = RNA_def_property(srna, "bl_label", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "name");
  RNA_def_property_ui_text(prop, "UI Name", "");
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_flag(prop, PROP_REGISTER);

  prop = RNA_def_property(srna, "bl_description", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "description");
  RNA_def_property_string_maxlength(prop, RNA_DYN_DESCR_MAX); /* else it uses the pointer size! */
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_ui_text(prop, "Description", "A short description of the USD hook");
}

static void rna_def_usd_hook_handle(BlenderRNA *brna) {
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "UsdHookHandle", "PropertyGroup");
  RNA_def_struct_sdna(srna, "UsdHookHandle");
  RNA_def_struct_ui_text(
      srna, "Operator Hook Handle", "A serializable handle representing a runtime hook");

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "name");
  RNA_def_property_string_funcs(prop, "rna_USDHookHandle_name_get", nullptr, nullptr);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE | PROP_IDPROPERTY | PROP_HIDDEN);
  RNA_def_property_ui_text(prop, "Hook Name", "Hook Name");

  prop = RNA_def_property(srna, "identifier", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "identifier");
  RNA_def_property_string_funcs(prop, "rna_USDHookHandle_identifier_get", nullptr, nullptr);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE | PROP_IDPROPERTY);
  RNA_def_property_ui_text(prop, "Hook Identifier", "Hook Identifier");

  prop = RNA_def_property(srna, "enabled", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE | PROP_IDPROPERTY);
  RNA_def_property_ui_icon(prop, ICON_CHECKBOX_DEHLT, 1);
  RNA_def_property_ui_text(prop, "Enabled", "This hook will run");

  prop = RNA_def_property(srna, "valid", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE | PROP_IDPROPERTY | PROP_HIDDEN);
  RNA_def_property_ui_text(prop, "Valid", "This hook is valid");
  RNA_def_property_boolean_funcs(prop, "rna_HookHandle_is_valid_get", nullptr);

  prop = RNA_def_property(srna, "handle_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_dummy_DEFAULT_items);
  RNA_def_property_enum_sdna(prop, nullptr, "id_val");
  RNA_def_property_enum_funcs(
      prop, nullptr, "rna_USDHookHandle_idval_set", "rna_USDHookHandle_idval_itemf");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_PRESET);
  RNA_def_property_ui_text(prop, "Subtype", "Subtype of the default value");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_UNIT);
}

/* --- */

void RNA_def_usd(BlenderRNA *brna)
{
  rna_def_usd_hook(brna);
  rna_def_usd_hook_handle(brna);
}

#endif
