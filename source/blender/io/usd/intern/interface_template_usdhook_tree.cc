/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>

#include "BKE_context.hh"
#include "BKE_key.hh"

#include "BLI_listbase.h"
#include "BLT_translation.hh"

#include "DNA_collection_types.h"

#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "DEG_depsgraph.hh"

#include "DNA_key_types.h"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_undo.hh"

#include "usd_hook.hh"

namespace blender::ui {

class UsdHookTreeView : public AbstractTreeView {
  //  protected:

 public:
  wmOperator &op_;
  Collection *col_;
  int idx_;
  UsdHookTreeView(wmOperator &op, Collection *col, int idx) : op_(op), col_(col)
  {
    idx_ = idx;
    is_flat_ = true;
  };

  void build_tree() override;

 private:
  void build_tree_node_recursive(TreeViewItemContainer &parent, const int bcoll_index);
  void build_tree_from_operator();
};

struct UsdHookProxy {
  // char name[64]; /** Should this use the ID max name length? */
  int index;
  PointerRNA data;
  PointerRNA *op;
};

class UsdHookProxyItem : public ui::AbstractTreeViewItem {
 private:
  UsdHookProxy hook_proxy_;

 public:
  UsdHookProxyItem(StringRef name, int index, PointerRNA data, PointerRNA *op)
  {
    label_ = name;
    hook_proxy_.index = index;
    hook_proxy_.data = data;
    hook_proxy_.op = op;
  };

  void build_row(uiLayout &row) override
  {
    bool alert = !RNA_boolean_get(&hook_proxy_.data, "valid");
    set_alert(alert);
    int row_icon = alert ? ICON_ERROR : ICON_BLANK1;
    std::string label = alert ? RNA_string_get(&hook_proxy_.data, "identifier") :
                                RNA_string_get(&hook_proxy_.data, "name");

    uiItemL_ex(&row, label, row_icon, false, false);
    uiLayout *sub = &row.row(true);
    sub->use_property_decorate_set(false);

    sub->prop(&hook_proxy_.data, "enabled", UI_ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);
  }

  std::optional<bool> should_be_active() const override
  {
    int hook_index = RNA_int_get(hook_proxy_.op, "hook_index");
    return (hook_index == hook_proxy_.index);
    return false;
  }

  void on_activate(bContext &C) override
  {
    PropertyRNA *prop = RNA_struct_find_property(hook_proxy_.op, "hook_index");
    RNA_property_int_set(hook_proxy_.op, prop, hook_proxy_.index);
    RNA_property_update(&C, hook_proxy_.op, prop);

    ED_undo_push(&C, "Set Active USD Hook");
  }

  // TODO: This
  // std::optional<bool> should_be_selected() const override
  // {
  //   return true;
  // }

  void set_selected(const bool select) override
  {
    AbstractViewItem::set_selected(select);
  }

  bool supports_renaming() const override
  {
    return false;
  }

  // bool rename(const bContext &C, StringRefNull new_name) override
  // {
  //   PropertyRNA *prop = RNA_struct_find_property(&hook_proxy_.data, "identifier");
  //   RNA_property_string_set(&hook_proxy_.data, prop, new_name.c_str());
  //   ED_undo_push(const_cast<bContext *>(&C), "Rename USD Hook Descriptor");
  //   return true;
  // }

  // StringRef get_rename_string() const override
  // {
  //   return StringRef(RNA_string_get(&hook_proxy_.data, "identifier").c_str());
  // }
};

void UsdHookTreeView::build_tree()
{
  PointerRNA *ptr = op_.ptr;
  int idx = 0;
  RNA_BEGIN (ptr, itemptr, "hooks") {
    this->add_tree_item<UsdHookProxyItem>(fmt::format("{}", idx).c_str(), idx, itemptr, ptr);
    idx += 1;
  }
  RNA_END;
}

void template_hook_list(bContext *C, uiLayout *layout, wmOperator *op)
{
  Collection *active_collection = CTX_data_collection(C);
  if (active_collection == nullptr) {
    return;
  }
  ListBase *exporters = &active_collection->exporters;
  if (BLI_listbase_is_empty(exporters)) {
    return;
  }

  const int index = active_collection->active_exporter_index;

  uiBlock *block = layout->block();
  ui::AbstractTreeView *tree_view = UI_block_add_view(
      *block,
      "USD Hook Tree View",
      std::make_unique<UsdHookTreeView>(*op, active_collection, index));
  tree_view->set_default_rows(4);

  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, *layout);
}
}  // namespace blender::ui
