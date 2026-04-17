/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <fmt/format.h>

#include "BKE_context.hh"
#include "BKE_key.hh"
#include "BKE_object.hh"
#include "BKE_modifier.hh"

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