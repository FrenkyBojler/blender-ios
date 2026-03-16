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
  ActionLayer &layer_;

 public:
  ActionLayerItem(Action &action, const int layer_index)
      : action_(action), layer_index_(layer_index), layer_(*action.layer(layer_index))
  {
    this->label_ = layer_.name;
  }

  void build_row(Layout &row) override
  {
    Button *name_label = uiItemL_ex(&row, layer_.name, ICON_NONE, false, false);
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
