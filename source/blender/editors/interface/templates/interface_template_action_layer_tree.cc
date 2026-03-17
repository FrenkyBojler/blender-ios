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
  bool listen(const wmNotifier &notifier) const override;
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
};

ActionLayerTreeView::ActionLayerTreeView(bAction &action) : action_(action.wrap()) {}

void ActionLayerTreeView::build_tree()
{
  for (int i = 0; i < action_.layer_array_num; i++) {
    Layer *layer = action_.layer(i);
    if (!layer) {
      continue;
    }
    add_tree_item<ActionLayerItem>(action_, i);
  }
}

bool ActionLayerTreeView::listen(const wmNotifier &notifier) const
{
  return notifier.data == ND_ANIMCHAN;
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
