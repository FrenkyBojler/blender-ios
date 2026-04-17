/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <fmt/format.h>

#include "BKE_attribute_storage.hh"
#include "BKE_context.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"

#include "BLI_index_range.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"

#include "DEG_depsgraph.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "WM_types.hh"
#include "WM_api.hh"

#include "ED_mesh.hh"
#include "ED_undo.hh"

namespace blender::ed::mesh::uvmap {

struct UVMapDragData {
  Mesh *mesh;
  char uv_name[sizeof(CustomDataLayer::name)];
};

class UVMapTreeView : public ui::AbstractTreeView {
 protected:
  Mesh &mesh_;

 public:
  UVMapTreeView(Mesh &mesh) : mesh_(mesh)
  {
    is_flat_ = true;
  }

  void build_tree() override;
};

class UVMapDragController : public ui::AbstractViewItemDragController {
 private:
  Mesh &mesh_;
  std::string drag_uv_name_;

 public:
  UVMapDragController(UVMapTreeView &view, Mesh &mesh, StringRefNull drag_uv_name)
    : AbstractViewItemDragController(view), mesh_(mesh), drag_uv_name_(drag_uv_name)
  {
  }

  std::optional<eWM_DragDataType> get_drag_type() const override
  {
    return WM_DRAG_MESH_UV_MAP;
  }

  void *create_drag_data() const override
  {
    UVMapDragData *drag_data = MEM_new<UVMapDragData>(__func__);
    drag_data->mesh = &mesh_;
    STRNCPY(drag_data->uv_name, drag_uv_name_.c_str());
    return drag_data;
  }
};

class UVMapDropTarget : public ui::TreeViewItemDropTarget {
 private:
  Mesh &mesh_;
  std::string drop_uv_name_;

 public:
  UVMapDropTarget(ui::AbstractTreeViewItem &item,
                  ui::DropBehavior behavior,
                  Mesh &mesh,
                  StringRefNull drop_uv_name)
      : TreeViewItemDropTarget(item, behavior), mesh_(mesh), drop_uv_name_(drop_uv_name)
  {
  }

  bool can_drop(const wmDrag &drag, const char ** /*r_disabled_hint*/) const override
  {
    if (drag.type != WM_DRAG_MESH_UV_MAP) {
      return false;
    }

    const UVMapDragData *drag_data = static_cast<const UVMapDragData *>(drag.poin);
    if (drag_data == nullptr) {
      return false;
    }

    return drag_data->mesh == &mesh_;
  }

  std::string drop_tooltip(const ui::DragInfo &drag_info) const override
  {
    const StringRef drag_name = TIP_("UV Map");
    const StringRef drop_name = drop_uv_name_;

    switch (drag_info.drop_location) {
      case ui::DropLocation::Into:
        BLI_assert_unreachable();
        break;
      case ui::DropLocation::Before:
        return fmt::format(fmt::runtime(TIP_("Move {} above {}")), drag_name, drop_name);
      case ui::DropLocation::After:
        return fmt::format(fmt::runtime(TIP_("Move {} below {}")), drag_name, drop_name);
      default:
        BLI_assert_unreachable();
        break;
    }

    return "";
  }

  bool on_drop(bContext *C, const ui::DragInfo &drag_info) const override
  {
    const UVMapDragData *drag_data = static_cast<const UVMapDragData *>(drag_info.drag_data.poin);
    if (drag_data == nullptr || drag_data->mesh != &mesh_) {
      return false;
    }

    if (mesh_.runtime->edit_mesh != nullptr) {
      return false;
    }

    const VectorSet<StringRefNull> uv_names = mesh_.uv_map_names();
    const int from_index = uv_names.index_of_try(drag_data->uv_name);
    const int drop_index = uv_names.index_of_try(drop_uv_name_);
    if (from_index == -1 || drop_index == -1) {
      return false;
    }

    int to_index = drop_index;
    switch (drag_info.drop_location) {
      case ui::DropLocation::Into:
        BLI_assert_unreachable();
        return false;
      case ui::DropLocation::Before:
        to_index -= int(from_index < to_index);
        break;
      case ui::DropLocation::After:
        to_index += int(from_index > to_index);
        break;
    }

    if (from_index == to_index) {
      return false;
    }

    bke::AttributeStorage &attribute_storage = mesh_.attribute_storage.wrap();
    const StringRefNull from_name = drag_data->uv_name;
    const int target_storage_index = attribute_storage.index_of(uv_names[to_index]);
    if (target_storage_index == -1 || !attribute_storage.move(from_name, target_storage_index)) {
      return false;
    }

    DEG_id_tag_update(&mesh_.id, ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_GEOM | ND_DATA, &mesh_);
    ED_undo_push(C, "Drop UV Map");

    return true;
  }
};

class UVMapItem : public ui::AbstractTreeViewItem {
 private:
  Mesh &mesh_;
  std::string uv_name_;

