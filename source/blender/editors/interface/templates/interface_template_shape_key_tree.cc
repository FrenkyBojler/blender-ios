/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "BKE_context.hh"
#include "BKE_key.hh"

#include "BLI_listbase.h"
#include "BLT_translation.hh"

#include "UI_interface.hh"
#include "UI_tree_view.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "DNA_key_types.h"

#include "ED_undo.hh"

#include "WM_api.hh"

#include <fmt/format.h>

namespace blender::ui::shapekey {

class ShapeKeyTreeView : public AbstractTreeView {
 protected:
  Object &object_;

 public:
  ShapeKeyTreeView(Object &ob) : object_(ob) {};

  void build_tree() override;
};

class ShapeKeyItem : public AbstractTreeViewItem {
 private:
  Object &object_;
  Key &key_;
  KeyBlock &kb_;
  int index_;

 public:
  ShapeKeyItem(Object &object, Key &key, KeyBlock &kb, int index)
      : object_(object), key_(key), kb_(kb), index_(index)
  {
    this->label_ = kb_.name;
  };

  void build_row(uiLayout &row) override
  {
    uiBut *but = uiItemL_ex(&row, this->label_, ICON_SHAPEKEY_DATA, false, false);
    uiLayout *sub = &row.row(true);
    uiLayoutSetPropDecorate(sub, false);
    PointerRNA shapekey_ptr = RNA_pointer_create_discrete(&key_.id, &RNA_ShapeKey, &kb_);
    sub->prop(&shapekey_ptr, "mute", UI_ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);
    sub->prop(&shapekey_ptr, "lock_shape", UI_ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);
  }

  void on_activate(bContext &C) override
  {
    PointerRNA object_ptr = RNA_pointer_create_discrete(
        &object_.id, &RNA_Object, &object_);
    PropertyRNA *prop = RNA_struct_find_property(&object_ptr, "active_shape_key_index");
    RNA_property_int_set(&object_ptr, prop, index_);
    RNA_property_update(&C, &object_ptr, prop);

    ED_undo_push(&C, "Active Shape Key");
  }
  bool supports_renaming() const override
  {
    return false;
  }
  // bool rename(const bContext &C, StringRefNull new_name) override {}
  // StringRef get_rename_string() const override {}
};

void ShapeKeyTreeView::build_tree()
{
  Key *key = BKE_key_from_object(&object_);
  if (key == nullptr) {
    return;
  }
  int index = 1;
  LISTBASE_FOREACH_INDEX (KeyBlock *, kb, &key->block, index) {
    this->add_tree_item<ShapeKeyItem>(object_, *key, *kb, index);
  }
}

}  // namespace blender::ui::shapekey

void uiTemplateShapeKeyTree(uiLayout *layout, bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return;
  }

  uiBlock *block = uiLayoutGetBlock(layout);

  blender::ui::AbstractTreeView *tree_view = UI_block_add_view(
      *block,
      "Shape Key Tree View",
      std::make_unique<blender::ui::shapekey::ShapeKeyTreeView>(*ob));
  tree_view->set_context_menu_title("Shape Key");
  tree_view->set_default_rows(3);

  blender::ui::TreeViewBuilder::build_tree_view(*C, *tree_view, *layout);
}
