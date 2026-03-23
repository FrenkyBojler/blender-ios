/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "BKE_context.hh"

#include "ANIM_action.hh"

#include "UI_interface.hh"
#include "UI_tree_view.hh"

#include "ED_undo.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

namespace blender::ui {

namespace action_layer {
using namespace blender::animrig;

class ActionLayerTreeView : public AbstractTreeView {
 protected:
  Action &action_;

 public:
  explicit ActionLayerTreeView(bAction &action);
  void build_tree() override;

  bool listen(const wmNotifier &notifier) const override
  {
    return notifier.data == ND_ANIMCHAN;
  }
};

class ActionLayerDragController : public AbstractViewItemDragController {
 private:
  Action &action_;
  Layer &layer_;

 public:
  ActionLayerDragController(ActionLayerTreeView &tree_view, Action &action, Layer &layer)
      : AbstractViewItemDragController(tree_view), action_(action), layer_(layer){};

  void *create_drag_data() const override
  {
    return &layer_;
  };

  void on_drag_start(bContext &C, AbstractViewItem &item) override
  {
    action_.layer_active_set(layer_);
  };

  std::optional<eWM_DragDataType> get_drag_type() const
  {
    return WM_DRAG_ACTION_LAYER;
  }
};

class ActionLayerItem : public AbstractTreeViewItem {
 private:
  Action &action_;
  int layer_index_;
  Layer &layer_;

 public:
  ActionLayerItem(Action &action, const int layer_index)
      : action_(action), layer_index_(layer_index), layer_(*action.layer(layer_index))
  {
    this->label_ = layer_.name;
  }

  void build_row(Layout &row) override
  {
    uiItemL_ex(&row, layer_.name, ICON_NONE, false, false);
    PointerRNA layer_pointer = RNA_pointer_create_discrete(&action_.id, RNA_ActionLayer, &layer_);
    const int icon = layer_.is_locked() ? ICON_LOCKED : ICON_UNLOCKED;
    row.prop(&layer_pointer, "is_locked", ITEM_R_ICON_ONLY, "", icon);
  }

  std::optional<bool> should_be_active() const override
  {
    return action_.layer_active_get() == &layer_;
  }

  void on_activate(bContext &C) override
  {
    /* Let RNA handle the property change. This makes sure all the notifiers and DEG
     * update calls are properly called. */
    PointerRNA layers_ptr = RNA_pointer_create_discrete(&action_.id, RNA_ActionLayers, &action_);
    PropertyRNA *prop = RNA_struct_find_property(&layers_ptr, "active");
    PointerRNA layer_pointer = RNA_pointer_create_discrete(&action_.id, RNA_ActionLayer, &layer_);

    RNA_property_pointer_set(&layers_ptr, prop, layer_pointer, nullptr);
    RNA_property_update(&C, &layers_ptr, prop);

    /* Using grouped push so repeated changes to the active layer don't clog the undo queue. */
    ED_undo_grouped_push(&C, "Change Action's active layer");
  }

  bool supports_renaming() const override
  {
    const bool is_override = ID_IS_OVERRIDE_LIBRARY(&action_);
    if (ID_IS_LINKED(&action_) && !is_override) {
      return false;
    }
    return true;
  }

  bool rename(const bContext &C, StringRefNull new_name) override
  {
    /* Let RNA handle the renaming. This makes sure all the notifiers and DEG
     * update calls are properly called. */
    PointerRNA layer_pointer = RNA_pointer_create_discrete(&action_.id, RNA_ActionLayer, &layer_);
    PropertyRNA *prop = RNA_struct_find_property(&layer_pointer, "name");

    RNA_property_string_set(&layer_pointer, prop, new_name.c_str());
    RNA_property_update(&const_cast<bContext &>(C), &layer_pointer, prop);

    ED_undo_push(&const_cast<bContext &>(C), "Rename Action Layer");
    return true;
  }

  StringRef get_rename_string() const override
  {
    return layer_.name;
  }

  std::unique_ptr<AbstractViewItemDragController> create_drag_controller() const override
  {
    ActionLayerTreeView &tree_view = static_cast<ActionLayerTreeView &>(get_tree_view());
    return std::make_unique<ActionLayerDragController>(tree_view, action_, layer_);
  }
};

ActionLayerTreeView::ActionLayerTreeView(bAction &action) : action_(action.wrap()) {}

void ActionLayerTreeView::build_tree()
{
  /* Iterating reverse so the layers are ordered to have overwriting layers on top. */
  for (int i = action_.layer_array_num - 1; i >= 0; i--) {
    Layer *layer = action_.layer(i);
    if (!layer) {
      continue;
    }
    add_tree_item<ActionLayerItem>(action_, i);
  }
}

}  // namespace action_layer

void template_action_layer_tree(Layout *layout, bContext *C, bAction *action)
{
  Block *block = layout->block();
  AbstractTreeView *tree_view = block_add_view(
      *block,
      "Action Layer Tree View",
      std::make_unique<action_layer::ActionLayerTreeView>(*action));
  tree_view->set_context_menu_title("Action Layer");
  tree_view->set_default_rows(5);
  TreeViewBuilder::build_tree_view(*C, *tree_view, *layout);
}
}  // namespace blender::ui