 public:
  UVMapItem(Mesh &mesh, StringRefNull uv_name) : mesh_(mesh), uv_name_(uv_name)
  {
    label_ = uv_name_;
  }

  void build_row(ui::Layout &row) override
  {
    PointerRNA mesh_ptr = RNA_pointer_create_discrete(&mesh_.id, RNA_Mesh, &mesh_);
    PropertyRNA *uv_layers_prop = RNA_struct_find_property(&mesh_ptr, "uv_layers");
    if (uv_layers_prop == nullptr) {
      row.label(uv_name_, ICON_GROUP_UVS);
      return;
    }

    const VectorSet<StringRefNull> uv_names = mesh_.uv_map_names();
    const int index = uv_names.index_of_try(uv_name_);
    if (index == -1) {
      row.label(uv_name_, ICON_GROUP_UVS);
      return;
    }

    PointerRNA layer_ptr;
    if (!RNA_property_collection_lookup_int(&mesh_ptr, uv_layers_prop, index, &layer_ptr)) {
      row.label(uv_name_, ICON_GROUP_UVS);
      return;
    }

    row.use_property_decorate_set(false);
    row.prop(&layer_ptr, "name", ui::ITEM_R_NO_BG, "", ICON_GROUP_UVS);

    ui::Layout &sub = row.row(true);
    sub.use_property_decorate_set(false);
    PropertyRNA *active_render_prop = RNA_struct_find_property(&layer_ptr, "active_render");
    const int render_icon = (active_render_prop && RNA_property_boolean_get(&layer_ptr, active_render_prop)) ?
                                ICON_RESTRICT_RENDER_OFF :
                                ICON_RESTRICT_RENDER_ON;
    sub.prop(&layer_ptr, "active_render", ui::ITEM_R_ICON_ONLY, std::nullopt, render_icon);
  }

  std::optional<bool> should_be_active() const override
  {
    return mesh_.active_uv_map_name() == uv_name_;
  }

  void on_activate(bContext &C) override
  {
    mesh_.uv_maps_active_set(uv_name_);
    BKE_mesh_tessface_clear(&mesh_);
    DEG_id_tag_update(&mesh_.id, ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_GEOM | ND_DATA, &mesh_);

    ED_undo_push(&C, "Set Active UV Map");
  }

  bool supports_renaming() const override
  {
    return true;
  }

  bool rename(const bContext &C, StringRefNull new_name) override
  {
    PointerRNA mesh_ptr = RNA_pointer_create_discrete(&mesh_.id, RNA_Mesh, &mesh_);
    PropertyRNA *uv_layers_prop = RNA_struct_find_property(&mesh_ptr, "uv_layers");
    if (uv_layers_prop == nullptr) {
      return false;
    }

    const VectorSet<StringRefNull> uv_names = mesh_.uv_map_names();
    const int index = uv_names.index_of_try(uv_name_);
    if (index == -1) {
      return false;
    }

    PointerRNA layer_ptr;
    if (!RNA_property_collection_lookup_int(&mesh_ptr, uv_layers_prop, index, &layer_ptr)) {
      return false;
    }

    RNA_string_set(&layer_ptr, "name", new_name.c_str());
    uv_name_ = new_name;
    ED_undo_push(const_cast<bContext *>(&C), "Rename UV Map");
    return true;
  }

  StringRef get_rename_string() const override
  {
    return uv_name_;
  }

  std::unique_ptr<ui::AbstractViewItemDragController> create_drag_controller() const override
  {
    return std::make_unique<UVMapDragController>(
        static_cast<UVMapTreeView &>(get_tree_view()), mesh_, uv_name_);
  }

  std::unique_ptr<ui::TreeViewItemDropTarget> create_drop_target() override
  {
    return std::make_unique<UVMapDropTarget>(
        *this, ui::DropBehavior::Reorder, mesh_, uv_name_);
  }
};

void UVMapTreeView::build_tree()
{
  PointerRNA mesh_ptr = RNA_pointer_create_discrete(&mesh_.id, RNA_Mesh, &mesh_);
  PropertyRNA *uv_layers_prop = RNA_struct_find_property(&mesh_ptr, "uv_layers");
  if (uv_layers_prop == nullptr) {
    return;
  }

  const VectorSet<StringRefNull> uv_names = mesh_.uv_map_names();
  for (const StringRefNull uv_name : uv_names) {
    this->add_tree_item<UVMapItem>(mesh_, uv_name);
  }
}

void template_tree(ui::Layout *layout, bContext *C)
{
  Mesh *mesh = ED_mesh_context(C);
  if (mesh == nullptr) {
    return;
  }

  ui::Block *block = layout->block();
  ui::AbstractTreeView *tree_view = block_add_view(
      *block, "UV Map Tree View", std::make_unique<UVMapTreeView>(*mesh));
  tree_view->set_default_rows(4);

  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, *layout);
}

}  // namespace blender::ed::mesh::uvmap