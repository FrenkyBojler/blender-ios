/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spimage
 */

#include <fmt/format.h>

#include "BKE_context.hh"
#include "BKE_image.hh"

#include "BLI_listbase.h"
#include "BLT_translation.hh"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "RE_pipeline.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_undo.hh"

#include "image_intern.hh"

namespace blender::ed::space_image {

class RenderSlotTreeView : public ui::AbstractTreeView {
 protected:
  Image &image_;
  const bContext *context_;

 public:
  RenderSlotTreeView(Image &image, const bContext *C) : image_(image), context_(C)
  {
    is_flat_ = true;
  }

  void build_tree() override;

  const bContext *get_context() const
  {
    return context_;
  }
};

struct RenderSlotData {
  Image *image;
  RenderSlot *slot;
  int index;
};

class RenderSlotDragController : public ui::AbstractViewItemDragController {
 private:
  RenderSlotData drag_slot_;

 public:
  RenderSlotDragController(RenderSlotTreeView &view, RenderSlotData drag_slot)
      : AbstractViewItemDragController(view), drag_slot_(drag_slot)
  {
  }

  std::optional<eWM_DragDataType> get_drag_type() const override
  {
    return WM_DRAG_RENDER_SLOT;
  }

  void *create_drag_data() const override
  {
    int selected_count = [&]() -> int {
      int count = 0;
      for (const auto [index, slot] : drag_slot_.image->renderslots.enumerate()) {
        if (slot.flag & RENDERSLOT_SEL) {
          count++;
        }
      }
      return count;
    }();

    /* If nothing is selected, include at least the dragged slot. */
    if (selected_count == 0) {
      RenderSlot **drag_data = MEM_new_array_zeroed<RenderSlot *>(2, "Selected Render Slots");
      drag_data[0] = drag_slot_.slot;
      return drag_data;
    }

    /* Allocate one extra element, to use it as null-delimiter. */
    RenderSlot **selected_slots = MEM_new_array_zeroed<RenderSlot *>(selected_count + 1,
                                                                     "Selected Render Slots");

    selected_count = 0;
    for (const auto [index, slot] : drag_slot_.image->renderslots.enumerate()) {
      if (slot.flag & RENDERSLOT_SEL) {
        selected_slots[selected_count] = &slot;
        selected_count++;
      }
    }
    BLI_assert_msg(selected_slots[selected_count] == nullptr,
                   "Expected last element to be null (null-delimiter)");
    return selected_slots;
  }
};

class RenderSlotDropTarget : public ui::TreeViewItemDropTarget {
 private:
  Image &image_;
  RenderSlot &drop_slot_;
  int drop_index_;

 public:
  RenderSlotDropTarget(ui::AbstractTreeViewItem &item,
                       ui::DropBehavior behavior,
                       Image &image,
                       RenderSlot &drop_slot,
                       int index)
      : TreeViewItemDropTarget(item, behavior),
        image_(image),
        drop_slot_(drop_slot),
        drop_index_(index)
  {
  }

  bool can_drop(const wmDrag &drag, const char ** /*r_disabled_hint*/) const override
  {
    if (drag.type != WM_DRAG_RENDER_SLOT) {
      return false;
    }

    const RenderSlot **drag_slots = static_cast<const RenderSlot **>(drag.poin);
    if (!drag_slots || !drag_slots[0]) {
      return false;
    }

    return true;
  }

  std::string drop_tooltip(const ui::DragInfo &drag_info) const override
  {
    const StringRef drag_name = TIP_("Selected Slots");
    const std::string drop_name = drop_slot_.name[0] != '\0' ?
                                      drop_slot_.name :
                                      fmt::format("Slot {}", drop_index_ + 1);

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
    const RenderSlot **drag_slots = static_cast<const RenderSlot **>(drag_info.drag_data.poin);

    if (!drag_slots || !drag_slots[0]) {
      return false;
    }

    for (int8_t i = 0; drag_slots[i] != nullptr; i++) {
      const int drag_index = BLI_findindex(&image_.renderslots, drag_slots[i]);
      int drop_index = BLI_findindex(&image_.renderslots, &drop_slot_);

      if (drag_index == -1) {
        continue;
      }

      switch (drag_info.drop_location) {
        case ui::DropLocation::Into:
          BLI_assert_unreachable();
          break;
        case ui::DropLocation::Before:
          drop_index -= int(drag_index < drop_index);
          break;
        case ui::DropLocation::After:
          drop_index += int(drag_index > drop_index) + i;
          break;
      }

      BKE_image_move_renderslot(&image_, drag_index, drop_index);
    }

    WM_event_add_notifier(C, NC_IMAGE | ND_DRAW, nullptr);
    ED_undo_push(C, "Drop Render Slot");

    return true;
  }
};

class RenderSlotItem : public ui::AbstractTreeViewItem {
 private:
  RenderSlotData slot_data_;

