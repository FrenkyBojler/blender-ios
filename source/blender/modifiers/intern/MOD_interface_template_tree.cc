/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <fmt/format.h>

#include "BKE_context.hh"
#include "BKE_object.hh"
#include "BKE_modifier.hh"
#include "BKE_report.hh"

#include "BLI_listbase.h"
#include "BLT_translation.hh"

#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "DEG_depsgraph.hh"

#include "DEG_depsgraph_build.hh"
#include "DNA_modifier_types.h"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_undo.hh"
#include "ED_object.hh"

#include "MOD_ui_common.hh"

namespace blender::modifier {

class ModifierTreeView : public ui::AbstractTreeView {
 protected:
  Object &object_;
  Scene &scene_;

 public:
  ModifierTreeView(Object &ob, Scene &scene) : object_(ob), scene_(scene)
  {
    is_flat_ = true;
  };

  void build_tree() override;
};

struct wmDragdataModifier {
  ModifierData *modifier_data;
};

class ModifierDragController : public ui::AbstractViewItemDragController {
  ModifierData *drag_md_;
  public:
  ModifierDragController(ModifierTreeView &view, ModifierData *md)
      : AbstractViewItemDragController(view), drag_md_(md)
  {
  }

  std::optional<eWM_DragDataType> get_drag_type() const override
  {
    return WM_DRAG_MODIFIER;
  }

  void *create_drag_data() const override
  {
    wmDragdataModifier *drag_data = MEM_new_zeroed<wmDragdataModifier>(__func__);
    drag_data->modifier_data = drag_md_;
    return drag_data;
  }
};

class ModifierDropTarget : public ui::TreeViewItemDropTarget {
    ModifierData *md_;

 public:

    ModifierDropTarget(ui::AbstractTreeViewItem &item,
                     ui::DropBehavior behavior,
                     ModifierData *md)
      : TreeViewItemDropTarget(item, behavior), md_(md)
  {
  }

   bool can_drop(const wmDrag &drag, const char ** /*r_disabled_hint*/) const override
   {
    return drag.type == WM_DRAG_MODIFIER;
   }

  std::string drop_tooltip(const ui::DragInfo &drag_info) const override
  {
    const wmDragdataModifier *drag_data = static_cast<const wmDragdataModifier *>(drag_info.drag_data.poin);
    const StringRef drag_name = drag_data->modifier_data->name;
    const StringRef drop_name = md_->name;

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
    Object *ob = CTX_data_active_object(C);
    wmDragdataModifier *drag_data = static_cast<wmDragdataModifier *>(drag_info.drag_data.poin);
    int drop_index = BLI_findindex(&ob->modifiers, md_);
    const int drag_index = BLI_findindex(&ob->modifiers, drag_data->modifier_data);
    switch (drag_info.drop_location) {
      case ui::DropLocation::Into:
        BLI_assert_unreachable();
        break;
      case ui::DropLocation::Before:
        if (drop_index == 0) {
          break;
        }
        drop_index -= drag_index < drop_index;
        break;
      case ui::DropLocation::After:
        drop_index -= drag_index > drop_index;
        break;
    }
    ed::object::modifier_move_to_index(nullptr, RPT_WARNING, ob, drag_data->modifier_data, drop_index, true);
    ED_undo_push(C, "Reorder Modifier");
    return true;
  }
};

class ModifierItem : public ui::AbstractTreeViewItem {
  Scene &scene_;
  Object &object_;
  ModifierData *modifier_data_;
  int index_;
  public:
  ModifierItem(Scene &scene, Object &object, ModifierData *md, int index) : scene_(scene), object_(object), modifier_data_(md), index_(index)
  {
    label_ = modifier_data_->name;
  };

  void build_row(ui::Layout &row) override
  {
    modifier_row_draw(row, object_, modifier_data_, scene_, index_);
  }

  std::optional<bool> should_be_active() const override
  {
    return modifier_data_->flag & eModifierFlag_Active;
  }

  void on_activate(bContext &C) override
  {
    BKE_object_modifier_set_active(&object_, modifier_data_);
    WM_event_add_notifier(&C, NC_OBJECT | ND_MODIFIER, &object_);

    ED_undo_push(&C, "Active Modifier");
  }

  bool supports_renaming() const override
  {
    return true;
  }

  StringRef get_rename_string() const override
  {
    return label_;
  }

  bool rename(const bContext &C, StringRefNull new_name) override
  {
    PointerRNA ptr = RNA_pointer_create_discrete(
        &object_.id, RNA_Modifier, modifier_data_);
    /* Call rna setter as it already handles the unique names. */
    PropertyRNA *prop = RNA_struct_find_property(&ptr, "name");
    RNA_property_string_set(&ptr, prop, new_name.c_str());
    RNA_property_update(&const_cast<bContext &>(C), &ptr, prop);
    ED_undo_push(const_cast<bContext *>(&C), "Rename Modifier");
    return true;
  }

  std::unique_ptr<ui::AbstractViewItemDragController> create_drag_controller() const override
  {
    return std::make_unique<ModifierDragController>(
        static_cast<ModifierTreeView &>(get_tree_view()), modifier_data_);
  }

  std::unique_ptr<ui::TreeViewItemDropTarget> create_drop_target() override
  {
    return std::make_unique<ModifierDropTarget>(
        *this, ui::DropBehavior::Reorder, modifier_data_);
  }
};

void ModifierTreeView::build_tree()
{
  for (const auto [index, md] : object_.modifiers.enumerate()) {
    this->add_tree_item<ModifierItem>(scene_, object_, &md, index);
  }
}

void template_tree(ui::Layout *layout, bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return;
  }

  ui::Block *block = layout->block();

  ui::AbstractTreeView *tree_view = block_add_view(
      *block,
      "Modifier Tree View",
      std::make_unique<ModifierTreeView>(*ob, *CTX_data_scene(C)));
  tree_view->set_context_menu_title("Modifier");
  tree_view->set_default_rows(4);

  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, *layout);
}
}