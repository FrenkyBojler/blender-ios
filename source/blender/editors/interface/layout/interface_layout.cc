/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

#include "MEM_guardedalloc.h"

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"

#include "BLI_array.hh"
#include "BLI_enum_flags.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_path_utils.hh"
#include "BLI_rect.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_path_templates.hh"
#include "BKE_screen.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"

#include "ED_id_management.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_intern.hh"

#include "layout_intern.hh"
#include "layout_containers.hh"

namespace blender {

namespace ui {

struct ButtonItem;

/* Show an icon button after each RNA button to use to quickly set keyframes,
 * this is a way to display animation/driven/override status, see #54951. */
#define UI_PROP_DECORATE
/* Alternate draw mode where some buttons can use single icon width,
 * giving more room for the text at the expense of nicely aligned text. */
#define UI_PROP_SEP_ICON_WIDTH_EXCEPTION

/* -------------------------------------------------------------------- */
/** \name Structs and Defines
 * \{ */

Item::Item(ItemType type) : type_{type} {}

ItemType Item::type() const
{
  return type_;
};

/** \} */

/* TODO Jean-Silas: Find a better place for this! */
Layout::Layout(ItemType type, LayoutRoot *root) : Item(type), root_{root} {};

/* -------------------------------------------------------------------- */
/** \name Item
 * \{ */

int2 Item::size() const
{
  if (this->type() == ItemType::Button) {
    const ButtonItem *bitem = static_cast<const ButtonItem *>(this);
    return {int(BLI_rctf_size_x(&bitem->but->rect)), int(BLI_rctf_size_y(&bitem->but->rect))};
  }
  return static_cast<const Layout *>(this)->size();
}

int2 Layout::offset() const
{
  return {x_, y_};
}

int2 Layout::size() const
{
  return {w_, h_};
}

int2 Item::offset() const
{
  if (this->type() == ItemType::Button) {
    const ButtonItem *bitem = static_cast<const ButtonItem *>(this);
    return {int(bitem->but->rect.xmin), int(bitem->but->rect.ymin)};
  }
  return {0, 0};
}

void LayoutInternal::layout_offset_size_set(Layout *layout, int x, int y, int w, int h)
{
  layout->x_ = x;
  layout->y_ = y;
  layout->w_ = w;
  layout->h_ = h;
}

void LayoutInternal::layout_move(Layout *layout, int delta_xmin, int delta_xmax)
{
  if (delta_xmin > 0) {
    layout->x_ += delta_xmin;
  }
  else {
    layout->w_ += delta_xmax;
  }
}

void LayoutInternal::layout_space_set(Layout *layout, int space)
{
  layout->space_ = space;
}

int LayoutInternal::layout_space_get(Layout *layout)
{
  return layout->space_;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Special RNA Items
 * \{ */

LayoutDirection Layout::local_direction() const
{
  switch (this->type()) {
    case ItemType::LayoutRow:
    case ItemType::LayoutRoot:
    case ItemType::LayoutOverlap:
    case ItemType::LayoutPanelHeader:
    case ItemType::LayoutGridFlow:
      return LayoutDirection::Horizontal;
    case ItemType::LayoutColumn:
    case ItemType::LayoutColumnFlow:
    case ItemType::LayoutSplit:
    case ItemType::LayoutAbsolute:
    case ItemType::LayoutBox:
    case ItemType::LayoutPanelBody:
    default:
      return LayoutDirection::Vertical;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Button Items
 * \{ */

PointerRNA Layout::op(wmOperatorType *ot,
                      std::optional<StringRef> name,
                      const int icon,
                      const wm::OpCallContext context,
                      const eUI_Item_Flag flag)
{
  PointerRNA ptr;
  uiItemFullO_ptr_ex(this, ot, name, icon, context, flag, &ptr);
  return ptr;
}

PointerRNA Layout::op_menu_hold(wmOperatorType *ot,
                                std::optional<StringRef> name,
                                int icon,
                                const wm::OpCallContext context,
                                const eUI_Item_Flag flag,
                                const char *menu_id)
{
  PointerRNA ptr;
  Button *but = uiItemFullO_ptr_ex(this, ot, name, icon, context, flag, &ptr);
  button_func_hold_set(but, item_menu_hold, BLI_strdup(menu_id));
  return ptr;
}

PointerRNA Layout::op(const StringRefNull opname,
                      const std::optional<StringRef> name,
                      int icon,
                      wm::OpCallContext context,
                      const eUI_Item_Flag flag)
{
  wmOperatorType *ot = WM_operatortype_find(opname.c_str(), false); /* print error next */
  UI_OPERATOR_ERROR_RET(ot, opname.c_str(), "UILayout.operator()");
  return this->op(ot, name, icon, context, flag);
}

BLI_INLINE bool layout_is_radial(const Layout *layout)
{
  return (layout->type() == ItemType::LayoutRadial) ||
         ((layout->type() == ItemType::LayoutRoot) &&
          (layout->root()->type == LayoutType::PieMenu));
}

void Layout::op_enum_items(wmOperatorType *ot,
                           const PointerRNA &ptr,
                           PropertyRNA *prop,
                           IDProperty *properties,
                           wm::OpCallContext context,
                           eUI_Item_Flag flag,
                           const EnumPropertyItem *item_array,
                           int totitem,
                           int active)
{
  const StringRefNull propname = RNA_property_identifier(prop);
  if (RNA_property_type(prop) != PROP_ENUM) {
    RNA_warning_bare("UILayout.operator_enum_items(): %s.%s, not an enum type",
                     RNA_struct_identifier(ptr.type),
                     propname.c_str());
    return;
  }

  Layout *target, *split = nullptr;
  Block *block = this->block();
  const bool radial = layout_is_radial(this);

  if (radial) {
    target = &this->menu_pie();
  }
  else if ((this->local_direction() == LayoutDirection::Horizontal) && (flag & ITEM_R_ICON_ONLY)) {
    target = this;
    block_layout_set_current(block, target);

    /* Add a blank button to the beginning of the row. */
    uiDefIconBut(block,
                 ButtonType::Label,
                 ICON_BLANK1,
                 0,
                 0,
                 1.25f * UI_UNIT_X,
                 UI_UNIT_Y,
                 nullptr,
                 0,
                 0,
                 std::nullopt);
  }
  else {
    split = &this->split(0.0f, false);
    target = &split->column(this->align());
  }

  bool last_iter = false;
  const EnumPropertyItem *item = item_array;
  for (int i = 1; item->identifier && !last_iter; i++, item++) {
    /* Handle over-sized pies. */
    if (radial && (totitem > PIE_MAX_ITEMS) && (i >= PIE_MAX_ITEMS)) {
      if (item->name) { /* only visible items */
        const EnumPropertyItem *tmp;

        /* Check if there are more visible items for the next level. If not, we don't
         * add a new level and add the remaining item instead of the 'more' button. */
        for (tmp = item + 1; tmp->identifier; tmp++) {
          if (tmp->name) {
            break;
          }
        }

        if (tmp->identifier) { /* only true if loop above found item and did early-exit */
          pie_menu_level_create(
              block, ot, propname, properties, item_array, totitem, context, flag);
          /* break since rest of items is handled in new pie level */
          break;
        }
        last_iter = true;
      }
      else {
        continue;
      }
    }

    if (item->identifier[0]) {
      PointerRNA tptr = target->op(
          ot, (flag & ITEM_R_ICON_ONLY) ? nullptr : item->name, item->icon, context, flag);
      if (properties) {
        IDP_CopyPropertyContent(tptr.data_as<IDProperty>(), properties);
      }
      RNA_property_enum_set(&tptr, prop, item->value);

      Button *but = block->buttons_ptrs.last().get();

      if (active == (i - 1)) {
        but->flag |= UI_SELECT_DRAW;
      }

      but_tip_from_enum_item(but, item);
    }
    else {
      if (item->name) {
        if (item != item_array && !radial && split != nullptr) {
          target = &split->column(this->align());
        }

        Button *but;
        if (item->icon || radial) {
          target->label(item->name, item->icon);

          but = block->buttons_ptrs.last().get();
        }
        else {
          /* Do not use Layout::label here, as our root layout is a menu one,
           * it will add a fake blank icon! */
          but = uiDefBut(block,
                         ButtonType::Label,
                         item->name,
                         0,
                         0,
                         UI_UNIT_X * 5,
                         UI_UNIT_Y,
                         nullptr,
                         0.0,
                         0.0,
                         "");
          target->separator();
        }
        but_tip_from_enum_item(but, item);
      }
      else {
        if (radial) {
          /* invisible dummy button to ensure all items are
           * always at the same position */
          target->separator();
        }
        else {
          /* XXX bug here, columns draw bottom item badly */
          target->separator();
        }
      }
    }
  }
}

void Layout::op_enum(const StringRefNull opname,
                     const StringRefNull propname,
                     IDProperty *properties,
                     wm::OpCallContext context,
                     eUI_Item_Flag flag,
                     const int active)
{
  wmOperatorType *ot = WM_operatortype_find(opname.c_str(), false); /* print error next */

  if (!ot || !ot->srna) {
    item_disabled(this, opname.c_str());
    RNA_warning_bare("UILayout.operator_enum(): %s '%s'",
                     ot ? "operator missing srna" : "unknown operator",
                     opname.c_str());
    return;
  }

  PointerRNA ptr = WM_operator_properties_create_ptr(ot);
  /* so the context is passed to itemf functions (some need it) */
  WM_operator_properties_sanitize(&ptr, false);
  PropertyRNA *prop = RNA_struct_find_property(&ptr, propname.c_str());

  /* don't let bad properties slip through */
  BLI_assert((prop == nullptr) || (RNA_property_type(prop) == PROP_ENUM));

  Block *block = this->block();
  if (prop && RNA_property_type(prop) == PROP_ENUM) {
    const EnumPropertyItem *item_array = nullptr;
    int totitem;
    bool free;

    if (layout_is_radial(this)) {
      /* XXX: While "_all()" guarantees spatial stability,
       * it's bad when an enum has > 8 items total,
       * but only a small subset will ever be shown at once
       * (e.g. Mode Switch menu, after the introduction of GP editing modes).
       */
#if 0
      RNA_property_enum_items_gettexted_all(
          static_cast<bContext *>(block->evil_C), &ptr, prop, &item_array, &totitem, &free);
#else
      RNA_property_enum_items_gettexted(
          static_cast<bContext *>(block->evil_C), &ptr, prop, &item_array, &totitem, &free);
#endif
    }
    else {
      bContext *C = static_cast<bContext *>(block->evil_C);
      const bContextStore *previous_ctx = CTX_store_get(C);
      CTX_store_set(C, context_);
      RNA_property_enum_items_gettexted(C, &ptr, prop, &item_array, &totitem, &free);
      CTX_store_set(C, previous_ctx);
    }

    /* add items */
    this->op_enum_items(ot, ptr, prop, properties, context, flag, item_array, totitem, active);

    if (free) {
      MEM_delete(item_array);
    }
  }
  else if (prop && RNA_property_type(prop) != PROP_ENUM) {
    RNA_warning_bare("UILayout.operator_enum() %s.%s, not an enum type",
                     RNA_struct_identifier(ptr.type),
                     propname.c_str());
    return;
  }
  else {
    RNA_warning_bare("UILayout.operator_enum() %s.%s not found",
                     RNA_struct_identifier(ptr.type),
                     propname.c_str());
    return;
  }
}

void Layout::op_enum(const StringRefNull opname, const StringRefNull propname)
{
  this->op_enum(opname, propname, nullptr, root_->opcontext, UI_ITEM_NONE);
}

PointerRNA Layout::op(wmOperatorType *ot, const std::optional<StringRef> name, int icon)
{
  return this->op(ot, name, icon, root_->opcontext, UI_ITEM_NONE);
}

PointerRNA Layout::op(const StringRefNull opname, const std::optional<StringRef> name, int icon)
{
  return this->op(opname, name, icon, root_->opcontext, UI_ITEM_NONE);
}

/**
 * Hack to add further items in a row into the second part of the split layout, so the label part
 * keeps a fixed size.
 * \return The layout to place further items in for the split layout.
 */
Layout *LayoutInternal::item_prop_split_layout_hack(Layout *layout_parent, Layout *layout_split)
{
  /* Tag item as using property split layout, this is inherited to children so they can get special
   * treatment if needed. */
  ItemInternal::inside_property_split_set(layout_parent, true);

  if (layout_parent->type() == ItemType::LayoutRow) {
    /* Prevent further splits within the row. */
    layout_parent->use_property_split_set(false);

    layout_parent->child_items_layout_ = &layout_split->row(true);
    return layout_parent->child_items_layout_;
  }
  return layout_split;
}

void Layout::prop(PointerRNA *ptr,
                  PropertyRNA *prop,
                  int index,
                  int value,
                  eUI_Item_Flag flag,
                  const std::optional<StringRef> name_opt,
                  int icon,
                  const std::optional<StringRef> placeholder)
{

  Block *block = this->block();
  char namestr[UI_MAX_NAME_STR];
  const bool use_prop_sep = this->use_property_split();
  const bool inside_prop_sep = flag_is_set(flag_, ItemInternalFlag::InsidePropSep);
  /* Columns can define a heading to insert. If the first item added to a split layout doesn't have
   * a label to display in the first column, the heading is inserted there. Otherwise it's inserted
   * as a new row before the first item. */
  Layout *heading_layout = layout_heading_find(this);
  /* Although check-boxes use the split layout, they are an exception and should only place their
   * label in the second column, to not make that almost empty.
   *
   * Keep using 'use_prop_sep' instead of disabling it entirely because
   * we need the ability to have decorators still. */
  bool use_prop_sep_split_label = use_prop_sep;
  bool use_split_empty_name = (flag & ITEM_R_SPLIT_EMPTY_NAME);

#ifdef UI_PROP_DECORATE
  struct DecorateInfo {
    bool use_prop_decorate;
    int len;
    Layout *layout;
    Button *but;
  };
  DecorateInfo ui_decorate{};
  ui_decorate.use_prop_decorate = this->use_property_decorate() && use_prop_sep;

#endif /* UI_PROP_DECORATE */

  block_layout_set_current(block, this);
  block_new_button_group(block, ButtonGroupFlag(0));

  /* retrieve info */
  const PropertyType type = RNA_property_type(prop);
  const bool is_array = RNA_property_array_check(prop);
  const int len = (is_array) ? RNA_property_array_length(ptr, prop) : 0;
  const bool is_id_name_prop = (ptr->owner_id == ptr->data && type == PROP_STRING &&
                                prop == RNA_struct_name_property(ptr->type));

  const bool icon_only = (flag & ITEM_R_ICON_ONLY) != 0;

  /* Boolean with -1 to signify that the value depends on the presence of an icon. */
  const int toggle = ((flag & ITEM_R_TOGGLE) ? 1 : ((flag & ITEM_R_ICON_NEVER) ? 0 : -1));
  const bool no_icon = (toggle == 0);

  /* set name and icon */
  StringRef name = name_opt.value_or(icon_only ? "" : RNA_property_ui_name(prop));

  if (type != PROP_BOOLEAN) {
    flag &= ~ITEM_R_CHECKBOX_INVERT;
  }

  if (flag & ITEM_R_ICON_ONLY) {
    /* pass */
  }
  else if (ELEM(type, PROP_INT, PROP_FLOAT, PROP_STRING, PROP_POINTER)) {
    if (use_prop_sep == false) {
      name = item_name_add_colon(name, namestr);
    }
  }
  else if (type == PROP_BOOLEAN && is_array && index == RNA_NO_INDEX) {
    if (use_prop_sep == false) {
      name = item_name_add_colon(name, namestr);
    }
  }
  else if (type == PROP_ENUM && index != RNA_ENUM_VALUE) {
    if (flag & ITEM_R_COMPACT) {
      name = "";
    }
    else {
      if (use_prop_sep == false) {
        name = item_name_add_colon(name, namestr);
      }
    }
  }

  if (no_icon == false) {
    if (icon == ICON_NONE) {
      icon = RNA_property_ui_icon(prop);
    }

    /* Menus and pie-menus don't show checkbox without this. */
    if ((root_->type == LayoutType::Menu) ||
        /* Use check-boxes only as a fallback in pie-menu's, when no icon is defined. */
        ((root_->type == LayoutType::PieMenu) && (icon == ICON_NONE)))
    {
      const int prop_flag = RNA_property_flag(prop);
      if (type == PROP_BOOLEAN) {
        if ((is_array == false) || (index != RNA_NO_INDEX)) {
          if (prop_flag & PROP_ICONS_CONSECUTIVE) {
            icon = ICON_CHECKBOX_DEHLT; /* but->iconadd will set to correct icon */
          }
          else if (is_array) {
            icon = RNA_property_boolean_get_index(ptr, prop, index) ? ICON_CHECKBOX_HLT :
                                                                      ICON_CHECKBOX_DEHLT;
          }
          else {
            icon = RNA_property_boolean_get(ptr, prop) ? ICON_CHECKBOX_HLT : ICON_CHECKBOX_DEHLT;
          }
        }
      }
      else if (type == PROP_ENUM) {
        if (index == RNA_ENUM_VALUE) {
          const int enum_value = RNA_property_enum_get(ptr, prop);
          if (prop_flag & PROP_ICONS_CONSECUTIVE) {
            icon = ICON_CHECKBOX_DEHLT; /* but->iconadd will set to correct icon */
          }
          else if (prop_flag & PROP_ENUM_FLAG) {
            icon = (enum_value & value) ? ICON_CHECKBOX_HLT : ICON_CHECKBOX_DEHLT;
          }
          else {
            /* Only a single value can be chosen, so display as radio buttons. */
            icon = (enum_value == value) ? ICON_RADIOBUT_ON : ICON_RADIOBUT_OFF;
          }
        }
      }
    }
  }

#ifdef UI_PROP_SEP_ICON_WIDTH_EXCEPTION
  if (use_prop_sep) {
    if (type == PROP_BOOLEAN && (icon == ICON_NONE) && !icon_only) {
      use_prop_sep_split_label = false;
      /* For check-boxes we make an exception: We allow showing them in a split row even without
       * label. It typically relates to its neighbor items, so no need for an extra label. */
      use_split_empty_name = true;
    }
  }
#endif

  if ((type == PROP_ENUM) && (RNA_property_flag(prop) & PROP_ENUM_FLAG)) {
    flag |= ITEM_R_EXPAND;
  }

  const bool slider = (flag & ITEM_R_SLIDER) != 0;
  const bool expand = (flag & ITEM_R_EXPAND) != 0;
  const bool no_bg = (flag & ITEM_R_NO_BG) != 0;
  const bool compact = (flag & ITEM_R_COMPACT) != 0;

  /* get size */
  int w, h;
  item_rna_size(this, name, icon, ptr, prop, index, icon_only, compact, &w, &h);

  const EmbossType prev_emboss = emboss_;
  if (no_bg) {
    emboss_ = EmbossType::NoneOrStatus;
  }

  Button *but = nullptr;

  /* Split the label / property. */
  Layout *layout_parent = this;
  Layout *layout = this;
  if (use_prop_sep) {
    Layout *layout_row = nullptr;
#ifdef UI_PROP_DECORATE
    if (ui_decorate.use_prop_decorate) {
      layout_row = &layout->row(true);
      layout_row->space_ = 0;
      ui_decorate.len = max_ii(1, len);
    }
#endif /* UI_PROP_DECORATE */

    if (name.is_empty() && !use_split_empty_name) {
      /* Ensure we get a column when text is not set. */
      layout = &(layout_row ? layout_row : layout)->column(true);
      layout->space_ = 0;
      if (heading_layout) {
        layout_heading_label_add(layout, heading_layout, false, false);
      }
    }
    else {
      Layout *layout_split =
          &(layout_row ? layout_row : layout)->split(UI_ITEM_PROP_SEP_DIVIDE, true);
      bool label_added = false;
      Layout *layout_sub = &layout_split->column(true);
      layout_sub->space_ = 0;

      if (!RNA_property_editable(ptr, prop)) {
        layout_sub->enabled_set(false);
      }

      if (!use_prop_sep_split_label) {
        /* Pass */
      }
      else if (item_rna_is_expand(prop, index, flag)) {
        fmt::memory_buffer name_with_suffix;
        char str[2] = {'\0'};
        for (int a = 0; a < len; a++) {
          str[0] = RNA_property_array_item_char(prop, a);
          const bool use_prefix = (a == 0 && !name.is_empty());
          if (use_prefix) {
            fmt::format_to(fmt::appender(name_with_suffix), "{} {}", name, str[0]);
          }
          but = uiDefBut(block,
                         ButtonType::Label,
                         use_prefix ? StringRef(name_with_suffix.data(), name_with_suffix.size()) :
                                      str,
                         0,
                         0,
                         w,
                         UI_UNIT_Y,
                         nullptr,
                         0.0,
                         0.0,
                         "");
          but->drawflag |= BUT_TEXT_RIGHT;
          but->drawflag &= ~BUT_TEXT_LEFT;

          label_added = true;
        }
      }
      else {
        but = uiDefBut(block, ButtonType::Label, name, 0, 0, w, UI_UNIT_Y, nullptr, 0.0, 0.0, "");
        but->drawflag |= BUT_TEXT_RIGHT;
        but->drawflag &= ~BUT_TEXT_LEFT;

        label_added = true;
      }

      if (!label_added && heading_layout) {
        layout_heading_label_add(layout_sub, heading_layout, true, false);
      }

      layout_split = LayoutInternal::item_prop_split_layout_hack(layout_parent, layout_split);

      /* Watch out! We can only write into the new layout now. */
      if ((type == PROP_ENUM) && (flag & ITEM_R_EXPAND)) {
        /* Expanded enums each have their own name. */

        /* Often expanded enum's are better arranged into a row,
         * so check the existing layout. */
        if (layout->local_direction() == LayoutDirection::Horizontal) {
          layout = &layout_split->row(true);
        }
        else {
          layout = &layout_split->column(true);
        }
      }
      else {
        if (use_prop_sep_split_label) {
          name = "";
        }
        layout = &layout_split->column(true);
      }
      layout->space_ = 0;
    }

#ifdef UI_PROP_DECORATE
    if (ui_decorate.use_prop_decorate) {
      ui_decorate.layout = &layout_row->column(true);
      ui_decorate.layout->space_ = 0;
      block_layout_set_current(block, layout);
      ui_decorate.but = block->last_but();

      /* Clear after. */
      layout->flag_ |= ItemInternalFlag::PropDecorateNoPad;
    }
#endif /* UI_PROP_DECORATE */
  }
  /* End split. */
  else if (heading_layout) {
    /* Could not add heading to split layout, fall back to inserting it to the layout with the
     * heading itself. */
    layout_heading_label_add(heading_layout, heading_layout, false, false);
  }

  /* array property */
  if (index == RNA_NO_INDEX && is_array) {
    if (inside_prop_sep) {
      /* Within a split row, add array items to a column so they match the column layout of
       * previous items (e.g. transform vector with lock icon for each item). */
      layout = &layout->column(true);
    }

    item_array(layout,
               block,
               name,
               icon,
               ptr,
               prop,
               len,
               0,
               0,
               w,
               h,
               expand,
               slider,
               toggle,
               icon_only,
               compact,
               !use_prop_sep_split_label);
  }
  /* enum item */
  else if (type == PROP_ENUM && index == RNA_ENUM_VALUE) {
    if (icon && !name.is_empty() && !icon_only) {
      uiDefIconTextButR_prop(
          block, ButtonType::Row, icon, name, 0, 0, w, h, ptr, prop, -1, 0, value, std::nullopt);
    }
    else if (icon) {
      uiDefIconButR_prop(
          block, ButtonType::Row, icon, 0, 0, w, h, ptr, prop, -1, 0, value, std::nullopt);
    }
    else {
      uiDefButR_prop(
          block, ButtonType::Row, name, 0, 0, w, h, ptr, prop, -1, 0, value, std::nullopt);
    }
  }
  /* expanded enum */
  else if (type == PROP_ENUM && expand) {
    item_enum_expand(layout, block, ptr, prop, name, h, icon_only);
  }
  /* property with separate label */
  else if (ELEM(type, PROP_ENUM, PROP_STRING, PROP_POINTER)) {
    but = item_with_label(layout,
                          block,
                          name,
                          icon,
                          ptr,
                          prop,
                          index,
                          0,
                          0,
                          w,
                          h,
                          flag,
                          std::nullopt,
                          "UILayout.prop()");

    if (is_id_name_prop) {
      Main *bmain = CTX_data_main(static_cast<bContext *>(block->evil_C));
      ID *id = ptr->owner_id;
      button_func_rename_full_set(
          but, [bmain, id](const std::string &new_name) { ED_id_rename(*bmain, *id, new_name); });
    }

    if (layout->red_alert()) {
      button_flag_enable(but, BUT_REDALERT);
    }

    if (layout->activate_init()) {
      button_flag_enable(but, BUT_ACTIVATE_ON_INIT);
    }
  }
  /* single button */
  else {
    std::optional<ButtonType> button_type = slider ? std::optional(ButtonType::NumSlider) :
                                                     std::nullopt;
    but = uiDefAutoButR(block, ptr, prop, index, name, icon, 0, 0, w, h, button_type);

    if (flag & ITEM_R_CHECKBOX_INVERT) {
      if (ELEM(but->type,
               ButtonType::Checkbox,
               ButtonType::CheckboxN,
               ButtonType::IconToggle,
               ButtonType::IconToggleN))
      {
        but->drawflag |= BUT_CHECKBOX_INVERT;
      }
    }

    if ((toggle == 1) && but->type == ButtonType::Checkbox) {
      but->type = ButtonType::Toggle;
    }

    if (layout->red_alert()) {
      button_flag_enable(but, BUT_REDALERT);
    }

    if (layout->activate_init()) {
      button_flag_enable(but, BUT_ACTIVATE_ON_INIT);
    }
  }

  /* The resulting button may have the icon set since boolean button drawing
   * is being 'helpful' and adding an icon for us.
   * In this case we want the ability not to have an icon.
   *
   * We could pass an argument not to set the icon to begin with however this is the one case
   * the functionality is needed. */
  if (but && no_icon) {
    if ((icon == ICON_NONE) && (but->icon != ICON_NONE)) {
      def_but_icon_clear(but);
    }
  }

  /* Mark non-embossed text-fields inside a list-box. */
  if (but && (block->flag & BLOCK_LIST_ITEM) && (but->type == ButtonType::Text) &&
      ELEM(but->emboss, EmbossType::None, EmbossType::NoneOrStatus))
  {
    button_flag_enable(but, BUT_LIST_ITEM);
  }

  if (but) {
    if (placeholder) {
      button_placeholder_set(but, *placeholder);
    }
    if (ELEM(but->type, ButtonType::Text) && (flag & ITEM_R_TEXT_BUT_FORCE_SEMI_MODAL_ACTIVE)) {
      button_flag2_enable(but, BUT2_FORCE_SEMI_MODAL_ACTIVE);
    }
  }

#ifdef UI_PROP_DECORATE
  if (ui_decorate.use_prop_decorate) {
    Button *but_decorate = ui_decorate.but ? block->next_but(ui_decorate.but) : block->first_but();

    /* Move temporarily last buts to avoid multiple reallocations while inserting decorators. */
    Vector<std::unique_ptr<Button>> tmp;
    tmp.reserve(ui_decorate.len);
    while (but_decorate && but_decorate != block->buttons_ptrs.last().get()) {
      tmp.append(block->buttons_ptrs.pop_last());
    }
    const bool use_blank_decorator = (flag & ITEM_R_FORCE_BLANK_DECORATE);
    Layout *layout_col = &ui_decorate.layout->column(false);
    layout_col->space_ = 0;
    layout_col->emboss_ = EmbossType::None;

    int i;
    for (i = 0; i < ui_decorate.len && but_decorate; i++) {
      PointerRNA *ptr_dec = use_blank_decorator ? nullptr : &but_decorate->rnapoin;
      PropertyRNA *prop_dec = use_blank_decorator ? nullptr : but_decorate->rnaprop;

      /* The icons are set in 'but_anim_flag' */
      layout_col->decorator(ptr_dec, prop_dec, but_decorate->rnaindex);
      but = block->buttons_ptrs.last().get();

      if (!tmp.is_empty()) {
        block->buttons_ptrs.append(tmp.pop_last());
        but_decorate = block->buttons_ptrs.last().get();
      }
      else {
        but_decorate = nullptr;
      }
    }
    while (!tmp.is_empty()) {
      block->buttons_ptrs.append(tmp.pop_last());
    }
    BLI_assert(ELEM(i, 1, ui_decorate.len));

    layout->flag_ &= ~ItemInternalFlag::PropDecorateNoPad;
  }
#endif /* UI_PROP_DECORATE */

  if (no_bg) {
    emboss_ = prev_emboss;
  }

  /* ensure text isn't added to icon_only buttons */
  if (but && icon_only) {
    BLI_assert(but->str.empty());
  }
}

void Layout::prop(PointerRNA *ptr,
                  const StringRefNull propname,
                  const eUI_Item_Flag flag,
                  const std::optional<StringRef> name,
                  int icon)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());

  if (!prop) {
    item_disabled(this, propname.c_str());
    RNA_warning_bare("UILayout.prop(): property not found: %s.%s",
                     RNA_struct_identifier(ptr->type),
                     propname.c_str());
    return;
  }

  this->prop(ptr, prop, RNA_NO_INDEX, 0, flag, name, icon);
}

void Layout::prop_with_popover(PointerRNA *ptr,
                               PropertyRNA *prop,
                               int index,
                               int value,
                               const eUI_Item_Flag flag,
                               const std::optional<StringRefNull> name,
                               int icon,
                               const char *panel_type)
{
  Block *block = this->block();
  int i = block->buttons_ptrs.size();
  this->prop(ptr, prop, index, value, flag, name, icon);
  for (; i < block->buttons_ptrs.size(); i++) {
    Button *but = block->buttons_ptrs[i].get();
    if (but->rnaprop == prop && ELEM(but->type, ButtonType::Menu, ButtonType::Color)) {
      button_rna_menu_convert_to_panel_type(but, panel_type);
      break;
    }
  }
  if (i == block->buttons_ptrs.size()) {
    const StringRefNull propname = RNA_property_identifier(prop);
    item_disabled(this, panel_type);
    RNA_warning_bare("UILayout.prop_with_popover(): property could not use a popover: %s.%s (%s)",
                     RNA_struct_identifier(ptr->type),
                     propname.c_str(),
                     panel_type);
  }
}

void Layout::prop_with_menu(PointerRNA *ptr,
                            PropertyRNA *prop,
                            int index,
                            int value,
                            const eUI_Item_Flag flag,
                            const std::optional<StringRefNull> name,
                            int icon,
                            const char *menu_type)
{
  Block *block = this->block();
  int i = block->buttons_ptrs.size();
  this->prop(ptr, prop, index, value, flag, name, icon);
  while (i < block->buttons_ptrs.size()) {
    Button *but = block->buttons_ptrs[i].get();
    if (but->rnaprop == prop && but->type == ButtonType::Menu) {
      button_rna_menu_convert_to_menu_type(but, menu_type);
      break;
    }
    i++;
  }
  if (i == block->buttons_ptrs.size()) {
    const StringRefNull propname = RNA_property_identifier(prop);
    item_disabled(this, menu_type);
    RNA_warning_bare("UILayout.prop_with_menu(): property could not use a menu: %s.%s (%s)",
                     RNA_struct_identifier(ptr->type),
                     propname.c_str(),
                     menu_type);
  }
}

void Layout::prop_enum(PointerRNA *ptr,
                       PropertyRNA *prop,
                       int value,
                       const std::optional<StringRefNull> name,
                       int icon)
{
  if (RNA_property_type(prop) != PROP_ENUM) {
    const StringRefNull propname = RNA_property_identifier(prop);
    item_disabled(this, propname.c_str());
    RNA_warning_bare("UILayout.prop_enum(): property not an enum: %s.%s",
                     RNA_struct_identifier(ptr->type),
                     propname.c_str());
    return;
  }

  this->prop(ptr, prop, RNA_ENUM_VALUE, value, UI_ITEM_NONE, name, icon);
}

void Layout::prop_enum(PointerRNA *ptr,
                       PropertyRNA *prop,
                       const char *value,
                       const std::optional<StringRefNull> name,
                       int icon)
{
  if (UNLIKELY(RNA_property_type(prop) != PROP_ENUM)) {
    const StringRefNull propname = RNA_property_identifier(prop);
    item_disabled(this, propname.c_str());
    RNA_warning_bare("UILayout.prop_enum(): not an enum property: %s.%s",
                     RNA_struct_identifier(ptr->type),
                     propname.c_str());
    return;
  }

  const EnumPropertyItem *item;
  bool free;
  RNA_property_enum_items(
      static_cast<bContext *>(this->block()->evil_C), ptr, prop, &item, nullptr, &free);

  int ivalue;
  if (!RNA_enum_value_from_id(item, value, &ivalue)) {
    const StringRefNull propname = RNA_property_identifier(prop);
    if (free) {
      MEM_delete(item);
    }
    item_disabled(this, propname.c_str());
    RNA_warning_bare("UILayout.prop_enum(): enum property value not found: %s", value);
    return;
  }

  for (int a = 0; item[a].identifier; a++) {
    if (item[a].identifier[0] == '\0') {
      /* Skip enum item separators. */
      continue;
    }
    if (item[a].value == ivalue) {
      const StringRefNull item_name = name.value_or(
          CTX_IFACE_(RNA_property_translation_context(prop), item[a].name));
      const eUI_Item_Flag flag = !item_name.is_empty() ? UI_ITEM_NONE : ITEM_R_ICON_ONLY;

      this->prop(ptr, prop, RNA_ENUM_VALUE, ivalue, flag, item_name, icon ? icon : item[a].icon);
      break;
    }
  }

  if (free) {
    MEM_delete(item);
  }
}

void Layout::prop_enum(PointerRNA *ptr,
                       const StringRefNull propname,
                       const char *value,
                       const std::optional<StringRefNull> name,
                       int icon)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());
  if (UNLIKELY(prop == nullptr)) {
    item_disabled(this, propname.c_str());
    RNA_warning_bare("UILayout.prop_enum(): enum property not found: %s.%s",
                     RNA_struct_identifier(ptr->type),
                     propname.c_str());
    return;
  }
  this->prop_enum(ptr, prop, value, name, icon);
}

void Layout::props_enum(PointerRNA *ptr, const StringRefNull propname)
{
  Block *block = this->block();

  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());

  if (!prop) {
    item_disabled(this, propname.c_str());
    RNA_warning_bare("UILayout.props_enum(): enum property not found: %s.%s",
                     RNA_struct_identifier(ptr->type),
                     propname.c_str());
    return;
  }

  if (RNA_property_type(prop) != PROP_ENUM) {
    RNA_warning_bare("UILayout.props_enum(): not an enum property: %s.%s",
                     RNA_struct_identifier(ptr->type),
                     propname.c_str());
    return;
  }

  Layout *split = &this->split(0.0f, false);
  Layout *column = &split->column(false);

  int totitem;
  const EnumPropertyItem *item;
  bool free;
  RNA_property_enum_items_gettexted(
      static_cast<bContext *>(block->evil_C), ptr, prop, &item, &totitem, &free);

  for (int i = 0; i < totitem; i++) {
    if (item[i].identifier[0]) {
      column->prop_enum(ptr, prop, item[i].value, item[i].name, item[i].icon);
      but_tip_from_enum_item(block->buttons_ptrs.last().get(), &item[i]);
    }
    else {
      if (item[i].name) {
        if (i != 0) {
          column = &split->column(false);
        }

        column->label(item[i].name, ICON_NONE);
        Button *bt = block->buttons_ptrs.last().get();
        bt->drawflag = BUT_TEXT_LEFT;

        but_tip_from_enum_item(bt, &item[i]);
      }
      else {
        column->separator();
      }
    }
  }

  if (free) {
    MEM_delete(item);
  }
}

void Layout::prop_search(PointerRNA *ptr,
                         PropertyRNA *prop,
                         PointerRNA *searchptr,
                         PropertyRNA *searchprop,
                         PropertyRNA *item_searchprop,
                         const std::optional<StringRefNull> name_opt,
                         int icon,
                         bool results_are_suggestions)
{
  const bool use_prop_sep = this->use_property_split();
  Block *block = this->block();
  block_new_button_group(block, ButtonGroupFlag(0));

  const PropertyType type = RNA_property_type(prop);
  if (!ELEM(type, PROP_POINTER, PROP_STRING, PROP_ENUM)) {
    RNA_warning_bare("UILayout.prop_search(): Property %s.%s must be a pointer, string or enum",
                     RNA_struct_identifier(ptr->type),
                     RNA_property_identifier(prop));
    return;
  }
  if (RNA_property_type(searchprop) != PROP_COLLECTION) {
    RNA_warning_bare(
        "UILayout.prop_search(): search collection property is not a collection type: %s.%s",
        RNA_struct_identifier(searchptr->type),
        RNA_property_identifier(searchprop));
    return;
  }
  if (item_searchprop && RNA_property_type(item_searchprop) != PROP_STRING) {
    RNA_warning_bare(
        "UILayout.prop_search(): Search collection items' property is not a string type: %s.%s",
        RNA_struct_identifier(RNA_property_pointer_type(searchptr, searchprop)),
        RNA_property_identifier(item_searchprop));
    return;
  }

  /* get icon & name */
  if (icon == ICON_NONE) {
    const StructRNA *icontype;
    if (type == PROP_POINTER) {
      icontype = RNA_property_pointer_type(ptr, prop);
    }
    else {
      icontype = RNA_property_pointer_type(searchptr, searchprop);
    }

    icon = RNA_struct_ui_icon(icontype);
  }
  StringRefNull name = name_opt.value_or(RNA_property_ui_name(prop));

  char namestr[UI_MAX_NAME_STR];
  if (use_prop_sep == false) {
    name = item_name_add_colon(name, namestr);
  }

  /* create button */

  int w, h;
  item_rna_size(this, name, icon, ptr, prop, 0, false, false, &w, &h);
  w += UI_UNIT_X; /* X icon needs more space */
  Button *but = item_with_label(this,
                                block,
                                name,
                                icon,
                                ptr,
                                prop,
                                0,
                                0,
                                0,
                                w,
                                h,
                                0,
                                ButtonType::SearchMenu,
                                "UILayout.prop_search()");
  BLI_assert(but->type == ButtonType::SearchMenu);
  button_configure_search(
      but, ptr, prop, searchptr, searchprop, item_searchprop, results_are_suggestions);
}

void Layout::prop_search(PointerRNA *ptr,
                         const StringRefNull propname,
                         PointerRNA *searchptr,
                         const StringRefNull searchpropname,
                         const std::optional<StringRefNull> name,
                         int icon)
{
  /* validate arguments */
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());
  if (!prop) {
    RNA_warning_bare("UILayout.prop_search(): property not found: %s.%s",
                     RNA_struct_identifier(ptr->type),
                     propname.c_str());
    return;
  }
  PropertyRNA *searchprop = RNA_struct_find_property(searchptr, searchpropname.c_str());
  if (!searchprop) {
    RNA_warning_bare("UILayout.prop_search(): search collection property not found: %s.%s",
                     RNA_struct_identifier(searchptr->type),
                     searchpropname.c_str());
    return;
  }

