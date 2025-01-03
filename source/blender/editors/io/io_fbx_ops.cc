/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editor/io
 */

#ifdef WITH_IO_FBX

#  include "BKE_context.hh"
#  include "BKE_report.hh"

#  include "BLI_string.h"

#  include "WM_api.hh"

#  include "DNA_space_types.h"

#  include "ED_outliner.hh"

#  include "RNA_access.hh"
#  include "RNA_define.hh"

#  include "BLT_translation.hh"

#  include "UI_interface.hh"

#  include "IO_fbx.hh"
#  include "IO_orientation.hh"
#  include "io_fbx_ops.hh"
#  include "io_utils.hh"

static int wm_fbx_import_exec(bContext *C, wmOperator *op)
{
  FBXImportParams params;
  params.forward_axis = eIOAxis(RNA_enum_get(op->ptr, "forward_axis"));
  params.up_axis = eIOAxis(RNA_enum_get(op->ptr, "up_axis"));
  params.global_scale = RNA_float_get(op->ptr, "global_scale");
  params.use_custom_normals = RNA_boolean_get(op->ptr, "use_custom_normals");
  params.use_custom_props = RNA_boolean_get(op->ptr, "use_custom_props");
  params.use_subsurf = RNA_boolean_get(op->ptr, "use_subsurf");
  params.validate_meshes = RNA_boolean_get(op->ptr, "validate_meshes");

  params.reports = op->reports;

  const auto paths = blender::ed::io::paths_from_operator_properties(op->ptr);

  if (paths.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No filepath given");
    return OPERATOR_CANCELLED;
  }
  for (const auto &path : paths) {
    STRNCPY(params.filepath, path.c_str());
    FBX_import(C, params);
  }

  Scene *scene = CTX_data_scene(C);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);
  ED_outliner_select_sync_from_object_tag(C);

  return OPERATOR_FINISHED;
}

static bool wm_fbx_import_check(bContext * /*C*/, wmOperator *op)
{
  const int num_axes = 3;
  /* Both forward and up axes cannot be the same (or same except opposite sign). */
  if (RNA_enum_get(op->ptr, "forward_axis") % num_axes ==
      (RNA_enum_get(op->ptr, "up_axis") % num_axes))
  {
    RNA_enum_set(op->ptr, "up_axis", RNA_enum_get(op->ptr, "up_axis") % num_axes + 1);
    return true;
  }
  return false;
}

static void ui_fbx_import_settings(const bContext *C, uiLayout *layout, PointerRNA *ptr)
{
  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);

  if (uiLayout *panel = uiLayoutPanel(C, layout, "FBX_import_general", false, IFACE_("General"))) {
    uiLayout *col = uiLayoutColumn(panel, false);
    uiItemR(col, ptr, "global_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    uiItemR(col, ptr, "forward_axis", UI_ITEM_NONE, IFACE_("Forward Axis"), ICON_NONE);
    uiItemR(col, ptr, "up_axis", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  if (uiLayout *panel = uiLayoutPanel(C, layout, "FBX_import_options", false, IFACE_("Options"))) {
    uiLayout *col = uiLayoutColumn(panel, false);
    uiItemR(col, ptr, "use_custom_normals", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    uiItemR(col, ptr, "use_custom_props", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    uiItemR(col, ptr, "use_subsurf", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    uiItemR(col, ptr, "validate_meshes", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

static void wm_fbx_import_draw(bContext *C, wmOperator *op)
{
  ui_fbx_import_settings(C, op->layout, op->ptr);
}

void WM_OT_fbx_import(wmOperatorType *ot)
{
  PropertyRNA *prop;

  ot->name = "Import FBX";
  ot->description = "Import FBX file into current scene";
  ot->idname = "WM_OT_fbx_import";

  ot->invoke = blender::ed::io::filesel_drop_import_invoke;
  ot->exec = wm_fbx_import_exec;
  ot->poll = WM_operator_winactive;
  ot->check = wm_fbx_import_check;
  ot->ui = wm_fbx_import_draw;
  ot->flag = OPTYPE_UNDO | OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_FILES | WM_FILESEL_DIRECTORY |
                                     WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  RNA_def_float(ot->srna, "global_scale", 1.0f, 1e-6f, 1e6f, "Scale", "", 0.001f, 1000.0f);
  RNA_def_enum(ot->srna, "forward_axis", io_transform_axis, IO_AXIS_Y, "Forward Axis", "");
  RNA_def_enum(ot->srna, "up_axis", io_transform_axis, IO_AXIS_Z, "Up Axis", "");

  RNA_def_boolean(ot->srna,
                  "use_custom_normals",
                  true,
                  "Custom Normals",
                  "Import custom normals, if available (otherwise Blender will compute them)");
  RNA_def_boolean(ot->srna,
                  "use_custom_props",
                  true,
                  "Custom Properties",
                  "Import user properties as custom properties");
  RNA_def_boolean(ot->srna,
                  "use_subsurf",
                  false,
                  "Subdivision Data",
                  "Import FBX subdivision information as subdivision surface modifiers");
  RNA_def_boolean(
      ot->srna,
      "validate_meshes",
      true,
      "Validate Meshes",
      "Ensure the data is valid "
      "(when disabled, data may be imported which causes crashes displaying or editing)");

  /* Only show `.fbx` files by default. */
  prop = RNA_def_string(ot->srna, "filter_glob", "*.fbx", 0, "Extension Filter", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

#endif /* WITH_IO_FBX */
