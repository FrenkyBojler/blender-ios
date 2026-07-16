/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 *
 * #bpy.types.ObjectModeType — the registrable type behind addon-defined
 * object modes (#OB_MODE_CUSTOM). Modeled on #RenderEngine (`rna_render.cc`).
 */

#include "DNA_object_types.h"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "rna_internal.hh"

#include "BKE_object_modes.hh"

#ifdef RNA_RUNTIME

#  include "MEM_guardedalloc.h"

#  include "BLI_string.hh"
#  include "BLI_string_utils.hh"

#  include "BKE_report.hh"

#  include "RNA_access.hh"

#  include "ED_object.hh"

#  include "UI_interface_c.hh"

#  ifdef WITH_PYTHON
#    include "BPY_extern.hh"
#  endif

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Callback Trampolines
 * \{ */

static void object_mode_enter(ObjectModeType *mt, bContext *C, Object *ob)
{
  extern FunctionRNA *rna_ObjectModeType_enter_func;
  ParameterList list;

  PointerRNA ptr = RNA_pointer_create_discrete(nullptr, mt->rna_ext.srna, mt);
  FunctionRNA *func = rna_ObjectModeType_enter_func;

  RNA_parameter_list_create(&list, &ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  RNA_parameter_set_lookup(&list, "ob", &ob);
  mt->rna_ext.call(C, &ptr, func, &list);

  RNA_parameter_list_free(&list);
}

static void object_mode_exit(ObjectModeType *mt, bContext *C, Object *ob)
{
  extern FunctionRNA *rna_ObjectModeType_exit_func;
  ParameterList list;

  PointerRNA ptr = RNA_pointer_create_discrete(nullptr, mt->rna_ext.srna, mt);
  FunctionRNA *func = rna_ObjectModeType_exit_func;

  RNA_parameter_list_create(&list, &ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  RNA_parameter_set_lookup(&list, "ob", &ob);
  mt->rna_ext.call(C, &ptr, func, &list);

  RNA_parameter_list_free(&list);
}

static void object_mode_flush(ObjectModeType *mt, Object *ob)
{
  extern FunctionRNA *rna_ObjectModeType_flush_func;
  ParameterList list;

  PointerRNA ptr = RNA_pointer_create_discrete(nullptr, mt->rna_ext.srna, mt);
  FunctionRNA *func = rna_ObjectModeType_flush_func;

  RNA_parameter_list_create(&list, &ptr, func);
  RNA_parameter_set_lookup(&list, "ob", &ob);
  mt->rna_ext.call(nullptr, &ptr, func, &list);

  RNA_parameter_list_free(&list);
}

static void object_mode_refresh(ObjectModeType *mt, bContext *C, Object *ob)
{
  extern FunctionRNA *rna_ObjectModeType_refresh_func;
  ParameterList list;

  PointerRNA ptr = RNA_pointer_create_discrete(nullptr, mt->rna_ext.srna, mt);
  FunctionRNA *func = rna_ObjectModeType_refresh_func;

  RNA_parameter_list_create(&list, &ptr, func);
  RNA_parameter_set_lookup(&list, "context", &C);
  RNA_parameter_set_lookup(&list, "ob", &ob);
  mt->rna_ext.call(C, &ptr, func, &list);

  RNA_parameter_list_free(&list);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

static int rna_ObjectModeType_object_types_get(PointerRNA *ptr)
{
  const ObjectModeType *mt = static_cast<const ObjectModeType *>(ptr->data);
  return int(mt->object_type_mask);
}

static void rna_ObjectModeType_object_types_set(PointerRNA *ptr, const int value)
{
  ObjectModeType *mt = static_cast<ObjectModeType *>(ptr->data);
  mt->object_type_mask = uint64_t(uint32_t(value));
}

static bool rna_ObjectModeType_unregister(Main *bmain, StructRNA *type)
{
  ObjectModeType *mt = static_cast<ObjectModeType *>(RNA_struct_blender_type_get(type));

  if (!mt) {
    return false;
  }

  /* Force-exit every object still in this mode: after unregistration the
   * callbacks are gone and the mode bit would dangle. */
  if (bmain) {
    ed::object::custom_mode_exit_all(bmain, mt);
  }

  ui::refresh_for_srna_unregister(bmain, type);

#  ifdef WITH_PYTHON
  if (mt->py_instance) {
    BPY_DECREF_RNA_INVALIDATE(mt->py_instance);
    mt->py_instance = nullptr;
  }
#  endif

  RNA_struct_free_extension(type, &mt->rna_ext);
  RNA_struct_free(&RNA_blender_rna_get(), type);
  BKE_object_mode_type_remove(mt);
  return true;
}

static StructRNA *rna_ObjectModeType_register(Main *bmain,
                                              ReportList *reports,
                                              void *data,
                                              const char *identifier,
                                              StructValidateFunc validate,
                                              StructCallbackFunc call,
                                              StructFreeFunc free)
{
  const char *error_prefix = "Registering object mode class:";
  ObjectModeType dummy_mt = {nullptr};
  bool have_function[4];

  PointerRNA dummy_mt_ptr = RNA_pointer_create_discrete(nullptr, RNA_ObjectModeType, &dummy_mt);

  /* Validate the python class. */
  if (validate(&dummy_mt_ptr, data, have_function) != 0) {
    return nullptr;
  }

  if (strlen(identifier) >= sizeof(dummy_mt.idname)) {
    BKE_reportf(reports,
                RPT_ERROR,
                "%s '%s' is too long, maximum length is %d",
                error_prefix,
                identifier,
                int(sizeof(dummy_mt.idname)));
    return nullptr;
  }

  /* Re-registration replaces the previous type. */
  if (ObjectModeType *mt_old = BKE_object_mode_type_find(dummy_mt.idname)) {
    StructRNA *srna = mt_old->rna_ext.srna;
    if (!(srna && rna_ObjectModeType_unregister(bmain, srna))) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "%s '%s', bl_idname '%s' %s",
                  error_prefix,
                  identifier,
                  dummy_mt.idname,
                  srna ? "is built-in" : "could not be unregistered");
      return nullptr;
    }
  }

  ObjectModeType *mt = MEM_new_uninitialized<ObjectModeType>("ObjectModeType");
  memcpy(mt, &dummy_mt, sizeof(dummy_mt));

  /* The idname is dotted/lowercase (it doubles as the context-mode string);
   * the RNA struct identifier must be a valid identifier, so dots become
   * underscores. The registered struct references the string, so it lives
   * on the type. */
  STRNCPY(mt->srna_idname, mt->idname);
  BLI_string_replace_char(mt->srna_idname, '.', '_');

  mt->rna_ext.srna = RNA_def_struct_ptr(
      &RNA_blender_rna_get(), mt->srna_idname, RNA_ObjectModeType);
  mt->rna_ext.data = data;
  mt->rna_ext.call = call;
  mt->rna_ext.free = free;
  RNA_struct_blender_type_set(mt->rna_ext.srna, mt);

  mt->enter = have_function[0] ? object_mode_enter : nullptr;
  mt->exit = have_function[1] ? object_mode_exit : nullptr;
  mt->flush = have_function[2] ? object_mode_flush : nullptr;
  mt->refresh = have_function[3] ? object_mode_refresh : nullptr;

  if (!BKE_object_mode_type_add(mt)) {
    BKE_reportf(
        reports, RPT_ERROR, "%s '%s' could not be registered", error_prefix, identifier);
    RNA_struct_free_extension(mt->rna_ext.srna, &mt->rna_ext);
    RNA_struct_free(&RNA_blender_rna_get(), mt->rna_ext.srna);
    MEM_delete(mt);
    return nullptr;
  }

  return mt->rna_ext.srna;
}

static void **rna_ObjectModeType_instance(PointerRNA *ptr)
{
  ObjectModeType *mt = static_cast<ObjectModeType *>(ptr->data);
  return &mt->py_instance;
}

static StructRNA *rna_ObjectModeType_refine(PointerRNA *ptr)
{
  ObjectModeType *mt = static_cast<ObjectModeType *>(ptr->data);
  return (mt->rna_ext.srna) ? mt->rna_ext.srna : RNA_ObjectModeType;
}

/** \} */

}  // namespace blender

#else /* RNA_RUNTIME */

namespace blender {

static void rna_def_object_mode_type(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  FunctionRNA *func;
  PropertyRNA *parm;

  /* Object types an addon mode can declare support for (`bl_object_types`).
   * Flag items (values are `1 << OB_*`) so registration stores a plain mask;
   * limited to types that can plausibly own an interactive mode. */
  static const EnumPropertyItem object_mode_object_type_items[] = {
      {1 << OB_MESH, "MESH", 0, "Mesh", ""},
      {1 << OB_CURVES_LEGACY, "CURVE", 0, "Curve", ""},
      {1 << OB_SURF, "SURFACE", 0, "Surface", ""},
      {1 << OB_MBALL, "META", 0, "Metaball", ""},
      {1 << OB_FONT, "FONT", 0, "Text", ""},
      {1 << OB_LATTICE, "LATTICE", 0, "Lattice", ""},
      {1 << OB_ARMATURE, "ARMATURE", 0, "Armature", ""},
      {1 << OB_CURVES, "CURVES", 0, "Hair Curves", ""},
      {1 << OB_POINTCLOUD, "POINTCLOUD", 0, "Point Cloud", ""},
      {1 << OB_VOLUME, "VOLUME", 0, "Volume", ""},
      {1 << OB_GREASE_PENCIL, "GREASEPENCIL", 0, "Grease Pencil", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  srna = RNA_def_struct(brna, "ObjectModeType", nullptr);
  RNA_def_struct_sdna(srna, "ObjectModeType");
  RNA_def_struct_ui_text(
      srna, "Object Mode Type", "Addon-registered object mode (custom interactive mode)");
  RNA_def_struct_refine_func(srna, "rna_ObjectModeType_refine");
  RNA_def_struct_register_funcs(srna,
                                "rna_ObjectModeType_register",
                                "rna_ObjectModeType_unregister",
                                "rna_ObjectModeType_instance");

  /* Callbacks. Order defines the `have_function[]` indexes in register. */
  func = RNA_def_function(srna, "enter", nullptr);
  RNA_def_function_ui_description(
      func, "Build the mode's session state when an object enters the mode");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "ob", "Object", "", "Object entering the mode");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(srna, "exit", nullptr);
  RNA_def_function_ui_description(
      func,
      "Flush and free the mode's session state; also called on forced exits "
      "(object/workspace switch, file close, addon disable)");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "ob", "Object", "", "Object leaving the mode");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(srna, "flush", nullptr);
  RNA_def_function_ui_description(
      func,
      "Write deferred mode state into the object's data so undo, save and "
      "render see a consistent result");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "ob", "Object", "", "Object to flush");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(srna, "refresh", nullptr);
  RNA_def_function_ui_description(
      func, "Re-sync the mode's session state after undo/redo replaced the underlying data");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_pointer(func, "ob", "Object", "", "Object to refresh");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  /* Registration. */
  RNA_define_verify_sdna(false);

  prop = RNA_def_property(srna, "bl_idname", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "idname");
  RNA_def_property_flag(prop, PROP_REGISTER);
  RNA_def_property_ui_text(
      prop,
      "ID Name",
      "Unique mode identifier, also the `context.mode` string while active; "
      "use an addon-prefixed lowercase name (e.g. \"my_addon.sculpt2\")");

  prop = RNA_def_property(srna, "bl_label", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "label");
  RNA_def_property_flag(prop, PROP_REGISTER);
  RNA_def_property_ui_text(prop, "Label", "Mode label for the mode dropdown and header");

  prop = RNA_def_property(srna, "bl_icon", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "icon");
  RNA_def_property_enum_items(prop, rna_enum_icon_items);
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_ui_text(prop, "Icon", "Mode icon for the mode dropdown and header");

  prop = RNA_def_property(srna, "bl_object_types", PROP_ENUM, PROP_NONE);
  RNA_def_property_flag(prop, PROP_REGISTER | PROP_ENUM_FLAG);
  RNA_def_property_enum_items(prop, object_mode_object_type_items);
  RNA_def_property_enum_funcs(prop,
                              "rna_ObjectModeType_object_types_get",
                              "rna_ObjectModeType_object_types_set",
                              nullptr);
  RNA_def_property_ui_text(prop, "Object Types", "Object types the mode can be entered on");

  prop = RNA_def_property(srna, "bl_keymap", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "keymap");
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_ui_text(
      prop, "Keymap", "Name of the keymap active while the mode is active (empty for none)");

  prop = RNA_def_property(srna, "bl_default_tool", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "default_tool");
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_ui_text(
      prop,
      "Default Tool",
      "Identifier of the tool made active when entering the mode (empty uses the generic default)");

  prop = RNA_def_property(srna, "bl_use_custom_undo", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", OBJECT_MODE_TYPE_USE_CUSTOM_UNDO);
  RNA_def_property_flag(prop, PROP_REGISTER_OPTIONAL);
  RNA_def_property_ui_text(
      prop,
      "Use Custom Undo",
      "Use the wrapped custom undo type instead of global (memfile) undo while in the mode");

  RNA_define_verify_sdna(true);
}

void RNA_def_object_mode(BlenderRNA *brna)
{
  rna_def_object_mode_type(brna);
}

}  // namespace blender

#endif /* RNA_RUNTIME */
