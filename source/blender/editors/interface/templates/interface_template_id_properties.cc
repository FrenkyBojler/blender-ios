/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "BLI_listbase.h"

#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "ED_undo.hh"

#include "WM_api.hh"
#include "WM_message.hh"

namespace blender::ui::id_properties {

class IDPropertyView : public AbstractTreeView {
 public:
  explicit IDPropertyView(ID *id) : id_(id) {}

  void build_tree() override;

 private:
  ID *id_;
};

class IDPropertyItem : public AbstractTreeViewItem {
 private:
  ID *id_;
  IDProperty *property_;
  int index_;

 public:
  IDPropertyItem(ID *id, IDProperty *property, int index)
      : id_(id), property_(property), index_(index)
  {
  }

  void build_row(ui::Layout &row) override
  {
    uiItemL_ex(&row, property_->name, ICON_NONE, false, false);
  }

  std::optional<bool> should_be_active() const override
  {
    return id_->idprop_active_index == index_;
  }

  void on_activate(bContext &C) override
  {
    PointerRNA id_ptr = RNA_pointer_create_discrete(id_, &RNA_ID, id_);
    PropertyRNA *prop = RNA_struct_find_property(&id_ptr, "idprop_active_index");
    RNA_property_int_set(&id_ptr, prop, index_);
    RNA_property_update(&C, &id_ptr, prop);

    ED_undo_push(&C, "Set Active IDProperty");
  }
};

void IDPropertyView::build_tree()
{
  if (!id_->properties) {
    return;
  }
  int index = 0;
  LISTBASE_FOREACH_INDEX (IDProperty *, id_property, &id_->properties->data.group, index) {
    this->add_tree_item<IDPropertyItem>(id_, id_property, index);
  }
}

void template_tree(ui::Layout *layout, bContext *C, ID *id)
{
  //   Object *ob = CTX_data_active_object(C);
  //   if (ob == nullptr) {
  //     return;
  //   }

  Block *block = layout->block();

  ui::AbstractTreeView *tree_view = block_add_view(
      *block, "Shape Key Tree View", std::make_unique<IDPropertyView>(id));
  tree_view->set_context_menu_title("ID Property");
  tree_view->set_default_rows(4);

  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, *layout);
}

void draw_id_properties_value(ui::Layout *layout, bContext *C, ID *id)
{
  if (!id->properties) {
    return;
  }
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);

  IDProperty *active_prop = static_cast<IDProperty *>(
      BLI_findlink(&id->properties->data.group, id->idprop_active_index));

  auto get_prop_type = [&](const char type) {
    switch (type) {
      case IDP_INT:
        return &RNA_IDPropertyUIDataInt;
      case IDP_FLOAT:
      case IDP_DOUBLE:
        return &RNA_IDPropertyUIDataFloat;
      case IDP_BOOLEAN:
        return &RNA_IDPropertyUIDataBool;
      case IDP_STRING:
        return &RNA_IDPropertyUIDataString;
      default:
        BLI_assert_unreachable();
    }
  };

  StructRNA *srna = active_prop->type == IDP_ARRAY ? get_prop_type(active_prop->subtype) :
                                                     get_prop_type(active_prop->type);

  PointerRNA prop_ptr = RNA_pointer_create_discrete(id, &RNA_IDProperty, active_prop);
  layout->prop(&prop_ptr, "type", UI_ITEM_NONE, "Type", ICON_NONE);

  PointerRNA propui_ptr = RNA_pointer_create_discrete(id, srna, active_prop->ui_data);
  layout->prop(&propui_ptr, "default_value", UI_ITEM_NONE, "Default Value", ICON_NONE);
  if (ELEM(srna, &RNA_IDPropertyUIDataInt, &RNA_IDPropertyUIDataFloat)) {
    layout->prop(&propui_ptr, "soft_min", UI_ITEM_NONE, "Soft Min", ICON_NONE);
    layout->prop(&propui_ptr, "soft_max", UI_ITEM_NONE, "Soft Max", ICON_NONE);
    layout->prop(&propui_ptr, "min", UI_ITEM_NONE, "Hard Min", ICON_NONE);
    layout->prop(&propui_ptr, "max", UI_ITEM_NONE, "Hard Max", ICON_NONE);
    layout->prop(&propui_ptr, "step", UI_ITEM_NONE, "Step", ICON_NONE);
  }
  layout->prop(&propui_ptr, "description", UI_ITEM_NONE, "Description", ICON_NONE);
}

}  // namespace blender::ui::id_properties