 public:
  RenderSlotItem(Image *image, RenderSlot *slot, int index)
  {
    label_ = slot->name[0] != '\0' ? slot->name : fmt::format("Slot {}", index + 1);
    slot_data_.image = image;
    slot_data_.slot = slot;
    slot_data_.index = index;
  }

  void build_row(ui::Layout &row) override
  {
    /* Determine icon based on slot state, same logic as `ui_imageuser_slot_menu`. */
    const RenderSlotTreeView &tree_view = static_cast<const RenderSlotTreeView &>(
        get_tree_view());
    const bContext *C = tree_view.get_context();

    /* Default to "blank" for nicer alignment. */
    int icon = ICON_BLANK1;

    /* The scene isn't expected to be null, check since it's not a requirement. */
    Scene *scene = CTX_data_scene(C);
    const bool has_active_render = scene && (RE_GetSceneRender(scene) != nullptr);

    if (slot_data_.index == slot_data_.image->last_render_slot) {
      if (has_active_render) {
        icon = ICON_RENDER_RESULT;
      }
    }
    else if (slot_data_.slot->render != nullptr) {
      icon = ICON_DOT;
    }

    uiItemL_ex(&row, this->label_, icon, false, false);
  }

  std::optional<bool> should_be_active() const override
  {
    return slot_data_.image->render_slot == slot_data_.index;
  }

  void on_activate(bContext &C) override
  {
    slot_data_.image->render_slot = slot_data_.index;
    BKE_image_partial_update_mark_full_update(slot_data_.image);

    WM_event_add_notifier(&C, NC_IMAGE | ND_DRAW, nullptr);
    ED_undo_push(&C, "Set Active Render Slot");
  }

  std::optional<bool> should_be_selected() const override
  {
    return slot_data_.slot->flag & RENDERSLOT_SEL;
  }

  void set_selected(const bool select) override
  {
    AbstractViewItem::set_selected(select);
    SET_FLAG_FROM_TEST(slot_data_.slot->flag, select, RENDERSLOT_SEL);
  }

  bool supports_renaming() const override
  {
    return true;
  }

  bool rename(const bContext &C, StringRefNull new_name) override
  {
    PointerRNA slot_ptr = RNA_pointer_create_discrete(
        &slot_data_.image->id, RNA_RenderSlot, slot_data_.slot);
    RNA_string_set(&slot_ptr, "name", new_name.c_str());
    WM_event_add_notifier(const_cast<bContext *>(&C), NC_IMAGE | ND_DRAW, nullptr);
    ED_undo_push(const_cast<bContext *>(&C), "Rename Render Slot");
    return true;
  }

  StringRef get_rename_string() const override
  {
    return slot_data_.slot->name;
  }

  void delete_item(bContext *C) override
  {
    ImageUser *iuser = &CTX_wm_space_image(C)->iuser;
    BKE_image_remove_renderslot(slot_data_.image, iuser, slot_data_.index);
    WM_event_add_notifier(C, NC_IMAGE | ND_DRAW, nullptr);
    ED_undo_grouped_push(C, "Delete Render Slot");
  }

  void build_context_menu(bContext &C, ui::Layout &layout) const override
  {
    MenuType *mt = WM_menutype_find("IMAGE_MT_render_slot_context_menu", true);
    if (!mt) {
      return;
    }
    ui::menutype_draw(&C, mt, &layout);
  }

  std::unique_ptr<ui::AbstractViewItemDragController> create_drag_controller() const override
  {
    return std::make_unique<RenderSlotDragController>(
        static_cast<RenderSlotTreeView &>(get_tree_view()), slot_data_);
  }

  std::unique_ptr<ui::TreeViewItemDropTarget> create_drop_target() override
  {
    return std::make_unique<RenderSlotDropTarget>(
        *this, ui::DropBehavior::Reorder, *slot_data_.image, *slot_data_.slot, slot_data_.index);
  }
};

void RenderSlotTreeView::build_tree()
{
  for (const auto [index, slot] : image_.renderslots.enumerate()) {
    this->add_tree_item<RenderSlotItem>(&image_, &slot, index);
  }
}

void image_render_slot_tree_view_draw(const bContext *C, ui::Layout &layout, Image *image)
{
  if (!image) {
    return;
  }

  ui::Block *block = layout.block();

  ui::AbstractTreeView *tree_view = block_add_view(
      *block,
      "Render Slot Tree View",
      std::make_unique<RenderSlotTreeView>(*image, C));
  tree_view->set_context_menu_title("Render Slot");
  tree_view->set_default_rows(3);
  tree_view->allow_multiselect_items();

  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, layout);
}

}  // namespace blender::ed::space_image