  this->prop_search(ptr, prop, searchptr, searchprop, nullptr, name, icon, false);
}

void Layout::menu(MenuType *mt, const std::optional<StringRef> name_opt, int icon)
{
  Block *block = this->block();
  bContext *C = static_cast<bContext *>(block->evil_C);
  if (WM_menutype_poll(C, mt) == false) {
    return;
  }

  const StringRef name = name_opt.value_or(CTX_IFACE_(mt->translation_context, mt->label));

  if (root_->type == LayoutType::Menu && !icon) {
    icon = ICON_BLANK1;
  }

  item_menu(this,
            name,
            icon,
            item_menutype_func,
            mt,
            nullptr,
            mt->description ? TIP_(mt->description) : "",
            false);
}

void Layout::menu(const StringRef menuname, const std::optional<StringRef> name, int icon)
{
  MenuType *mt = WM_menutype_find(menuname, false);
  if (mt == nullptr) {
    RNA_warning_bare("UILayout.menu(): not found %s", std::string(menuname).c_str());
    return;
  }
  this->menu(mt, name, icon);
}

void Layout::menu_contents(const StringRef menuname)
{
  MenuType *mt = WM_menutype_find(menuname, false);
  if (mt == nullptr) {
    RNA_warning_bare("UILayout.menu_contents(): not found %s", std::string(menuname).c_str());
    return;
  }

  Block *block = this->block();
  bContext *C = static_cast<bContext *>(block->evil_C);
  if (WM_menutype_poll(C, mt) == false) {
    return;
  }

  menutype_draw(C, mt, this);
}

void Layout::decorator(PointerRNA *ptr, PropertyRNA *prop, int index)
{
  Block *block = this->block();

  block_layout_set_current(block, this);
  Layout &col = this->column(false);
  col.space_ = 0;
  col.emboss_ = EmbossType::None;

  if (ELEM(nullptr, ptr, prop) || !RNA_property_animateable(ptr, prop)) {
    Button *but = uiDefIconBut(block,
                               ButtonType::Decorator,
                               ICON_BLANK1,
                               0,
                               0,
                               UI_UNIT_X,
                               UI_UNIT_Y,
                               nullptr,
                               0.0,
                               0.0,
                               "");
    but->flag |= BUT_DISABLED;
    return;
  }

  const bool is_expand = item_rna_is_expand(prop, index, UI_ITEM_NONE);
  const bool is_array = RNA_property_array_check(prop);

  /* Loop for the array-case, but only do in case of an expanded array. */
  for (int i = 0; i < (is_expand ? RNA_property_array_length(ptr, prop) : 1); i++) {
    ButtonDecorator *but = static_cast<ButtonDecorator *>(uiDefIconBut(block,
                                                                       ButtonType::Decorator,
                                                                       ICON_DOT,
                                                                       0,
                                                                       0,
                                                                       UI_UNIT_X,
                                                                       UI_UNIT_Y,
                                                                       nullptr,
                                                                       0.0,
                                                                       0.0,
                                                                       TIP_("Animate property")));

    button_func_set(but, button_anim_decorate_cb, but, nullptr);
    but->flag |= BUT_UNDO | BUT_DRAG_LOCK;
    /* Decorators have their own RNA data, using the normal #Button RNA members has many
     * side-effects. */
    but->decorated_rnapoin = *ptr;
    but->decorated_rnaprop = prop;
    /* def_but_rna() sets non-array buttons to have a RNA index of 0. */
    but->decorated_rnaindex = (!is_array || is_expand) ? i : index;
  }
}

void Layout::decorator(PointerRNA *ptr, const std::optional<StringRefNull> propname, int index)
{
  PropertyRNA *prop = nullptr;

  if (ptr && propname) {
    /* validate arguments */
    prop = RNA_struct_find_property(ptr, propname->c_str());
    if (!prop) {
      item_disabled(this, propname->c_str());
      RNA_warning_bare("UILayout::decorator(): property not found: %s.%s",
                       RNA_struct_identifier(ptr->type),
                       propname->c_str());
      return;
    }
  }

  /* ptr and prop are allowed to be nullptr here. */
  this->decorator(ptr, prop, index);
}

void Layout::popover(const bContext *C,
                     PanelType *pt,
                     const std::optional<StringRef> name_opt,
                     int icon)
{
  Layout *layout = this;
  const StringRef name = name_opt.value_or(CTX_IFACE_(pt->translation_context, pt->label));

  if (root_->type == LayoutType::Menu && !icon) {
    icon = ICON_BLANK1;
  }

  const bContextStore *previous_ctx = CTX_store_get(C);
  /* Set context for polling (and panel header drawing). */
  CTX_store_set(const_cast<bContext *>(C), context_);

  const bool ok = (pt->poll == nullptr) || pt->poll(C, pt);
  if (ok && (pt->draw_header != nullptr)) {
    layout = &this->row(true);
    Panel panel{};
    Panel_Runtime panel_runtime{};
    panel.runtime = &panel_runtime;
    panel.type = pt;
    panel.layout = layout;
    panel.flag = PNL_POPOVER;
    pt->draw_header(C, &panel);
  }

  CTX_store_set(const_cast<bContext *>(C), previous_ctx);

  Button *but = item_menu(
      layout, name, icon, item_paneltype_func, pt, nullptr, TIP_(pt->description), true);
  but->type = ButtonType::Popover;

  /* Override button size when there is no icon or label. */
  if (layout->root()->type == LayoutType::VerticalBar && !icon && name.is_empty()) {
    but->rect.xmax = but->rect.xmin + UI_UNIT_X;
  }

  if (!ok) {
    but->flag |= BUT_DISABLED;
  }
}

void Layout::popover(const bContext *C,
                     const StringRef panel_type,
                     std::optional<StringRef> name_opt,
                     int icon,
                     PopupAttachDirection direction)
{
  PanelType *pt = WM_paneltype_find(panel_type, true);
  if (pt == nullptr) {
    RNA_warning_bare("UILayout.popover(): Panel type not found '%s'",
                     std::string(panel_type).c_str());
    return;
  }
  pt->popup_draw_direction = direction;
  this->popover(C, pt, name_opt, icon);
}

void Layout::popover_group(
    bContext *C, int space_id, int region_id, const char *context, const char *category)
{
  SpaceType *st = BKE_spacetype_from_id(space_id);
  if (st == nullptr) {
    RNA_warning_bare("UILayout.popover(): space type not found %d", space_id);
    return;
  }
  ARegionType *art = BKE_regiontype_from_id(st, region_id);
  if (art == nullptr) {
    RNA_warning_bare("UILayout.popover(): region type not found %d", region_id);
    return;
  }

  for (PanelType &pt : art->paneltypes) {
    /* Causes too many panels, check context. */
    if (pt.parent_id[0] == '\0') {
      if (/* (*context == '\0') || */ STREQ(pt.context, context)) {
        if ((*category == '\0') || STREQ(pt.category, category)) {
          if (pt.poll == nullptr || pt.poll(C, &pt)) {
            this->popover(C, &pt, std::nullopt, ICON_NONE);
          }
        }
      }
    }
  }
}

void Layout::label(const StringRef name, int icon)
{
  uiItem_simple(this, name, icon);
}

Button *Layout::button(const StringRef name,
                       const int icon,
                       std::function<void(bContext &)> func,
                       std::optional<StringRef> tooltip)
{
  Button *but = uiItem_simple(this, name, icon, tooltip, ButtonType::But);
  button_func_set(but, std::move(func));
  return but;
}

void Layout::separator(float factor, const LayoutSeparatorType type)
{
  Block *block = this->block();
  const bool is_menu = block_is_menu(block);
  const bool is_pie = block_is_pie_menu(block);
  if (is_menu && !block_can_add_separator(block)) {
    return;
  }

  /* Sizing of spaces should not depend on line width. */
  const int space = (is_menu) ? int(7.0f * UI_SCALE_FAC * factor) :
                                int(6.0f * UI_SCALE_FAC * factor);

  ButtonType but_type;

  switch (type) {
    case LayoutSeparatorType::Line:
      but_type = ButtonType::SeprLine;
      break;
    case LayoutSeparatorType::Auto:
      but_type = (is_menu && !is_pie) ? ButtonType::SeprLine : ButtonType::Sepr;
      break;
    default:
      but_type = ButtonType::Sepr;
  }

  bool is_vertical_bar = (w_ == 0) && but_type == ButtonType::SeprLine;

  block_layout_set_current(block, this);
  Button *but = uiDefBut(block,
                         but_type,
                         "",
                         0,
                         0,
                         space,
                         is_vertical_bar ? UI_UNIT_Y : space,
                         nullptr,
                         0.0,
                         0.0,
                         "");

  if (but_type == ButtonType::SeprLine) {
    auto *but_line = static_cast<ButtonSeparatorLine *>(but);
    but_line->is_vertical = is_vertical_bar;
  }
}

void Layout::progress_indicator(const char *text,
                                const float factor,
                                const ButProgressType progress_type)
{
  const bool has_text = text && text[0];
  Block *block = this->block();
  short width;

  if (progress_type == ButProgressType::Bar) {
    width = UI_UNIT_X * 5;
  }
  else if (has_text) {
    width = UI_UNIT_X * 8;
  }
  else {
    width = UI_UNIT_X;
  }

  block_layout_set_current(block, this);
  Button *but = uiDefBut(block,
                         ButtonType::Progress,
                         (text) ? text : "",
                         0,
                         0,
                         width,
                         short(UI_UNIT_Y),
                         nullptr,
                         0.0,
                         0.0,
                         "");

  if (has_text && (progress_type == ButProgressType::Ring)) {
    /* For progress bar, centered is okay, left aligned for ring/pie. */
    but->drawflag |= BUT_TEXT_LEFT;
  }

  ButtonProgress *progress_bar = static_cast<ButtonProgress *>(but);
  progress_bar->progress_type = progress_type;
  progress_bar->progress_factor = factor;
}

void Layout::separator_spacer()
{
  Block *block = this->block();
  const bool is_popup = block_is_popup_any(block);

  if (is_popup) {
    printf("Error: separator_spacer() not supported in popups.\n");
    return;
  }

  if (block->direction & UI_DIR_RIGHT) {
    printf("Error: separator_spacer() only supported in horizontal blocks.\n");
    return;
  }

  block_layout_set_current(block, this);
  uiDefBut(
      block, ButtonType::SeprSpacer, "", 0, 0, 0.3f * UI_UNIT_X, UI_UNIT_Y, nullptr, 0.0, 0.0, "");
}

void Layout::menu_fn(const StringRefNull name, int icon, MenuCreateFunc func, void *arg)
{
  if (!func) {
    return;
  }

  item_menu(this, name, icon, func, arg, nullptr, "", false);
}

void Layout::menu_fn_argN_free(const StringRefNull name, int icon, MenuCreateFunc func, void *argN)
{
  if (!func) {
    return;
  }

  /* Second 'argN' only ensures it gets freed. */
  item_menu(this, name, icon, func, argN, argN, "", false);
}

PointerRNA Layout::op_menu_enum(const bContext *C,
                                wmOperatorType *ot,
                                const StringRefNull propname,
                                std::optional<StringRefNull> name,
                                int icon)
{
  /* Caller must check */
  BLI_assert(ot->srna != nullptr);

  std::string operator_name;
  if (!name) {
    operator_name = WM_operatortype_name(ot, nullptr);
    name = operator_name.c_str();
  }

  if (root_->type == LayoutType::Menu && !icon) {
    icon = ICON_BLANK1;
  }

  MenuItemLevel *lvl = MEM_new<MenuItemLevel>("MenuItemLevel");
  STRNCPY_UTF8(lvl->opname, ot->idname);
  STRNCPY_UTF8(lvl->propname, propname.c_str());
  lvl->opcontext = root_->opcontext;

  Button *but = item_menu(this,
                          *name,
                          icon,
                          menu_item_enum_opname_menu,
                          nullptr,
                          lvl,
                          std::nullopt,
                          true,
                          but_func_argN_free<MenuItemLevel>,
                          but_func_argN_copy<MenuItemLevel>);
  /* Use the menu button as owner for the operator properties, which will then be passed to the
   * individual menu items. */
  but->opptr = MEM_new<PointerRNA>("uiButOpPtr", WM_operator_properties_create_ptr(ot));
  BLI_assert(but->opptr->data == nullptr);
  WM_operator_properties_alloc(
      &but->opptr, reinterpret_cast<IDProperty **>(&but->opptr->data), ot->idname);

  /* add hotkey here, lower UI code can't detect it */
  if ((this->block()->flag & BLOCK_LOOP) && (ot->prop && ot->invoke)) {
    if (std::optional<std::string> shortcut_str = WM_key_event_operator_string(
            C, ot->idname, root_->opcontext, nullptr, false))
    {
      button_add_shortcut(but, shortcut_str->c_str(), false);
    }
  }
  return *but->opptr;
}

PointerRNA Layout::op_menu_enum(const bContext *C,
                                const StringRefNull opname,
                                const StringRefNull propname,
                                StringRefNull name,
                                int icon)
{
  wmOperatorType *ot = WM_operatortype_find(opname.c_str(), false); /* print error next */

  UI_OPERATOR_ERROR_RET(ot, opname.c_str(), "UILayout.operator_menu_enum()");

  if (!ot->srna) {
    item_disabled(this, opname.c_str());
    RNA_warning_bare("UILayout.operator_menu_enum(): operator missing srna '%s'", opname.c_str());
    return PointerRNA_NULL;
  }

  return this->op_menu_enum(C, ot, propname, name, icon);
}

static void menu_item_enum_rna_menu(bContext * /*C*/, Layout *layout, void *arg)
{
  MenuItemLevel *lvl = static_cast<MenuItemLevel *>((static_cast<Button *>(arg))->func_argN);

  layout->operator_context_set(lvl->opcontext);
  layout->props_enum(&lvl->rnapoin, lvl->propname);
}

void Layout::prop_menu_enum(PointerRNA *ptr,
                            PropertyRNA *prop,
                            const std::optional<StringRefNull> name,
                            int icon)
{
  if (root_->type == LayoutType::Menu && !icon) {
    icon = ICON_BLANK1;
  }

  MenuItemLevel *lvl = MEM_new<MenuItemLevel>("MenuItemLevel");
  lvl->rnapoin = *ptr;
  STRNCPY_UTF8(lvl->propname, RNA_property_identifier(prop));
  lvl->opcontext = root_->opcontext;

  item_menu(this,
            name.value_or(RNA_property_ui_name(prop)),
            icon,
            menu_item_enum_rna_menu,
            nullptr,
            lvl,
            RNA_property_description(prop),
            false,
            but_func_argN_free<MenuItemLevel>,
            but_func_argN_copy<MenuItemLevel>);
}

void Layout::prop_tabs_enum(bContext *C,
                            PointerRNA *ptr,
                            PropertyRNA *prop,
                            PointerRNA *ptr_highlight,
                            PropertyRNA *prop_highlight,
                            bool icon_only,
                            EnumTabExpand expand_as)
{
  Block *block = this->block();

  block_layout_set_current(block, this);
  item_enum_expand_tabs(this,
                        C,
                        block,
                        ptr,
                        prop,
                        ptr_highlight,
                        prop_highlight,
                        std::nullopt,
                        UI_UNIT_Y,
                        icon_only,
                        expand_as);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layout Items
 * \{ */

void LayoutInternal::layout_estimate(Layout *layout)
{
  layout->estimate();
}

void LayoutInternal::layout_resolve(Layout *layout)
{
  layout->resolve();
}

/* single-row layout */
void LayoutRow::estimate_impl()
{
  if (this->type() == ItemType::LayoutRoot) {
    return;
  }
  bool min_size_flag = true;

  w_ = 0;
  h_ = 0;

  if (this->items().is_empty()) {
    return;
  }

  const Item *item_last = this->items().last();
  for (Item *item : this->items()) {
    const bool is_item_last = (item == item_last);
    const int2 size = item->size();

    min_size_flag = min_size_flag && item->fixed_size();

    w_ += size.x;
    h_ = std::max(size.y, h_);

    if (!is_item_last) {
      w_ += space_;
    }
  }

  if (min_size_flag) {
    this->fixed_size_set(true);
  }
}

void LayoutRow::resolve_impl()
{
  if (this->items().is_empty()) {
    return;
  }

  int last_free_item_idx = -1;
  int x, neww, newtotw, minw, offset;
  int freew, fixedx, freex, flag = 0, lastw = 0;
  float extra_pixel;

  const int y = y_;
  int w = w_;
  int totw = 0;
  int tot = 0;

  for (Item *item : this->items()) {
    totw += item->size().x;
    tot++;
  }

  if (totw == 0) {
    return;
  }

  if (w != 0) {
    w -= (tot - 1) * space_;
  }
  int fixedw = 0;

  const Item *item_last = this->items().last();

  /* keep clamping items to fixed minimum size until all are done */
  do {
    freew = 0;
    x = 0;
    flag = 0;
    newtotw = totw;
    extra_pixel = 0.0f;

    for (Item *item : this->items()) {
      if (ItemInternal::auto_fixed_size(item)) {
        continue;
      }
      const bool is_item_last = (item == item_last);

      int2 size = item->size();
      minw = litem_min_width(size.x);

      if (w - lastw > 0) {
        neww = item_fit(size.x, x, totw, w - lastw, is_item_last, this->alignment(), &extra_pixel);
      }
      else {
        neww = 0; /* no space left, all will need clamping to minimum size */
      }

      x += neww;

      bool min_flag = item->fixed_size();
      /* ignore min flag for rows with right or center alignment */
      if (item->type() != ItemType::Button &&
          ELEM((static_cast<Layout *>(item))->alignment(),
               LayoutAlign::Right,
               LayoutAlign::Center) &&
          this->alignment() == LayoutAlign::Expand && this->fixed_size())
      {
        min_flag = false;
      }

      if ((neww < minw || min_flag) && w != 0) {
        /* fixed size */
        ItemInternal::auto_fixed_size_set(item, true);
        if (item->type() != ItemType::Button && item->fixed_size()) {
          minw = size.x;
        }
        fixedw += minw;
        flag = 1;
        newtotw -= size.x;
      }
      else {
        /* keep free size */
        ItemInternal::auto_fixed_size_set(item, false);
        freew += size.x;
      }
    }

    totw = newtotw;
    lastw = fixedw;
  } while (flag);

  freex = 0;
  fixedx = 0;
  extra_pixel = 0.0f;
  x = x_;

  int item_idx = -1;
  for (Item *item : this->items()) {
    item_idx++;
    const bool is_item_last = (item == item_last);
    int2 size = item->size();
    minw = litem_min_width(size.x);

    if (ItemInternal::auto_fixed_size(item)) {
      /* fixed minimum size items */
      if (item->type() != ItemType::Button && item->fixed_size()) {
        minw = size.x;
      }
      size.x = item_fit(
          minw, fixedx, fixedw, min_ii(w, fixedw), is_item_last, this->alignment(), &extra_pixel);
      fixedx += size.x;
    }
    else {
      /* free size item */
      size.x = item_fit(
          size.x, freex, freew, w - fixedw, is_item_last, this->alignment(), &extra_pixel);
      freex += size.x;
      last_free_item_idx = item_idx;
    }

    /* align right/center */
    offset = 0;
    if (this->alignment() == LayoutAlign::Right) {
      if (freew + fixedw > 0 && freew + fixedw < w) {
        offset = w - (fixedw + freew);
      }
    }
    else if (this->alignment() == LayoutAlign::Center) {
      if (freew + fixedw > 0 && freew + fixedw < w) {
        offset = (w - (fixedw + freew)) / 2;
      }
    }

    /* position item */
    item_position(item, x + offset, y - size.y, size.x, size.y);

    x += size.x;
    if (!is_item_last) {
      x += space_;
    }
  }

  /* add extra pixel */
  int extra_pixel_move = w_ - (x - x_);
  if (extra_pixel_move > 0 && this->alignment() == LayoutAlign::Expand &&
      last_free_item_idx >= 0 && item_last && ItemInternal::auto_fixed_size(item_last))
  {
    item_move(this->items()[last_free_item_idx], 0, extra_pixel_move);
    Span<Item *> items_after_last_free = this->items().drop_front(last_free_item_idx + 1);
    for (Item *item : items_after_last_free) {
      item_move(item, extra_pixel_move, extra_pixel_move);
    }
  }

  w_ = x - x_;
  h_ = y_ - y;
  x_ = x;
  y_ = y;
}

/* single-column layout */
void LayoutColumn::estimate_impl()
{
  if (this->type() == ItemType::LayoutRoot) {
    return;
  }
  const bool is_box = this->type() == ItemType::LayoutBox;
  bool min_size_flag = true;

  w_ = 0;
  h_ = 0;

  for (auto *iter = this->items().begin(); iter != this->items().end(); iter++) {
    Item *item = *iter;
    const int2 size = item->size();

    min_size_flag = min_size_flag && item->fixed_size();

    w_ = std::max(w_, size.x);
    h_ += size.y;

    const Item *next_item = (item == this->items().last()) ? nullptr : *(iter + 1);
    const int spaces_num = spaces_after_column_item(this, item, next_item, is_box);
    h_ += spaces_num * space_;
  }

  if (min_size_flag) {
    this->fixed_size_set(true);
  }
}

void LayoutColumn::resolve_impl()
{
  const bool is_box = this->type() == ItemType::LayoutBox;
  const bool is_menu = this->type() == ItemType::LayoutRoot &&
                       this->root()->type == LayoutType::Menu;
  const int x = x_;
  int y = y_;

  for (auto *iter = this->items().begin(); iter != this->items().end(); iter++) {
    Item *item = *iter;
    const int2 size = item->size();

    y -= size.y;
    item_position(item, x, y, is_menu ? size.x : w_, size.y);

    const Item *next_item = (item == this->items().last()) ? nullptr : *(iter + 1);
    const int spaces_num = spaces_after_column_item(this, item, next_item, is_box);
    y -= spaces_num * space_;

    if (is_box) {
      ItemInternal::box_item_set(item, true);
    }
  }

  h_ = y_ - y;
  x_ = x;
  y_ = y;
}

void LayoutRadial::resolve_impl()
{
  int itemnum = 0;

  /* For the radial layout we will use Matt Ebb's design
   * for radiation, see http://mattebb.com/weblog/radiation/
   * also the old code at #5103. */

  const int pie_radius = U.pie_menu_radius * UI_SCALE_FAC;

  const int x = x_;
  const int y = y_;

  int minx = x, miny = y, maxx = x, maxy = y;

  this->block()->pie_data->pie_dir_mask = 0;

  for (Item *item : this->items()) {
    /* Not all button types are drawn in a radial menu, do filtering here. */
    if (!item_is_radial_displayable(item)) {
      continue;
    }

    float vec[2];
    const RadialDirection dir = get_radialbut_vec(vec, itemnum);
    const float factor[2] = {
        (vec[0] > 0.01f) ? 0.0f : ((vec[0] < -0.01f) ? -1.0f : -0.5f),
        (vec[1] > 0.99f) ? 0.0f : ((vec[1] < -0.99f) ? -1.0f : -0.5f),
    };
    itemnum++;

    /* Enable for non-buttons because a direction may reference a layout, see: #112610. */
    bool use_dir = true;

    if (item->type() == ItemType::Button) {
      ButtonItem *bitem = static_cast<ButtonItem *>(item);

      bitem->but->pie_dir = dir;
      /* Scale the buttons. */
      bitem->but->rect.ymax *= 1.5f;
      /* Add a little bit more here to include number. */
      bitem->but->rect.xmax += 1.5f * UI_UNIT_X;
      /* Enable drawing as pie item if supported by widget. */
      if (item_is_radial_drawable(bitem)) {
        bitem->but->emboss = EmbossType::PieMenu;
        bitem->but->drawflag |= BUT_ICON_LEFT;
      }

      if (ELEM(bitem->but->type, ButtonType::Sepr, ButtonType::SeprLine)) {
        use_dir = false;
      }
    }

    if (use_dir) {
      this->block()->pie_data->pie_dir_mask |= 1 << int(dir);
    }

    const int2 size = item->size();

    item_position(item,
                  x + (vec[0] * pie_radius) + (factor[0] * size.x),
                  y + (vec[1] * pie_radius) + (factor[1] * size.y),
                  size.x,
                  size.y);

    minx = min_ii(minx, x + (vec[0] * pie_radius) - (size.x / 2));
    maxx = max_ii(maxx, x + (vec[0] * pie_radius) + (size.x / 2));
    miny = min_ii(miny, y + (vec[1] * pie_radius) - (size.y / 2));
    maxy = max_ii(maxy, y + (vec[1] * pie_radius) + (size.y / 2));
  }

  x_ = minx;
  y_ = miny;
  w_ = maxx - minx;
  h_ = maxy - miny;
}

void Layout::estimate_impl()
{
  /* nothing to do */
}
void Layout::resolve_impl()
{
  /* Nothing to do. */
}

void LayoutRootPieMenu::resolve_impl()
{
  /* first item is pie menu title, align on center of menu */
  Item *item = this->items().first();

  if (item->type() == ItemType::Button) {
    int x, y;
    x = x_;
    y = y_;

    const int2 size = item->size();

    item_position(
        item, x - size.x / 2, y + UI_SCALE_FAC * (U.pie_menu_threshold + 9.0f), size.x, size.y);
  }
}

/* panel header layout */
void LayoutItemPanelHeader::estimate_impl()
{
  BLI_assert(this->items().size() == 1);
  Item *item = this->items().first();

  const int2 size = item->size();
  w_ = size.x;
  h_ = size.y;
}

void LayoutItemPanelHeader::resolve_impl()
{
  Panel *panel = this->root_panel();

  BLI_assert(this->items().size() == 1);
  Item *item = this->items().first();

  const int2 size = item->size();
  y_ -= size.y;
  item_position(item, x_, y_, w_, size.y);
  panel->runtime->layout_panels.headers.append(
      {float(y_), float(y_ + h_), open_prop_owner, open_prop_name});
}

/* panel body layout */
void LayoutItemPanelBody::resolve_impl()
{
  Panel *panel = this->root_panel();
  LayoutColumn::resolve_impl();
  const int space = LayoutInternal::layout_space_get(this->parent_);
  panel->runtime->layout_panels.bodies.append({
      float(y_ - space),
      float(y_ + h_ + space),
  });
}

/* box layout */
void LayoutItemBx::estimate_impl()
{
  const uiStyle *style = this->root()->style;

  LayoutColumn::estimate_impl();

  int boxspace = style->boxspace;
  if (this->root()->type == LayoutType::Header) {
    boxspace = 0;
  }
  w_ += 2 * boxspace;
  h_ += 2 * boxspace;
}

void LayoutItemBx::resolve_impl()
{
  const uiStyle *style = this->root()->style;

  int boxspace = style->boxspace;
  if (this->root()->type == LayoutType::Header) {
    boxspace = 0;
  }

  const int w = w_;
  const int h = h_;

  x_ += boxspace;
  y_ -= boxspace;

  if (w != 0) {
    w_ -= 2 * boxspace;
  }
  if (h != 0) {
    h_ -= 2 * boxspace;
  }

  LayoutColumn::resolve_impl();

  x_ -= boxspace;
  y_ -= boxspace;

  if (w != 0) {
    w_ += 2 * boxspace;
  }
  if (h != 0) {
    h_ += 2 * boxspace;
  }

  /* roundbox around the sublayout */
  Button *but = this->roundbox;
  but->rect.xmin = x_;
  but->rect.ymin = y_;
  but->rect.xmax = x_ + w_;
  but->rect.ymax = y_ + h_;
}

/* multi-column layout, automatically flowing to the next */
void LayoutItemFlow::estimate_impl()
{
  const uiStyle *style = this->root()->style;
  LayoutItemFlow *flow = this;

  int maxw = 0;

  /* compute max needed width and total height */
  int toth = 0;
  int totitem = 0;
  for (Item *item : this->items()) {
    const int2 size = item->size();
    maxw = std::max(maxw, size.x);
    toth += size.y;
    totitem++;
  }

  if (flow->number <= 0) {
    /* auto compute number of columns, not very good */
    if (maxw == 0) {
      flow->totcol = 1;
      return;
    }

    flow->totcol = max_ii(this->root()->emw / maxw, 1);
    flow->totcol = min_ii(flow->totcol, totitem);
  }
  else {
    flow->totcol = flow->number;
  }

  /* compute sizes */
  int x = 0;
  int y = 0;
  int emy = 0;
  int miny = 0;

  maxw = 0;
  const int emh = toth / flow->totcol;

  /* create column per column */
  int col = 0;
  for (Item *item : this->items()) {
    const int2 size = item->size();

    y -= size.y + style->buttonspacey;
    miny = min_ii(miny, y);
    emy -= size.y;
    maxw = max_ii(size.x, maxw);

    /* decide to go to next one */
    if (col < flow->totcol - 1 && emy <= -emh) {
      x += maxw + space_;
      maxw = 0;
      y = 0;
      emy = 0; /* need to reset height again for next column */
      col++;
    }
  }

  w_ = x;
  h_ = y_ - miny;
}

void LayoutItemFlow::resolve_impl()
{
  const uiStyle *style = this->root()->style;
  int col, emh;

  /* compute max needed width and total height */
  int toth = 0;
  for (Item *item : this->items()) {
    const int2 size = item->size();
    toth += size.y;
  }

  /* compute sizes */
  int x = x_;
  int y = y_;
  int emy = 0;
  int miny = 0;

  emh = toth / this->totcol;

  /* create column per column */
  col = 0;
  int w = (w_ - (this->totcol - 1) * style->columnspace) / this->totcol;
  for (Item *item : this->items()) {
    int2 size = item->size();

    size.x = (this->alignment() == LayoutAlign::Expand) ? w : min_ii(w, size.x);

    y -= size.y;
    emy -= size.y;
    item_position(item, x, y, size.x, size.y);
    y -= style->buttonspacey;
    miny = min_ii(miny, y);

    /* decide to go to next one */
    if (col < this->totcol - 1 && emy <= -emh) {
      x += w + style->columnspace;
      y = y_;
      emy = 0; /* need to reset height again for next column */
      col++;

      const int remaining_width = w_ - (x - x_);
      const int remaining_width_between_columns = (this->totcol - col - 1) * style->columnspace;
      const int remaining_columns = this->totcol - col;
      w = (remaining_width - remaining_width_between_columns) / remaining_columns;
    }
  }

  h_ = y_ - miny;
  x_ = x;
  y_ = miny;
}

void LayoutItemGridFlow::estimate_impl()
{
  const uiStyle *style = this->root()->style;
  LayoutItemGridFlow *gflow = this;

  const int space_x = style->columnspace;
  const int space_y = style->buttonspacey;

  /* Estimate average needed width and height per item. */
  {
    float avg_w;
    int max_h;

    UILayoutGridFlowInput input{};
    input.row_major = gflow->row_major;
    input.even_columns = gflow->even_columns;
    input.even_rows = gflow->even_rows;
    input.litem_w = w_;
    input.litem_x = x_;
    input.litem_y = y_;
    input.space_x = space_x;
    input.space_y = space_y;
    UILayoutGridFlowOutput output{};
    output.tot_items = &gflow->tot_items;
    output.global_avg_w = &avg_w;
    output.global_max_h = &max_h;
    litem_grid_flow_compute(this->items(), &input, &output);

    if (gflow->tot_items == 0) {
      w_ = h_ = 0;
      gflow->tot_columns = gflow->tot_rows = 0;
      return;
    }

    /* Even in varying column width case,
     * we fix our columns number from weighted average width of items,
     * a proper solving of required width would be too costly,
     * and this should give reasonably good results in all reasonable cases. */
    if (gflow->columns_len > 0) {
      gflow->tot_columns = gflow->columns_len;
    }
    else {
      if (avg_w == 0.0f) {
        gflow->tot_columns = 1;
      }
      else {
        gflow->tot_columns = min_ii(max_ii(int(w_ / avg_w), 1), gflow->tot_items);
      }
    }
    gflow->tot_rows = int(ceilf(float(gflow->tot_items) / gflow->tot_columns));

    /* Try to tweak number of columns and rows to get better filling of last column or row,
     * and apply 'modulo' value to number of columns or rows.
     * Note that modulo does not prevent ending with fewer columns/rows than modulo, if mandatory
     * to avoid empty column/row. */
    {
      const int modulo = (gflow->columns_len < -1) ? -gflow->columns_len : 0;
      const int step = modulo ? modulo : 1;

      if (gflow->row_major) {
        /* Adjust number of columns to be multiple of given modulo. */
        if (modulo && gflow->tot_columns % modulo != 0 && gflow->tot_columns > modulo) {
          gflow->tot_columns = gflow->tot_columns - (gflow->tot_columns % modulo);
        }
        /* Find smallest number of columns conserving computed optimal number of rows. */
        for (gflow->tot_rows = int(ceilf(float(gflow->tot_items) / gflow->tot_columns));
             (gflow->tot_columns - step) > 0 &&
             int(ceilf(float(gflow->tot_items) / (gflow->tot_columns - step))) <= gflow->tot_rows;
             gflow->tot_columns -= step)
        {
          /* pass */
        }
      }
      else {
        /* Adjust number of rows to be multiple of given modulo. */
        if (modulo && gflow->tot_rows % modulo != 0) {
          gflow->tot_rows = min_ii(gflow->tot_rows + modulo - (gflow->tot_rows % modulo),
                                   gflow->tot_items);
        }
        /* Find smallest number of rows conserving computed optimal number of columns. */
        for (gflow->tot_columns = int(ceilf(float(gflow->tot_items) / gflow->tot_rows));
             (gflow->tot_rows - step) > 0 &&
             int(ceilf(float(gflow->tot_items) / (gflow->tot_rows - step))) <= gflow->tot_columns;
             gflow->tot_rows -= step)
        {
          /* pass */
        }
      }
    }

    /* Set evenly-spaced axes size
     * (quick optimization in case we have even columns and rows). */
    if (gflow->even_columns && gflow->even_rows) {
      w_ = int(gflow->tot_columns * avg_w) + space_x * (gflow->tot_columns - 1);
      h_ = int(gflow->tot_rows * max_h) + space_y * (gflow->tot_rows - 1);
      return;
    }
  }

  /* Now that we have our final number of columns and rows,
   * we can compute actual needed space for non-evenly sized axes. */
  {
    int tot_w, tot_h;
    UILayoutGridFlowInput input{};
    input.row_major = gflow->row_major;
    input.even_columns = gflow->even_columns;
    input.even_rows = gflow->even_rows;
    input.litem_w = w_;
    input.litem_x = x_;
    input.litem_y = y_;
    input.space_x = space_x;
    input.space_y = space_y;
    input.tot_columns = gflow->tot_columns;
    input.tot_rows = gflow->tot_rows;
    UILayoutGridFlowOutput output{};
    output.tot_w = &tot_w;
    output.tot_h = &tot_h;
    litem_grid_flow_compute(this->items(), &input, &output);

    w_ = tot_w;
    h_ = tot_h;
  }
}

void LayoutItemGridFlow::resolve_impl()
{
  const uiStyle *style = this->root()->style;

  if (this->tot_items == 0) {
    w_ = h_ = 0;
    return;
  }

  BLI_assert(this->tot_columns > 0);
  BLI_assert(this->tot_rows > 0);

  const int space_x = style->columnspace;
  const int space_y = style->buttonspacey;

  Array<int, 64> widths(this->tot_columns);
  Array<int, 64> heights(this->tot_rows);
  Array<int, 64> cos_x(this->tot_columns);
  Array<int, 64> cos_y(this->tot_rows);

  /* This time we directly compute coordinates and sizes of all cells. */
  UILayoutGridFlowInput input{};
  input.row_major = this->row_major;
  input.even_columns = this->even_columns;
  input.even_rows = this->even_rows;
  input.litem_w = w_;
  input.litem_x = x_;
  input.litem_y = y_;
  input.space_x = space_x;
  input.space_y = space_y;
  input.tot_columns = this->tot_columns;
  input.tot_rows = this->tot_rows;
  UILayoutGridFlowOutput output{};
  output.cos_x_array = cos_x.data();
  output.cos_y_array = cos_y.data();
  output.widths_array = widths.data();
  output.heights_array = heights.data();
  litem_grid_flow_compute(this->items(), &input, &output);

  int i = 0;
  for (Item *item : this->items()) {
    const int col = this->row_major ? i % this->tot_columns : i / this->tot_rows;
    const int row = this->row_major ? i / this->tot_columns : i % this->tot_rows;
    int2 size = item->size();

    const int w = widths[col];
    const int h = heights[row];
    if (this->alignment() == LayoutAlign::Expand) {
      size = {w, h};
    }
    else {
      size = {min_ii(w, size.x), min_ii(h, size.y)};
    }

    item_position(item, cos_x[col], cos_y[row], size.x, size.y);
    i++;
  }

  h_ = y_ - cos_y[this->tot_rows - 1];
  x_ = (cos_x[this->tot_columns - 1] - x_) + widths[this->tot_columns - 1];
  y_ = y_ - h_;
}

/* free layout */
void LayoutAbsolute::estimate_impl()
{
  int minx = 1e6;
  int miny = 1e6;
  w_ = 0;
  h_ = 0;

  for (Item *item : this->items()) {
    const int2 offset = item->offset();
    const int2 size = item->size();

    minx = min_ii(minx, offset.x);
    miny = min_ii(miny, offset.y);

    w_ = std::max(w_, offset.x + size.x);
    h_ = std::max(h_, offset.y + size.y);
  }

  w_ -= minx;
  h_ -= miny;
}

void LayoutAbsolute::resolve_impl()
{
  float scalex = 1.0f, scaley = 1.0f;
  int x, y, newx, newy;

  int minx = 1e6;
  int miny = 1e6;
  int totw = 0;
  int toth = 0;

  for (Item *item : this->items()) {
    const int2 offset = item->offset();
    const int2 size = item->size();

    minx = min_ii(minx, offset.x);
    miny = min_ii(miny, offset.y);

    totw = max_ii(totw, offset.x + size.x);
    toth = max_ii(toth, offset.y + size.y);
  }

  totw -= minx;
  toth -= miny;

  if (w_ && totw > 0) {
    scalex = float(w_) / float(totw);
  }
  if (h_ && toth > 0) {
    scaley = float(h_) / float(toth);
  }

  x = x_;
  y = y_ - scaley * toth;

  for (Item *item : this->items()) {
    int2 offset = item->offset();
    int2 size = item->size();

    if (scalex != 1.0f) {
      newx = (offset.x - minx) * scalex;
      size.x = (offset.x - minx + size.x) * scalex - newx;
      offset.x = minx + newx;
    }

    if (scaley != 1.0f) {
      newy = (offset.y - miny) * scaley;
      size.y = (offset.y - miny + size.y) * scaley - newy;
      offset.y = miny + newy;
    }

    item_position(item, x + offset.x - minx, y + offset.y - miny, size.x, size.y);
  }

  w_ = scalex * totw;
  h_ = y_ - y;
  x_ = x + w_;
  y_ = y;
}

/* split layout */
void LayoutItemSplit::estimate_impl()
{
  LayoutRow::estimate_impl();
  this->fixed_size_set(false);
}

void LayoutItemSplit::resolve_impl()
{
  float extra_pixel = 0.0f;
  const int tot = int(this->items().size());

  if (tot == 0) {
    return;
  }

  int x = x_;
  const int y = y_;

  const float percentage = (this->percentage == 0.0f) ? 1.0f / float(tot) : this->percentage;

  const int w = (w_ - (tot - 1) * space_);
  int colw = w * percentage;
  colw = std::max(colw, 0);

  const Item *item_last = this->items().last();
  for (Item *item : this->items()) {
    const bool is_item_last = (item == item_last);
    const int2 size = item->size();

    item_position(item, x, y - size.y, colw, size.y);
    x += colw;

    if (!is_item_last) {
      const float width = extra_pixel + (w - int(w * percentage)) / (float(tot) - 1);
      extra_pixel = width - int(width);
      colw = int(width);
      colw = std::max(colw, 0);

      x += space_;
    }
  }

  w_ = x - x_;
  h_ = y_ - y;
  x_ = x;
  y_ = y;
}

/* overlap layout */
void LayoutOverlap::estimate_impl()
{
  w_ = 0;
  h_ = 0;

  for (Item *item : this->items()) {
    const int2 size = item->size();

    w_ = std::max(size.x, w_);
    h_ = std::max(size.y, h_);
  }
}

void LayoutOverlap::resolve_impl()
{

  const int x = x_;
  const int y = y_;

  for (Item *item : this->items()) {
    const int2 size = item->size();
    item_position(item, x, y - size.y, w_, size.y);

    h_ = std::max(h_, size.y);
  }

  x_ = x;
  y_ = y - h_;
}

void LayoutInternal::init_from_parent(Layout *litem, Layout *layout, int align)
{
  litem->root_ = layout->root_;
  litem->align_ = align;
  /* Children of grid-flow layout shall never have "ideal big size" returned as estimated size. */
  litem->variable_size_ = layout->variable_size_ || layout->type() == ItemType::LayoutGridFlow;
  litem->active_ = true;
  litem->enabled_ = true;
  litem->context_ = layout->context_;
  litem->redalert_ = layout->redalert_;
  litem->w_ = layout->w_;
  litem->emboss_ = layout->emboss_;
  litem->flag_ = (layout->flag_ & (ItemInternalFlag::PropSep | ItemInternalFlag::PropDecorate |
                                   ItemInternalFlag::InsidePropSep));

  if (layout->child_items_layout_) {
    layout->child_items_layout_->items_.append(litem);
    litem->parent_ = layout->child_items_layout_;
  }
  else {
    layout->items_.append(litem);
    litem->parent_ = layout;
  }
}

Layout &Layout::row(bool align)
{
  Layout *litem = MEM_new<LayoutRow>(__func__, nullptr);
  LayoutInternal::init_from_parent(litem, this, align);

  litem->space_ = (align) ? 0 : root_->style->buttonspacex;

  block_layout_set_current(this->block(), litem);

  return *litem;
}

PanelLayout Layout::panel_prop(const bContext *C,
                               PointerRNA *open_prop_owner,
                               const StringRefNull open_prop_name)
{
  const ARegion *region = CTX_wm_region(C);

  const bool is_real_open = RNA_boolean_get(open_prop_owner, open_prop_name.c_str());
  const bool search_filter_active = region->flag & RGN_FLAG_SEARCH_FILTER_ACTIVE;
  const bool is_open = is_real_open || search_filter_active;

  PanelLayout panel_layout{};
  {
    LayoutItemPanelHeader *header_litem = MEM_new<LayoutItemPanelHeader>(__func__);
    LayoutInternal::init_from_parent(header_litem, this, false);

    header_litem->open_prop_owner = *open_prop_owner;
    header_litem->open_prop_name = open_prop_name;

    Layout *row = &header_litem->row(true);

    Block *block = row->block();

    const bool is_popup = block_is_popup_any(block);
    bool inside_layout_panel = false;

    if (is_popup) {
      Layout *parent = this;
      while (parent) {
        inside_layout_panel = parent->type_ == ItemType::LayoutPanelBody;
        parent = parent->parent_;
        if (inside_layout_panel) {
          break;
        }
      }
    }
    if (!is_popup || inside_layout_panel) {
      uiDefBut(this->block(),
               ButtonType::Sepr,
               "",
               0,
               0,
               std::round(0.85 * UI_UNIT_X - float(root_->style->panelspace)),
               0,
               nullptr,
               0.0,
               0.0,
               "");
    }
    const int icon = is_open ? ICON_DOWNARROW_HLT : ICON_RIGHTARROW;
    const int icon_width = (UI_UNIT_X * 0.9) + 1.1f * UI_SCALE_FAC;
    uiDefIconTextBut(block, ButtonType::Label, icon, "", 0, 0, icon_width, UI_UNIT_Y, nullptr, "");

    panel_layout.header = row;
  }

  if (!is_open) {
    return panel_layout;
  }

  LayoutItemPanelBody *body_litem = MEM_new<LayoutItemPanelBody>(__func__);
  body_litem->space_ = root_->style->templatespace;
  LayoutInternal::init_from_parent(body_litem, this, false);
  block_layout_set_current(this->block(), body_litem);
  panel_layout.body = body_litem;

  return panel_layout;
}

PanelLayout Layout::panel_prop_with_bool_header(const bContext *C,
                                                PointerRNA *open_prop_owner,
                                                const StringRefNull open_prop_name,
                                                PointerRNA *bool_prop_owner,
                                                const StringRefNull bool_prop_name,
                                                const std::optional<StringRef> label)
{
  PanelLayout panel_layout = this->panel_prop(C, open_prop_owner, open_prop_name);

  Layout *panel_header = panel_layout.header;
  panel_header->flag_ &= ~(ItemInternalFlag::PropSep | ItemInternalFlag::PropDecorate |
                           ItemInternalFlag::InsidePropSep);
  panel_header->prop(bool_prop_owner, bool_prop_name, UI_ITEM_NONE, label, ICON_NONE);

  return panel_layout;
}

Layout *Layout::panel_prop(const bContext *C,
                           PointerRNA *open_prop_owner,
                           const StringRefNull open_prop_name,
                           const StringRef label)
{
  PanelLayout panel_layout = this->panel_prop(C, open_prop_owner, open_prop_name);
  panel_layout.header->label(label, ICON_NONE);

  return panel_layout.body;
}

PanelLayout Layout::panel(const bContext *C, const StringRef idname, const bool default_closed)
{
  Panel *root_panel = this->root_panel();
  BLI_assert(root_panel != nullptr);

  LayoutPanelState *state = BKE_panel_layout_panel_state_ensure(
      root_panel, idname, default_closed);
  PointerRNA state_ptr = RNA_pointer_create_discrete(nullptr, RNA_LayoutPanelState, state);

  return this->panel_prop(C, &state_ptr, "is_open");
}

Layout *Layout::panel(const bContext *C,
                      const StringRef idname,
                      const bool default_closed,
                      const StringRef label)
{
  PanelLayout panel_layout = this->panel(C, idname, default_closed);
  panel_layout.header->label(label, ICON_NONE);

  return panel_layout.body;
}

Layout &Layout::row(bool align, const StringRef heading)
{
  Layout &litem = this->row(align);
  litem.heading_ = heading;
  return litem;
}

Layout &Layout::column(bool align)
{
  Layout *litem = MEM_new<LayoutColumn>(__func__, nullptr);
  LayoutInternal::init_from_parent(litem, this, align);

  litem->space_ = (align) ? 0 : root_->style->buttonspacey;

  block_layout_set_current(this->block(), litem);

  return *litem;
}

Layout &Layout::column(bool align, const StringRef heading)
{
  Layout &litem = this->column(align);
  litem.heading_ = heading;
  return litem;
}

Layout &Layout::column_flow(int number, bool align)
{
  LayoutItemFlow *flow = MEM_new<LayoutItemFlow>(__func__);
  LayoutInternal::init_from_parent(flow, this, align);

  flow->space_ = flow->align() ? 0 : root_->style->columnspace;
  flow->number = number;

  block_layout_set_current(this->block(), flow);

  return *flow;
}

Layout &Layout::grid_flow(
    bool row_major, int columns_len, bool even_columns, bool even_rows, bool align)
{
  LayoutItemGridFlow *flow = MEM_new<LayoutItemGridFlow>(__func__);
  LayoutInternal::init_from_parent(flow, this, align);

  flow->space_ = flow->align() ? 0 : root_->style->columnspace;
  flow->row_major = row_major;
  flow->columns_len = columns_len;
  flow->even_columns = even_columns;
  flow->even_rows = even_rows;

  block_layout_set_current(this->block(), flow);

  return *flow;
}

Layout &Layout::menu_pie()
{
  /* radial layouts are only valid for radial menus */
  if (root_->type != LayoutType::PieMenu) {
    return *item_local_sublayout(this, this, false);
  }

  /* only one radial wheel per root layout is allowed, so check and return that, if it exists */
  for (Item *item : root_->layout->items()) {
    if (item->type() == ItemType::LayoutRadial) {
      Layout *litem = static_cast<Layout *>(item);
      block_layout_set_current(this->block(), litem);
      return *litem;
    }
  }

  Layout *litem = MEM_new<LayoutRadial>(__func__);
  LayoutInternal::init_from_parent(litem, this, false);

  block_layout_set_current(this->block(), litem);

  return *litem;
}

Layout &Layout::box()
{
  return *layout_box(this, ButtonType::Roundbox);
}

Layout &Layout::list_box(uiList *ui_list, PointerRNA *actptr, PropertyRNA *actprop)
{
  LayoutItemBx *item_box = layout_box(this, ButtonType::ListBox);
  Button *but = item_box->roundbox;

  but->custom_data = ui_list;

  but->rnapoin = *actptr;
  but->rnaprop = actprop;

  /* only for the undo string */
  if (but->flag & BUT_UNDO) {
    but->tip = RNA_property_description(actprop);
  }

  return *item_box;
}

Layout &Layout::absolute(bool align)
{
  Layout *litem = MEM_new<LayoutAbsolute>(__func__);
  LayoutInternal::init_from_parent(litem, this, align);

  block_layout_set_current(this->block(), litem);

  return *litem;
}

Layout &Layout::overlap()
{
  Layout *litem = MEM_new<LayoutOverlap>(__func__);
  LayoutInternal::init_from_parent(litem, this, false);

  block_layout_set_current(this->block(), litem);

  return *litem;
}

Layout &Layout::split(float percentage, bool align)
{
  LayoutItemSplit *split = MEM_new<LayoutItemSplit>(__func__);
  LayoutInternal::init_from_parent(split, this, align);

  split->space_ = root_->style->columnspace;
  split->percentage = percentage;

  block_layout_set_current(this->block(), split);

  return *split;
}

void Layout::emboss_set(EmbossType emboss)
{
  emboss_ = emboss;
}

bool Layout::use_property_split() const
{
  return flag_is_set(flag_, ItemInternalFlag::PropSep);
}

void Layout::use_property_split_set(bool is_sep)
{
  SET_FLAG_FROM_TEST(flag_, is_sep, ItemInternalFlag::PropSep);
}

bool Layout::use_property_decorate() const
{
  return flag_is_set(flag_, ItemInternalFlag::PropDecorate);
}

void Layout::use_property_decorate_set(bool is_sep)
{
  SET_FLAG_FROM_TEST(flag_, is_sep, ItemInternalFlag::PropDecorate);
}

Panel *Layout::root_panel() const
{
  return this->block()->panel;
}

EmbossType Layout::emboss() const
{
  if (emboss_ == EmbossType::Undefined) {
    return this->block()->emboss;
  }
  return emboss_;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layout
 * \{ */

void Layout::estimate()
{
  if (this->items().is_empty()) {
    w_ = 0;
    h_ = 0;
    return;
  }

  for (Item *subitem : this->items()) {
    if (subitem->type() == ItemType::Button) {
      continue;
    }
    static_cast<Layout *>(subitem)->estimate();
  }

  if (this->scale_x() != 0.0f || this->scale_y() != 0.0f) {
    item_scale(this, float2{this->scale_x(), this->scale_y()});
  }
  this->estimate_impl();

  /* Force fixed size. */
  if (this->ui_units_x() > 0) {
    w_ = UI_UNIT_X * this->ui_units_x();
  }
  if (this->ui_units_y() > 0) {
    h_ = UI_UNIT_Y * this->ui_units_y();
  }
}

void Layout::resolve()
{

  if (this->items().is_empty()) {
    return;
  }

  if (this->align()) {
    item_align(this, ++this->block()->alignnr);
  }
  if (!this->active()) {
    item_flag(this, BUT_INACTIVE);
  }
  if (!this->enabled()) {
    item_flag(this, BUT_DISABLED);
  }
  this->resolve_impl();

  for (Item *subitem : this->items()) {
    if (ItemInternal::box_item(this)) {
      ItemInternal::box_item_set(subitem, true);
    }
    if (subitem->type() == ItemType::Button) {
      if (ItemInternal::box_item(this)) {
        ButtonItem *sub_bitem = static_cast<ButtonItem *>(subitem);
        sub_bitem->but->drawflag |= BUT_BOX_ITEM;
      }
      continue;
    }
    static_cast<Layout *>(subitem)->resolve();
  }
}

Block *Layout::block() const
{
  return root_->block;
}

wm::OpCallContext Layout::operator_context() const
{
  return root_->opcontext;
}

void LayoutInternal::layout_add_but(Layout *layout, Button *but)
{
  ButtonItem *bitem = MEM_new<ButtonItem>(__func__);
  bitem->but = but;

  int2 size = bitem->size();
  /* XXX Button hasn't scaled yet
   * we can flag the button as not expandable, depending on its size */
  if (size.x <= 2 * UI_UNIT_X && but->str.empty()) {
    bitem->fixed_size_set(true);
  }

  if (layout->child_items_layout_) {
    layout->child_items_layout_->items_.append(bitem);
  }
  else {
    layout->items_.append(bitem);
  }
  but->layout = layout;
  but->search_weight = layout->search_weight_;

  if (layout->context_) {
    but->context = layout->context_;
    layout->context_->used = true;
  }

  if (layout->emboss_ != EmbossType::Undefined) {
    but->emboss = layout->emboss_;
  }

  button_group_add_but(layout->block(), but);
}


ButtonItem *LayoutInternal::layout_find_button_item(const Layout *layout, const Button *but)
{
  const Vector<Item *> &child_list = layout->child_items_layout_ ?
                                         layout->child_items_layout_->items() :
                                         layout->items();

  for (Item *item : child_list) {
    if (item->type() == ItemType::Button) {
      ButtonItem *bitem = static_cast<ButtonItem *>(item);

      if (bitem->but == but) {
        return bitem;
      }
    }
    else {
      ButtonItem *nested_item = LayoutInternal::layout_find_button_item(
          static_cast<Layout *>(item), but);
      if (nested_item) {
        return nested_item;
      }
    }
  }

  return nullptr;
}

void LayoutInternal::layout_remove_but(Layout *layout, const Button *but)
{
  Vector<Item *> &child_list = layout->child_items_layout_ ? layout->child_items_layout_->items_ :
                                                             layout->items_;
  const int64_t removed_num = child_list.remove_if([but](auto item) {
    if (item->type() == ItemType::Button) {
      ButtonItem *bitem = static_cast<ButtonItem *>(item);
      return (bitem->but == but);
    }
    return false;
  });

  BLI_assert(removed_num <= 1);
  UNUSED_VARS_NDEBUG(removed_num);
}


void Item::fixed_size_set(bool fixed_size)
{
  SET_FLAG_FROM_TEST(flag_, fixed_size, ItemInternalFlag::FixedSize);
}

bool Item::fixed_size() const
{
  return flag_is_set(flag_, ItemInternalFlag::FixedSize);
}

void Layout::operator_context_set(wm::OpCallContext opcontext)
{
  root_->opcontext = opcontext;
}

const PointerRNA *Layout::context_ptr_get(const StringRef name, const StructRNA *type) const
{
  if (!context_) {
    return nullptr;
  }
  return CTX_store_ptr_lookup(context_, name, type);
}

void Layout::context_ptr_set(StringRef name, const PointerRNA *ptr)
{
  Block *block = this->block();
  context_ = CTX_store_add(block->contexts, name, ptr);
}
std::optional<StringRefNull> Layout::context_string_get(const StringRef name) const
{
  if (!context_) {
    return std::nullopt;
  }
  return CTX_store_string_lookup(context_, name);
}

void Layout::context_string_set(StringRef name, StringRef value)
{
  Block *block = this->block();
  context_ = CTX_store_add(block->contexts, name, value);
}

std::optional<int64_t> Layout::context_int_get(const StringRef name) const
{
  if (!context_) {
    return std::nullopt;
  }
  return CTX_store_int_lookup(context_, name);
}

void Layout::context_int_set(StringRef name, int64_t value)
{
  Block *block = this->block();
  context_ = CTX_store_add(block->contexts, name, value);
}

void Layout::context_copy(const bContextStore *context)
{
  Block *block = this->block();
  context_ = CTX_store_add_all(block->contexts, context);
}

void Layout::context_set_from_but(const Button *but)
{
  if (but->opptr) {
    this->context_ptr_set("button_operator", but->opptr);
  }

  if (but->rnapoin.data && but->rnaprop) {
    /* TODO: index could be supported as well */
    PointerRNA ptr_prop = RNA_pointer_create_discrete(nullptr, RNA_Property, but->rnaprop);
    this->context_ptr_set("button_prop", &ptr_prop);
    this->context_ptr_set("button_pointer", &but->rnapoin);
  }
}
/** \} */

LayoutRoot *Layout::root() const
{
  return root_;
};
const bContextStore *Layout::context() const
{
  return context_;
};
Layout *Layout::parent() const
{
  return parent_;
};
StringRef Layout::heading() const
{
  return heading_;
};
void Layout::heading_reset()
{
  heading_ = {};
}
Span<Item *> Layout::items() const
{
  return items_;
};
bool Layout::align() const
{
  return align_;
}
bool Layout::variable_size() const
{
  return variable_size_;
}
EmbossType Layout::emboss_or_undefined() const
{
  return emboss_;
}

}  // namespace ui
}  // namespace blender
