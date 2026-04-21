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
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"

#include "DEG_depsgraph.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_mesh.hh"
#include "ED_undo.hh"

namespace blender::ed::mesh::uvmap {

static std::optional<std::string> uv_name_from_uid(const VectorSet<StringRefNull> &uv_names,
                                                   int uv_uid)
{
  if (!uv_names.index_range().contains(uv_uid)) {
    return std::nullopt;
  }
  return std::string(uv_names[uv_uid]);
}

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
struct UVMap {
    Mesh &mesh;
    int uid;
  };

class UVMapDragController : public ui::AbstractViewItemDragController {
 private:
  UVMap drag_uv_;
  
 public:
  UVMapDragController(UVMapTreeView &view, Mesh &mesh, int drag_uv_uid)
      : AbstractViewItemDragController(view), drag_uv_{mesh, drag_uv_uid}
  {
  }

  std::optional<eWM_DragDataType> get_drag_type() const override
  {
    return WM_DRAG_MESH_UV_MAP;
  }

  void *create_drag_data() const override
  {
    Vector<int> selected_uv_uids;
    view_.foreach_view_item([&](ui::AbstractViewItem &item) {
      if (!item.is_selected()) {
        return;
      }
      const std::string name(item.get_rename_string());
      if (name.empty()) {
        return;
      }
      const int uv_uid = drag_uv_.mesh.uv_map_names().index_of_try(name.c_str());
      if (uv_uid != -1) {
        selected_uv_uids.append(uv_uid);
      }
    });

    const int drag_uv_uid = drag_uv_.uid;
    bool has_drag_uv_uid = false;
    for (const int uid : selected_uv_uids) {
      if (uid == drag_uv_uid) {
        has_drag_uv_uid = true;
        break;
      }
    }
    if (drag_uv_uid != -1 && !has_drag_uv_uid) {
      selected_uv_uids.append(drag_uv_uid);
    }

    if (selected_uv_uids.is_empty()) {
      return nullptr;
    }

    int *selected_uv_maps = MEM_new_array_zeroed<int>(selected_uv_uids.size() + 2,
                                                      "Selected UV Maps");
    selected_uv_maps[0] = int(drag_uv_.mesh.id.session_uid);
    for (const int i : selected_uv_uids.index_range()) {
      selected_uv_maps[i + 1] = selected_uv_uids[i];
    }
    selected_uv_maps[selected_uv_uids.size() + 1] = -1;

    return selected_uv_maps;
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

    const int *drag_uv_maps = static_cast<const int *>(drag.poin);
    if (drag_uv_maps == nullptr || drag_uv_maps[1] == -1) {
      return false;
    }

    return drag_uv_maps[0] == int(mesh_.id.session_uid);
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
    const int *drag_uv_maps = static_cast<const int *>(drag_info.drag_data.poin);
    if (drag_uv_maps == nullptr || drag_uv_maps[1] == -1 || drag_uv_maps[0] != int(mesh_.id.session_uid)) {
      return false;
    }

    if (mesh_.runtime->edit_mesh != nullptr) {
      return false;
    }

    const VectorSet<StringRefNull> initial_uv_names = mesh_.uv_map_names();
    Vector<std::string> drag_uv_names;
    for (int i = 1; drag_uv_maps[i] != -1; i++) {
      const std::optional<std::string> drag_uv_name = uv_name_from_uid(initial_uv_names,
                                                                       drag_uv_maps[i]);
      if (drag_uv_name) {
        drag_uv_names.append(*drag_uv_name);
      }
    }

    if (drag_uv_names.is_empty()) {
      return false;
    }

    const int from_index = initial_uv_names.index_of_try(drag_uv_names.first());
    const int drop_index = initial_uv_names.index_of_try(drop_uv_name_);
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

    for (const int i : drag_uv_names.index_range()) {
      const VectorSet<StringRefNull> current_uv_names = mesh_.uv_map_names();
      const StringRefNull drag_uv_name(drag_uv_names[i].c_str());
      const int drag_index = current_uv_names.index_of_try(drag_uv_name);
      if (drag_index == -1) {
        continue;
      }

      if (i > 0) {
        /* Place subsequent items directly after the previously moved item. */
        to_index += int(drag_index > to_index);
      }

      if (to_index < 0 || to_index >= current_uv_names.size()) {
        return false;
      }

      bke::AttributeStorage &attribute_storage = mesh_.attribute_storage.wrap();
      const int target_storage_index = attribute_storage.index_of(current_uv_names[to_index]);
      if (target_storage_index == -1 ||
          !attribute_storage.move(drag_uv_name, target_storage_index))
      {
        return false;
      }
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
    const int render_icon = (active_render_prop &&
                             RNA_property_boolean_get(&layer_ptr, active_render_prop)) ?
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
        static_cast<UVMapTreeView &>(get_tree_view()), mesh_, mesh_.uv_map_names().index_of_try(uv_name_));
  }

  std::unique_ptr<ui::TreeViewItemDropTarget> create_drop_target() override
  {
    return std::make_unique<UVMapDropTarget>(*this, ui::DropBehavior::Reorder, mesh_, uv_name_);
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
  tree_view->allow_multiselect_items();

  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, *layout);
}

}  // namespace blender::ed::mesh::uvmap
