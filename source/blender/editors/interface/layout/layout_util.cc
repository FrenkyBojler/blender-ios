/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_path_templates.hh"
#include "BKE_screen.hh"
#include "BLI_math_base.h"
#include "BLI_path_utils.hh"
#include "BLI_string_ref.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_interface_types.hh"
#include "interface_intern.hh"
#include "layout_containers.hh"

#include "layout_intern.hh"

#include "RNA_prototypes.hh"
#include "WM_keymap.hh"

#include "BLI_listbase.h"
#include "BLT_translation.hh"
#include "DNA_screen_types.h"
#include "WM_api.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Item
 * \{ */

StringRef item_name_add_colon(StringRef name, char namestr[UI_MAX_NAME_STR])
{
  const int len = name.size();

  if (len != 0 && len + 1 < UI_MAX_NAME_STR) {
    memcpy(namestr, name.data(), len);
    namestr[len] = ':';
    namestr[len + 1] = '\0';
    return namestr;
  }

  return name;
}

StringRefNull item_name_add_colon(StringRefNull name, char namestr[UI_MAX_NAME_STR])
{
  const int len = name.size();

  if (len != 0 && len + 1 < UI_MAX_NAME_STR) {
    memcpy(namestr, name.data(), len);
    namestr[len] = ':';
    namestr[len + 1] = '\0';
    return namestr;
  }

  return name;
}

int item_fit(const int item,
                    const int pos,
                    const int all,
                    const int available,
                    const bool is_last,
                    const LayoutAlign alignment,
                    float *extra_pixel)
{
  /* available == 0 is unlimited */
  if (ELEM(0, available, all)) {
    return item;
  }

  if (all > available) {
    /* contents is bigger than available space */
    if (is_last) {
      return available - pos;
    }

    const float width = *extra_pixel + (item * available) / float(all);
    *extra_pixel = width - int(width);
    return int(width);
  }

  /* contents is smaller or equal to available space */
  if (alignment == LayoutAlign::Expand) {
    if (is_last) {
      return available - pos;
    }

    const float width = *extra_pixel + (item * available) / float(all);
    *extra_pixel = width - int(width);
    return int(width);
  }
  return item;
}

int layout_vary_direction(Layout *layout)
{
  return ((ELEM(layout->root()->type, LayoutType::Header, LayoutType::PieMenu) ||
           (layout->alignment() != LayoutAlign::Expand)) ?
              UI_ITEM_VARY_X :
              UI_ITEM_VARY_Y);
}

bool layout_variable_size(Layout *layout)
{
  /* Note that this code is probably a bit unreliable, we'd probably want to know whether it's
   * variable in X and/or Y, etc. But for now it mimics previous one,
   * with addition of variable flag set for children of grid-flow layouts. */
  return layout_vary_direction(layout) == UI_ITEM_VARY_X || layout->variable_size();
}

/**
 * Estimated size of text + icon.
 */
int text_icon_width_ex(Layout *layout,
                              const StringRef name,
                              int icon,
                              const TextIconPadFactor &pad_factor,
                              const uiFontStyle *fstyle)
{
  const int unit_x = UI_UNIT_X * (layout->scale_x() ? layout->scale_x() : 1.0f);

  /* When there is no text, always behave as if this is an icon-only button
   * since it's not useful to return empty space. */
  if (icon && name.is_empty()) {
    return unit_x * (1.0f + pad_factor.icon_only);
  }

  if (layout_variable_size(layout)) {
    if (!icon && name.is_empty()) {
      return unit_x * (1.0f + pad_factor.icon_only);
    }

    if (layout->alignment() != LayoutAlign::Expand) {
      layout->fixed_size_set(true);
    }

    float margin = pad_factor.text;
    if (icon) {
      margin += pad_factor.icon;
    }

    const float aspect = layout->block()->aspect;
    return fontstyle_string_width_with_block_aspect(fstyle, name, aspect) +
           int(ceilf(unit_x * margin));
  }
  return unit_x * 10;
}

int text_icon_width(Layout *layout,
                           const StringRef name,
                           const int icon,
                           const bool compact)
{
  return text_icon_width_ex(
      layout, name, icon, compact ? text_pad_compact : text_pad_default, UI_FSTYLE_WIDGET);
}

void item_position(Item *item, const int x, const int y, const int w, const int h)
{
  if (item->type() == ItemType::Button) {
    ButtonItem *bitem = static_cast<ButtonItem *>(item);

    bitem->but->rect.xmin = x;
    bitem->but->rect.ymin = y;
    bitem->but->rect.xmax = x + w;
    bitem->but->rect.ymax = y + h;

    button_update(bitem->but); /* For `strlen`. */
  }
  else {
    LayoutInternal::layout_offset_size_set(static_cast<Layout *>(item), x, y + h, w, h);
  }
}

void item_move(Item *item, const int delta_xmin, const int delta_xmax)
{
  if (item->type() == ItemType::Button) {
    ButtonItem *bitem = static_cast<ButtonItem *>(item);

    bitem->but->rect.xmin += delta_xmin;
    bitem->but->rect.xmax += delta_xmax;

    button_update(bitem->but); /* For `strlen`. */
  }
  else {
    LayoutInternal::layout_move(static_cast<Layout *>(item), delta_xmin, delta_xmax);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Special RNA Items
 * \{ */

Layout *item_local_sublayout(Layout *test, Layout *layout, bool align)
{
  Layout *sub;
  if (test->local_direction() == LayoutDirection::Horizontal) {
    sub = &layout->row(align);
  }
  else {
    sub = &layout->column(align);
  }

  LayoutInternal::layout_space_set(sub, 0);
  return sub;
}

void layer_but_cb(bContext *C, void *arg_but, void *arg_index)
{
  wmWindow *win = CTX_wm_window(C);
  Button *but = static_cast<Button *>(arg_but);
  PointerRNA *ptr = &but->rnapoin;
  PropertyRNA *prop = but->rnaprop;
  const int index = POINTER_AS_INT(arg_index);
  const bool shift = win->runtime->eventstate->modifier & KM_SHIFT;
  const int len = RNA_property_array_length(ptr, prop);

  if (!shift) {
    BLI_assert(index < len);
    Array<bool, RNA_STACK_ARRAY> value_array(len);
    value_array.fill(false);
    value_array[index] = true;

    RNA_property_boolean_set_array(ptr, prop, value_array.data());

    RNA_property_update(C, ptr, prop);

    for (Button &cbut : but->block->buttons()) {
      button_update(&cbut);
    }
  }
}

/* create buttons for an item with an RNA array */
void item_array(Layout *layout,
                       Block *block,
                       const StringRef name,
                       int icon,
                       PointerRNA *ptr,
                       PropertyRNA *prop,
                       const int len,
                       int x,
                       const int y,
                       int w,
                       const int /*h*/,
                       const bool expand,
                       const bool slider,
                       const int toggle,
                       const bool icon_only,
                       const bool compact,
                       const bool show_text)
{
  const uiStyle *style = layout->root()->style;

  /* retrieve type and subtype */
  const PropertyType type = RNA_property_type(prop);
  const PropertySubType subtype = RNA_property_subtype(prop);

  Layout *sub = item_local_sublayout(layout, layout, true);
  block_layout_set_current(block, sub);

  /* create label */
  if (!name.is_empty() && show_text) {
    uiDefBut(block, ButtonType::Label, name, 0, 0, w, UI_UNIT_Y, nullptr, 0.0, 0.0, "");
  }

  /* create buttons */
  if (type == PROP_BOOLEAN && ELEM(subtype, PROP_LAYER, PROP_LAYER_MEMBER)) {
    /* special check for layer layout */
    const int cols = (len >= 20) ? 2 : 1;
    const int colbuts = len / (2 * cols);
    uint layer_used = 0;
    uint layer_active = 0;

    block_layout_set_current(block, &layout->absolute(false));

    const int butw = UI_UNIT_X * 0.75;
    const int buth = UI_UNIT_X * 0.75;

    for (int b = 0; b < cols; b++) {
      block_align_begin(block);

      for (int a = 0; a < colbuts; a++) {
        const int layer_num = a + b * colbuts;
        const uint layer_flag = (1u << layer_num);

        if (layer_used & layer_flag) {
          if (layer_active & layer_flag) {
            icon = ICON_LAYER_ACTIVE;
          }
          else {
            icon = ICON_LAYER_USED;
          }
        }
        else {
          icon = ICON_BLANK1;
        }

        Button *but = uiDefAutoButR(
            block, ptr, prop, layer_num, "", icon, x + butw * a, y + buth, butw, buth);
        if (subtype == PROP_LAYER_MEMBER) {
          button_func_set(but, layer_but_cb, but, POINTER_FROM_INT(layer_num));
        }
      }
      for (int a = 0; a < colbuts; a++) {
        const int layer_num = a + len / 2 + b * colbuts;
        const uint layer_flag = (1u << layer_num);

        if (layer_used & layer_flag) {
          if (layer_active & layer_flag) {
            icon = ICON_LAYER_ACTIVE;
          }
          else {
            icon = ICON_LAYER_USED;
          }
        }
        else {
          icon = ICON_BLANK1;
        }

        Button *but = uiDefAutoButR(
            block, ptr, prop, layer_num, "", icon, x + butw * a, y, butw, buth);
        if (subtype == PROP_LAYER_MEMBER) {
          button_func_set(but, layer_but_cb, but, POINTER_FROM_INT(layer_num));
        }
      }
      block_align_end(block);

      x += colbuts * butw + style->buttonspacex;
    }
  }
  else if (subtype == PROP_MATRIX) {
    int totdim, dim_size[/*RNA_MAX_ARRAY_DIMENSION*/ 3];
    int row, col;

    block_layout_set_current(block, &layout->absolute(true));

    totdim = RNA_property_array_dimension(ptr, prop, dim_size);
    if (totdim != 2) {
      /* Only 2D matrices supported in UI so far. */
      return;
    }

    w /= dim_size[1];
    // h /= dim_size[0]; /* UNUSED */

    for (int a = 0; a < len; a++) {
      /* We are going over flat array indices (the way matrices are stored internally [also check
       * logic in #pyrna_py_from_array_index()]) -- and they are not ordered "row first" -- , so
       * map these to rows/columns. */
      col = a % dim_size[1];
      row = a / dim_size[1];
      std::optional<ButtonType> button_type = slider ? std::optional(ButtonType::NumSlider) :
                                                       std::nullopt;
      uiDefAutoButR(block,
                    ptr,
                    prop,
                    a,
                    "",
                    ICON_NONE,
                    x + w * col,
                    y + (dim_size[0] * UI_UNIT_Y) - (row * UI_UNIT_Y),
                    w,
                    UI_UNIT_Y,
                    button_type);
    }
  }
  else if (subtype == PROP_DIRECTION && !expand) {
    uiDefButR_prop(block,
                   ButtonType::Unitvec,
                   name,
                   x,
                   y,
                   UI_UNIT_X * 3,
                   UI_UNIT_Y * 3,
                   ptr,
                   prop,
                   -1,
                   0,
                   0,
                   std::nullopt);
  }
  else {
    /* NOTE: this block of code is a bit arbitrary and has just been made
     * to work with common cases, but may need to be re-worked */

    /* special case, boolean array in a menu, this could be used in a more generic way too */
    if (ELEM(subtype, PROP_COLOR, PROP_COLOR_GAMMA) && !expand && ELEM(len, 3, 4)) {
      uiDefAutoButR(block, ptr, prop, -1, "", ICON_NONE, 0, 0, w, UI_UNIT_Y);
    }
    else {
      /* Even if 'expand' is false, we expand anyway. */

      /* Layout for known array sub-types. */
      char str[3] = {'\0'};

      if (!icon_only && show_text) {
        if (type != PROP_BOOLEAN) {
          str[1] = ':';
        }
      }

      /* Show check-boxes for rna on a non-emboss block (menu for eg). */
      bool *boolarr = nullptr;
      if (type == PROP_BOOLEAN &&
          ELEM(layout->block()->emboss, EmbossType::None, EmbossType::Pulldown))
      {
        boolarr = MEM_new_array_zeroed<bool>(len, __func__);
        RNA_property_boolean_get_array(ptr, prop, boolarr);
      }

      const char *str_buf = show_text ? str : "";
      for (int a = 0; a < len; a++) {
        if (!icon_only && show_text) {
          str[0] = RNA_property_array_item_char(prop, a);
        }
        if (boolarr) {
          icon = boolarr[a] ? ICON_CHECKBOX_HLT : ICON_CHECKBOX_DEHLT;
        }

        const int width_item = ((compact && type == PROP_BOOLEAN) ?
                                    min_ii(w, text_icon_width(layout, str_buf, icon, false)) :
                                    w);
        std::optional<ButtonType> button_type = slider ? std::optional(ButtonType::NumSlider) :
                                                         std::nullopt;
        Button *but = uiDefAutoButR(
            block, ptr, prop, a, str_buf, icon, 0, 0, width_item, UI_UNIT_Y, button_type);
        if ((toggle == 1) && but->type == ButtonType::Checkbox) {
          but->type = ButtonType::Toggle;
        }
        if ((a == 0) && (subtype == PROP_AXISANGLE)) {
          button_unit_type_set(but, PROP_UNIT_ROTATION);
        }
      }

      if (boolarr) {
        MEM_delete(boolarr);
      }
    }
  }

  block_layout_set_current(block, layout);
}

void item_enum_expand_handle(bContext *C, void *arg1, void *arg2)
{
  wmWindow *win = CTX_wm_window(C);

  if ((win->runtime->eventstate->modifier & KM_SHIFT) == 0) {
    Button *but = static_cast<Button *>(arg1);
    const int enum_value = POINTER_AS_INT(arg2);

    int current_value = RNA_property_enum_get(&but->rnapoin, but->rnaprop);
    if (!(current_value & enum_value)) {
      current_value = enum_value;
    }
    else {
      current_value &= enum_value;
    }
    RNA_property_enum_set(&but->rnapoin, but->rnaprop, current_value);
  }
}

/**
 * Draw a single enum button, a utility for #item_enum_expand_exec
 */
void item_enum_expand_elem_exec(Layout *layout,
                                       Block *block,
                                       PointerRNA *ptr,
                                       PropertyRNA *prop,
                                       const std::optional<StringRef> uiname,
                                       const int h,
                                       const ButtonType but_type,
                                       const bool icon_only,
                                       const EnumPropertyItem *item,
                                       const bool is_first)
{
  const char *name = (!uiname || !uiname->is_empty()) ? item->name : "";
  const int icon = item->icon;
  const int value = item->value;
  const int itemw = text_icon_width(block->curlayout, icon_only ? "" : name, icon, false);

  Button *but;
  if (icon && name[0] && !icon_only) {
    but = uiDefIconTextButR_prop(
        block, but_type, icon, name, 0, 0, itemw, h, ptr, prop, -1, 0, value, std::nullopt);
  }
  else if (icon) {
    const int w = (is_first) ? itemw : ceilf(itemw - U.pixelsize);
    but = uiDefIconButR_prop(
        block, but_type, icon, 0, 0, w, h, ptr, prop, -1, 0, value, std::nullopt);
  }
  else {
    but = uiDefButR_prop(
        block, but_type, name, 0, 0, itemw, h, ptr, prop, -1, 0, value, std::nullopt);
  }

  if (RNA_property_flag(prop) & PROP_ENUM_FLAG) {
    /* If this is set, assert since we're clobbering someone else's callback. */
    /* Buttons get their block's func by default, so we cannot assert in that case either. */
    BLI_assert(ELEM(but->func, nullptr, block->func));
    button_func_set(but, item_enum_expand_handle, but, POINTER_FROM_INT(value));
  }

  if (layout->local_direction() != LayoutDirection::Horizontal) {
    but->drawflag |= BUT_TEXT_LEFT;
  }

  /* Allow quick, inaccurate swipe motions to switch tabs
   * (no need to keep cursor over them). */
  if (but_type == ButtonType::Tab) {
    but->flag |= BUT_DRAG_LOCK;
  }
}

void item_enum_expand_exec(Layout *layout,
                                  Block *block,
                                  PointerRNA *ptr,
                                  PropertyRNA *prop,
                                  const std::optional<StringRef> uiname,
                                  const int h,
                                  const ButtonType but_type,
                                  const bool icon_only)
{
  /* XXX: The way this function currently handles uiname parameter
   * is insane and inconsistent with general UI API:
   *
   * - uiname is the *enum property* label.
   * - when it is nullptr or empty, we do not draw *enum items* labels,
   *   this doubles the icon_only parameter.
   * - we *never* draw (i.e. really use) the enum label uiname, it is just used as a mere flag!
   *
   * Unfortunately, fixing this implies an API "soft break", so better to defer it for later... :/
   * - mont29
   */

  BLI_assert(RNA_property_type(prop) == PROP_ENUM);

  const bool radial = (layout->root()->type == LayoutType::PieMenu);

  bool free;
  const EnumPropertyItem *item_array;
  if (radial) {
    RNA_property_enum_items_gettexted_all(
        static_cast<bContext *>(block->evil_C), ptr, prop, &item_array, nullptr, &free);
  }
  else {
    RNA_property_enum_items_gettexted(
        static_cast<bContext *>(block->evil_C), ptr, prop, &item_array, nullptr, &free);
  }

  /* We don't want nested rows, cols in menus. */
  Layout *layout_radial = nullptr;
  if (radial) {
    if (layout->root()->layout == layout) {
      layout_radial = &layout->menu_pie();
      block_layout_set_current(block, layout_radial);
    }
    else {
      if (layout->type() == ItemType::LayoutRadial) {
        layout_radial = layout;
      }
      block_layout_set_current(block, layout);
    }
  }
  else if (ELEM(layout->type(), ItemType::LayoutGridFlow, ItemType::LayoutColumnFlow) ||
           layout->root()->type == LayoutType::Menu)
  {
    block_layout_set_current(block, layout);
  }
  else {
    block_layout_set_current(block, item_local_sublayout(layout, layout, true));
  }

  for (const EnumPropertyItem *item = item_array; item->identifier; item++) {
    const bool is_first = item == item_array;

    if (!item->identifier[0]) {
      const EnumPropertyItem *next_item = item + 1;

      /* Separate items, potentially with a label. */
      if (next_item->identifier) {
        /* Item without identifier but with name:
         * Add group label for the following items. */
        if (item->name) {
          if (!is_first) {
            block->curlayout->separator();
          }
          block->curlayout->label(item->name, item->icon);
        }
        else if (radial && layout_radial) {
          layout_radial->separator();
        }
        else {
          block->curlayout->separator();
        }
      }
      continue;
    }

    item_enum_expand_elem_exec(
        layout, block, ptr, prop, uiname, h, but_type, icon_only, item, is_first);
  }

  block_layout_set_current(block, layout);

  if (free) {
    MEM_delete(item_array);
  }
}

void item_enum_expand(Layout *layout,
                             Block *block,
                             PointerRNA *ptr,
                             PropertyRNA *prop,
                             const std::optional<StringRef> uiname,
                             const int h,
                             const bool icon_only)
{
  item_enum_expand_exec(layout, block, ptr, prop, uiname, h, ButtonType::Row, icon_only);
}

void item_enum_expand_tabs(Layout *layout,
                                  bContext *C,
                                  Block *block,
                                  PointerRNA *ptr,
                                  PropertyRNA *prop,
                                  PointerRNA *ptr_highlight,
                                  PropertyRNA *prop_highlight,
                                  const std::optional<StringRef> uiname,
                                  const int h,
                                  const bool icon_only,
                                  EnumTabExpand expand_as)
{
  const int start_size = block->buttons_ptrs.size();

  item_enum_expand_exec(layout,
                        block,
                        ptr,
                        prop,
                        uiname,
                        h,
                        expand_as == EnumTabExpand::Default ? ButtonType::Tab : ButtonType::Row,
                        icon_only);

  if (block->buttons_ptrs.is_empty()) {
    return;
  }

  BLI_assert(start_size != block->buttons_ptrs.size());

  if (expand_as == EnumTabExpand::Default) {
    for (Button &tab : block->buttons() | std::views::drop(start_size)) {
      button_drawflag_enable(&tab, button_align_opposite_to_area_align_get(CTX_wm_region(C)));
      if (icon_only) {
        button_drawflag_enable(&tab, BUT_HAS_QUICK_TOOLTIP);
      }
    }
  }

  const bool use_custom_highlight = (prop_highlight != nullptr);

  if (use_custom_highlight) {
    const int highlight_array_len = RNA_property_array_length(ptr_highlight, prop_highlight);
    Array<bool, 64> highlight_array(highlight_array_len);
    RNA_property_boolean_get_array(ptr_highlight, prop_highlight, highlight_array.data());
    const int end = std::min<int>(start_size + highlight_array_len, block->buttons_ptrs.size());
    for (int i = start_size; i < end; i++) {
      Button *tab_but = block->buttons_ptrs[i].get();
      SET_FLAG_FROM_TEST(tab_but->flag, !highlight_array[i - start_size], BUT_INACTIVE);
    }
  }
}

/* callback for keymap item change button */
void keymap_but_cb(bContext * /*C*/, void *but_v, void * /*key_v*/)
{
  Button *but = static_cast<Button *>(but_v);
  BLI_assert(but->type == ButtonType::HotkeyEvent);
  const ButtonHotkeyEvent *hotkey_but = static_cast<ButtonHotkeyEvent *>(but);

  RNA_int_set(
      &but->rnapoin, "shift", (hotkey_but->modifier_key & KM_SHIFT) ? KM_MOD_HELD : KM_NOTHING);
  RNA_int_set(
      &but->rnapoin, "ctrl", (hotkey_but->modifier_key & KM_CTRL) ? KM_MOD_HELD : KM_NOTHING);
  RNA_int_set(
      &but->rnapoin, "alt", (hotkey_but->modifier_key & KM_ALT) ? KM_MOD_HELD : KM_NOTHING);
  RNA_int_set(
      &but->rnapoin, "oskey", (hotkey_but->modifier_key & KM_OSKEY) ? KM_MOD_HELD : KM_NOTHING);
  RNA_int_set(
      &but->rnapoin, "hyper", (hotkey_but->modifier_key & KM_HYPER) ? KM_MOD_HELD : KM_NOTHING);
}

Button *item_with_label(Layout *layout,
                               Block *block,
                               const StringRef name,
                               const int icon,
                               PointerRNA *ptr,
                               PropertyRNA *prop,
                               const int index,
                               const int x,
                               const int y,
                               const int w_hint,
                               const int h,
                               const int flag,
                               std::optional<ButtonType> button_type_override,
                               const char *caller_fn_name)
{
  Layout *sub = layout;
  int prop_but_width = w_hint;
#ifdef UI_PROP_DECORATE
  Layout *layout_prop_decorate = nullptr;
  const bool use_prop_sep = layout->use_property_split();
  const bool use_prop_decorate = use_prop_sep && layout->use_property_decorate() &&
                                 !ItemInternal::use_property_decorate_no_pad(layout);
#endif

  const bool is_keymapitem_ptr = RNA_struct_is_a(ptr->type, RNA_KeyMapItem);
  if ((flag & ITEM_R_FULL_EVENT) && !is_keymapitem_ptr) {
    RNA_warning_bare("%s: Data is not a keymap item struct: %s. Ignoring 'full_event' option.",
                     caller_fn_name,
                     RNA_struct_identifier(ptr->type));
  }

  block_layout_set_current(block, layout);

  /* Only add new row if more than 1 item will be added. */
  if (!name.is_empty()
#ifdef UI_PROP_DECORATE
      || use_prop_decorate
#endif
  )
  {
    /* Also avoid setting 'align' if possible. Set the space to zero instead as aligning a large
     * number of labels can end up aligning thousands of buttons when displaying key-map search (a
     * heavy operation), see: #78636. */
    sub = &layout->row(layout->align());
    LayoutInternal::layout_space_set(sub, 0);
  }

  if (!name.is_empty()) {
#ifdef UI_PROP_DECORATE
    if (use_prop_sep) {
      layout_prop_decorate = uiItemL_respect_property_split(layout, name, ICON_NONE);
    }
    else
#endif
    {
      int w_label;
      if (layout_variable_size(layout)) {
        /* In this case, a pure label without additional padding.
         * Use a default width for property button(s). */
        prop_but_width = UI_UNIT_X * 5;
        w_label = text_icon_width_ex(layout, name, ICON_NONE, text_pad_none, UI_FSTYLE_WIDGET);
      }
      else {
        w_label = w_hint / 3;
      }
      uiDefBut(block, ButtonType::Label, name, x, y, w_label, h, nullptr, 0.0, 0.0, "");
    }
  }

  const PropertyType type = RNA_property_type(prop);
  const PropertySubType subtype = RNA_property_subtype(prop);

  Button *but;
  if (ELEM(subtype, PROP_FILEPATH, PROP_DIRPATH)) {
    block_layout_set_current(block, &sub->row(true));
    but = uiDefAutoButR(block,
                        ptr,
                        prop,
                        index,
                        "",
                        icon,
                        x,
                        y,
                        prop_but_width - UI_UNIT_X,
                        h,
                        button_type_override);

    if (but != nullptr) {
      if (ELEM(subtype, PROP_FILEPATH, PROP_DIRPATH)) {
        if ((RNA_property_flag(prop) & PROP_PATH_SUPPORTS_BLEND_RELATIVE) == 0) {
          if (BLI_path_is_rel(but->drawstr.c_str())) {
            button_flag_enable(but, BUT_REDALERT);
          }
        }
      }
    }

    /* #BUTTONS_OT_file_browse calls #context_active_but_prop_get_filebrowser. */
    uiDefIconButO(block,
                  ButtonType::But,
                  subtype == PROP_DIRPATH ? "BUTTONS_OT_directory_browse" :
                                            "BUTTONS_OT_file_browse",
                  wm::OpCallContext::InvokeDefault,
                  ICON_FILEBROWSER,
                  x,
                  y,
                  UI_UNIT_X,
                  h,
                  std::nullopt);
  }
  else if (flag & ITEM_R_EVENT) {
    but = uiDefButR_prop(block,
                         ButtonType::KeyEvent,
                         name,
                         x,
                         y,
                         prop_but_width,
                         h,
                         ptr,
                         prop,
                         index,
                         0,
                         0,
                         std::nullopt);
  }
  else if ((flag & ITEM_R_FULL_EVENT) && is_keymapitem_ptr) {
    std::string kmi_str =
        WM_keymap_item_to_string(static_cast<const wmKeyMapItem *>(ptr->data), false).value_or("");

    but = uiDefButR_prop(block,
                         ButtonType::HotkeyEvent,
                         kmi_str,
                         x,
                         y,
                         prop_but_width,
                         h,
                         ptr,
                         prop,
                         0,
                         0,
                         0,
                         std::nullopt);
    button_func_set(but, keymap_but_cb, but, nullptr);
  }
  else {
    const std::optional<StringRefNull> str = (type == PROP_ENUM && !(flag & ITEM_R_ICON_ONLY)) ?
                                                 std::nullopt :
                                                 std::make_optional<StringRefNull>("");
    but = uiDefAutoButR(
        block, ptr, prop, index, str, icon, x, y, prop_but_width, h, button_type_override);
  }

  /* Highlight in red on path template validity errors. */
  if (but != nullptr && ELEM(but->type, ButtonType::Text)) {
    /* We include PROP_NONE here because some plain string properties are used
     * as parts of paths. For example, the sub-paths in the compositor's File
     * Output node. */
    if (ELEM(subtype, PROP_FILEPATH, PROP_DIRPATH, PROP_FILENAME, PROP_NONE)) {
      if ((RNA_property_flag(prop) & PROP_PATH_SUPPORTS_TEMPLATES) != 0) {
        const std::string path = RNA_property_string_get(ptr, prop);
        if (BKE_path_contains_template_syntax(path)) {
          const std::optional<bke::path_templates::VariableMap> variables =
              BKE_build_template_variables_for_prop(
                  static_cast<const bContext *>(block->evil_C), ptr, prop);
          BLI_assert(variables.has_value());

          if (!BKE_path_validate_template(path, *variables).is_empty()) {
            button_flag_enable(but, BUT_REDALERT);
          }
        }
      }
    }
  }

  if (flag & ITEM_R_IMMEDIATE) {
    button_flag_enable(but, BUT_ACTIVATE_ON_INIT);
  }

#ifdef UI_PROP_DECORATE
  /* Only for alignment. */
  if (use_prop_decorate) { /* Note that sep flag may have been unset meanwhile. */
    (layout_prop_decorate ? layout_prop_decorate : sub)->label(nullptr, ICON_BLANK1);
  }
#endif /* UI_PROP_DECORATE */

  block_layout_set_current(block, layout);
  return but;
}

/* TODO Jean-Silas: this is defined in UI_interface_c.hh, and should be moved elsewhere */
void context_active_but_prop_get_filebrowser(const bContext *C,
                                             PointerRNA *r_ptr,
                                             PropertyRNA **r_prop,
                                             bool *r_is_undo,
                                             bool *r_is_userdef)
{
  ARegion *region = CTX_wm_region_popup(C) ? CTX_wm_region_popup(C) : CTX_wm_region(C);
  Button *prevbut = nullptr;

  *r_ptr = {};
  *r_prop = nullptr;
  *r_is_undo = false;
  *r_is_userdef = false;

  if (!region) {
    return;
  }

  for (Block &block : region->runtime->uiblocks) {
    for (Button &but : block.buttons()) {
      if (but.rnapoin.data) {
        if (RNA_property_type(but.rnaprop) == PROP_STRING) {
          prevbut = &but;
        }
      }
      /* find the button before the active one */
      if ((but.flag & BUT_LAST_ACTIVE) && prevbut) {
        *r_ptr = prevbut->rnapoin;
        *r_prop = prevbut->rnaprop;
        *r_is_undo = (prevbut->flag & BUT_UNDO) != 0;
        *r_is_userdef = button_is_userdef(prevbut);
        return;
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Button Items
 * \{ */

void but_tip_from_enum_item(Button *but, const EnumPropertyItem *item)
{
  if (but->tip == nullptr || but->tip[0] == '\0') {
    if (item->description && item->description[0] &&
        !(but->optype && but->optype->get_description))
    {
      but->tip = item->description;
    }
  }
}

void item_disabled(Layout *layout, const char *name)
{
  Block *block = layout->block();

  block_layout_set_current(block, layout);

  if (!name) {
    name = "";
  }

  const int w = text_icon_width(layout, name, 0, false);

  Button *but = uiDefBut(
      block, ButtonType::Label, name, 0, 0, w, UI_UNIT_Y, nullptr, 0.0, 0.0, "");
  button_disable(but, "");
}

Button *uiItemFullO_ptr_ex(Layout *layout,
                                  wmOperatorType *ot,
                                  std::optional<StringRef> name,
                                  int icon,
                                  const wm::OpCallContext context,
                                  const eUI_Item_Flag flag,
                                  PointerRNA *r_opptr)
{
  /* Take care to fill 'r_opptr' whatever happens. */
  Block *block = layout->block();

  std::string operator_name;
  if (!name) {
    if (ot && ot->srna && (flag & ITEM_R_ICON_ONLY) == 0) {
      operator_name = WM_operatortype_name(ot, nullptr);
      name = operator_name.c_str();
    }
    else {
      name = "";
    }
  }

  if (layout->root()->type == LayoutType::Menu && !icon) {
    icon = ICON_BLANK1;
  }

  block_layout_set_current(block, layout);
  block_new_button_group(block, ButtonGroupFlag(0));

  const int w = text_icon_width(layout, *name, icon, false);

  const EmbossType prev_emboss = layout->emboss_or_undefined();
  if (flag & ITEM_R_NO_BG) {
    layout->emboss_set(EmbossType::NoneOrStatus);
  }

  /* create the button */
  Button *but;
  if (icon) {
    if (!name->is_empty()) {
      but = uiDefIconTextButO_ptr(
          block, ButtonType::But, ot, context, icon, *name, 0, 0, w, UI_UNIT_Y, std::nullopt);
    }
    else {
      but = uiDefIconButO_ptr(
          block, ButtonType::But, ot, context, icon, 0, 0, w, UI_UNIT_Y, std::nullopt);
    }
  }
  else {
    but = uiDefButO_ptr(
        block, ButtonType::But, ot, context, *name, 0, 0, w, UI_UNIT_Y, std::nullopt);
  }

  BLI_assert(but->optype != nullptr);

  if (flag & ITEM_R_NO_BG) {
    layout->emboss_set(prev_emboss);
  }

  if (flag & ITEM_O_DEPRESS) {
    but->flag |= UI_SELECT_DRAW;
  }

  if (flag & ITEM_R_ICON_ONLY) {
    button_drawflag_disable(but, BUT_ICON_LEFT);
  }

  if (layout->red_alert()) {
    button_flag_enable(but, BUT_REDALERT);
  }

  if (layout->active_default()) {
    button_flag_enable(but, BUT_ACTIVE_DEFAULT);
  }

  /* assign properties */
  if (r_opptr) {
    PointerRNA *opptr = button_operator_ptr_ensure(but);
    opptr->data = bke::idprop::create_group("wmOperatorProperties").release();
    *r_opptr = *opptr;
  }

  return but;
}

void item_menu_hold(bContext *C, ARegion *butregion, Button *but)
{
  PopupMenu *pup = popup_menu_begin(C, "", ICON_NONE);
  Layout *layout = popup_menu_layout(pup);
  Block *block = layout->block();
  popup_menu_but_set(pup, butregion, but);

  block->flag |= BLOCK_POPUP_HOLD;

  char direction = UI_DIR_DOWN;
  if (but->drawstr.empty()) {
    switch (RGN_ALIGN_ENUM_FROM_MASK(butregion->alignment)) {
      case RGN_ALIGN_LEFT:
        direction = UI_DIR_RIGHT;
        break;
      case RGN_ALIGN_RIGHT:
        direction = UI_DIR_LEFT;
        break;
      case RGN_ALIGN_BOTTOM:
        direction = UI_DIR_UP;
        break;
      default:
        direction = UI_DIR_DOWN;
        break;
    }
  }
  block_direction_set(block, direction);

  const char *menu_id = static_cast<const char *>(but->hold_argN);
  MenuType *mt = WM_menutype_find(menu_id, true);
  if (mt) {
    layout->context_set_from_but(but);
    menutype_draw(C, mt, layout);
  }
  else {
    layout->label(RPT_("Menu Missing:"), ICON_NONE);
    layout->label(menu_id, ICON_NONE);
  }
  popup_menu_end(C, pup);
}

void item_rna_size(Layout *layout,
                          StringRef name,
                          int icon,
                          PointerRNA *ptr,
                          PropertyRNA *prop,
                          int index,
                          bool icon_only,
                          bool compact,
                          int *r_w,
                          int *r_h)
{
  int w = 0, h;

  /* arbitrary extended width by type */
  const PropertyType type = RNA_property_type(prop);
  const PropertySubType subtype = RNA_property_subtype(prop);
  const int len = RNA_property_array_length(ptr, prop);

  bool is_checkbox_only = false;
  if (name.is_empty() && !icon_only) {
    if (ELEM(type, PROP_STRING, PROP_POINTER)) {
      name = "non-empty text";
    }
    else if (type == PROP_BOOLEAN) {
      if (icon == ICON_NONE) {
        /* Exception for check-boxes, they need a little less space to align nicely. */
        is_checkbox_only = true;
      }
      icon = ICON_DOT;
    }
    else if (type == PROP_ENUM) {
      /* Find the longest enum item name, instead of using a dummy text! */
      const EnumPropertyItem *item_array;
      bool free;
      RNA_property_enum_items_gettexted(static_cast<bContext *>(layout->block()->evil_C),
                                        ptr,
                                        prop,
                                        &item_array,
                                        nullptr,
                                        &free);

      for (const EnumPropertyItem *item = item_array; item->identifier; item++) {
        if (item->identifier[0]) {
          w = max_ii(w, text_icon_width(layout, item->name, item->icon, compact));
        }
      }
      if (free) {
        MEM_delete(item_array);
      }
    }
  }

  if (!w) {
    if (type == PROP_ENUM && icon_only) {
      w = text_icon_width(layout, "", ICON_BLANK1, compact);
      if (index != RNA_ENUM_VALUE) {
        w += 0.6f * UI_UNIT_X;
      }
    }
    else {
      /* not compact for float/int buttons, looks too squashed */
      w = text_icon_width(layout, name, icon, ELEM(type, PROP_FLOAT, PROP_INT) ? false : compact);
    }
  }
  h = UI_UNIT_Y;

  /* increase height for arrays */
  if (index == RNA_NO_INDEX && len > 0) {
    if (name.is_empty() && icon == ICON_NONE) {
      h = 0;
    }
    if (layout->use_property_split()) {
      h = 0;
    }
    if (ELEM(subtype, PROP_LAYER, PROP_LAYER_MEMBER)) {
      h += 2 * UI_UNIT_Y;
    }
    else if (subtype == PROP_MATRIX) {
      int dim_size[/*RNA_MAX_ARRAY_DIMENSION*/ 3];
      RNA_property_array_dimension(ptr, prop, dim_size);
      h += dim_size[0] * UI_UNIT_Y;
    }
    else {
      h += len * UI_UNIT_Y;
    }
  }

  /* Increase width requirement if in a variable size layout. */
  if (layout_variable_size(layout)) {
    if (type == PROP_BOOLEAN && !name.is_empty()) {
      w += UI_UNIT_X / 5;
    }
    else if (is_checkbox_only) {
      w -= UI_UNIT_X / 4;
    }
    else if (type == PROP_ENUM && !icon_only) {
      w += UI_UNIT_X / 4;
    }
    else if (ELEM(type, PROP_FLOAT, PROP_INT)) {
      w += UI_UNIT_X * 3;
    }
  }

  *r_w = w;
  *r_h = h;
}

bool item_rna_is_expand(PropertyRNA *prop, int index, const eUI_Item_Flag item_flag)
{
  const bool is_array = RNA_property_array_check(prop);
  const int subtype = RNA_property_subtype(prop);
  return is_array && (index == RNA_NO_INDEX) &&
         ((item_flag & ITEM_R_EXPAND) ||
          !ELEM(subtype, PROP_COLOR, PROP_COLOR_GAMMA, PROP_DIRECTION));
}

Layout *layout_heading_find(Layout *cur_layout)
{
  for (Layout *parent = cur_layout; parent; parent = parent->parent()) {
    if (!parent->heading().is_empty()) {
      return parent;
    }
  }

  return nullptr;
}

void layout_heading_label_add(Layout *layout,
                                     Layout *heading_layout,
                                     bool right_align,
                                     bool respect_prop_split)
{
  const LayoutAlign prev_alignment = layout->alignment();

  if (right_align) {
    layout->alignment_set(LayoutAlign::Right);
  }

  if (respect_prop_split) {
    uiItemL_respect_property_split(layout, heading_layout->heading(), ICON_NONE);
  }
  else {
    layout->label(heading_layout->heading(), ICON_NONE);
  }
  /* After adding the heading label, we have to mark it somehow as added, so it's not added again
   * for other items in this layout. For now just clear it. */
  heading_layout->heading_reset();

  layout->alignment_set(prev_alignment);
}

void search_id_collection(StructRNA *ptype, PointerRNA *r_ptr, PropertyRNA **r_prop)
{
  /* look for collection property in Main */
  /* NOTE: using global Main is OK-ish here, UI shall not access other Mains anyway. */
  *r_ptr = RNA_main_pointer_create(G_MAIN);

  *r_prop = nullptr;

  RNA_STRUCT_BEGIN (r_ptr, iprop) {
    /* if it's a collection and has same pointer type, we've got it */
    if (RNA_property_type(iprop) == PROP_COLLECTION) {
      StructRNA *srna = RNA_property_pointer_type(r_ptr, iprop);

      if (ptype == srna) {
        *r_prop = iprop;
        break;
      }
    }
  }
  RNA_STRUCT_END;
}

void rna_collection_search_arg_free_fn(void *ptr)
{
  RNACollectionSearch *coll_search = static_cast<RNACollectionSearch *>(ptr);
  butstore_free(coll_search->butstore_block, coll_search->butstore);
  MEM_delete(coll_search);
}

/* TODO Jean-Silas: this is defined in interface_intern.hh, and should be moved elsewhere */
void button_configure_search(Button *but,
                             PointerRNA *ptr,
                             PropertyRNA *prop,
                             PointerRNA *searchptr,
                             PropertyRNA *searchprop,
                             PropertyRNA *item_searchprop,
                             const bool results_are_suggestions)
{
  /* for ID's we do automatic lookup */
  bool has_search_fn = false;

  PointerRNA sptr;
  if (!searchprop) {
    if (RNA_property_type(prop) == PROP_STRING) {
      has_search_fn = (RNA_property_string_search_flag(prop) != 0);
    }
    if (RNA_property_type(prop) == PROP_POINTER) {
      StructRNA *ptype = RNA_property_pointer_type(ptr, prop);
      search_id_collection(ptype, &sptr, &searchprop);
      searchptr = &sptr;
    }
  }

  /* turn button into search button */
  if (has_search_fn || searchprop) {
    RNACollectionSearch *coll_search = MEM_new<RNACollectionSearch>(__func__);

    BLI_assert(but->type == ButtonType::SearchMenu);
    ButtonSearch *search_but = static_cast<ButtonSearch *>(but);

    if (searchptr) {
      search_but->rnasearchpoin = *searchptr;
      search_but->rnasearchprop = searchprop;
    }

    but->hardmax = std::max(but->hardmax, 256.0f);
    but->drawflag |= BUT_ICON_LEFT | BUT_TEXT_LEFT;
    if (RNA_property_is_unlink(prop)) {
      but->flag |= BUT_VALUE_CLEAR;
    }

    coll_search->target_ptr = *ptr;
    coll_search->target_prop = prop;

    if (searchptr) {
      coll_search->search_ptr = *searchptr;
      coll_search->search_prop = searchprop;
      coll_search->item_search_prop = item_searchprop;
    }
    else {
      /* Rely on `has_search_fn`. */
      coll_search->search_ptr = PointerRNA_NULL;
      coll_search->search_prop = nullptr;
      coll_search->item_search_prop = nullptr;
    }

    coll_search->search_but = but;
    coll_search->butstore_block = but->block;
    coll_search->butstore = butstore_create(coll_search->butstore_block);
    butstore_register(coll_search->butstore, &coll_search->search_but);

    if (RNA_property_type(prop) == PROP_ENUM) {
      /* XXX, this will have a menu string,
       * but in this case we just want the text */
      but->str.clear();
    }

    button_func_search_set_results_are_suggestions(but, results_are_suggestions);

    button_func_search_set(but,
                           searchbox_create_generic,
                           rna_collection_search_update_fn,
                           coll_search,
                           false,
                           rna_collection_search_arg_free_fn,
                           nullptr,
                           nullptr);
    /* If this is called multiple times for the same button, an earlier call may have taken the
     * else branch below so the button was disabled. Now we have a searchprop, so it can be enabled
     * again. */
    but->flag &= ~BUT_DISABLED;
  }
  else if (but->type == ButtonType::SearchMenu) {
    /* In case we fail to find proper searchprop,
     * so other code might have already set but->type to search menu... */
    but->flag |= BUT_DISABLED;
  }
}

/* TODO Jean-Silas: this is defined in interface_intern.hh, and should be moved elsewhere */
void item_menutype_func(bContext *C, Layout *layout, void *arg_mt)
{
  MenuType *mt = static_cast<MenuType *>(arg_mt);
  menutype_draw(C, mt, layout);
}

/* TODO Jean-Silas: this is defined in interface_intern.hh, and should be moved elsewhere */
void item_paneltype_func(bContext *C, Layout *layout, void *arg_pt)
{
  PanelType *pt = static_cast<PanelType *>(arg_pt);
  UI_paneltype_draw(C, pt, layout);
}

Button *item_menu(Layout *layout,
                         const StringRef name,
                         int icon,
                         MenuCreateFunc func,
                         void *arg,
                         void *argN,
                         const std::optional<StringRef> tip,
                         bool force_menu,
                         ButtonArgNFree func_argN_free_fn,
                         ButtonArgNCopy func_argN_copy_fn)
{
  Block *block = layout->block();
  Layout *heading_layout = layout_heading_find(layout);

  block_layout_set_current(block, layout);
  block_new_button_group(block, ButtonGroupFlag(0));

  if (layout->root()->type == LayoutType::Menu && !icon) {
    icon = ICON_BLANK1;
  }

  TextIconPadFactor pad_factor = text_pad_compact;
  if (layout->root()->type == LayoutType::Header) { /* Ugly! */
    if (icon == ICON_NONE && force_menu) {
      /* pass */
    }
    else if (force_menu) {
      pad_factor.text = 1.85;
      pad_factor.icon_only = 0.6f;
    }
    else {
      pad_factor.text = 0.75f;
    }
  }

  const int w = text_icon_width_ex(layout, name, icon, pad_factor, UI_FSTYLE_WIDGET);
  const int h = UI_UNIT_Y;

  if (heading_layout) {
    layout_heading_label_add(layout, heading_layout, true, true);
  }

  Button *but;
  if (!name.is_empty() && icon) {
    but = uiDefIconTextMenuBut(block, func, arg, icon, name, 0, 0, w, h, tip);
  }
  else if (icon) {
    but = uiDefIconMenuBut(block, func, arg, icon, 0, 0, w, h, tip);
    if (force_menu && !name.is_empty()) {
      button_drawflag_enable(but, BUT_ICON_LEFT);
    }
  }
  else {
    but = uiDefMenuBut(block, func, arg, name, 0, 0, w, h, tip);
  }

  if (argN) {
    /* ugly! */
    if (arg != argN) {
      but->poin = reinterpret_cast<char *>(but);
    }
    but->func_argN = argN;
    but->func_argN_free_fn = func_argN_free_fn;
    but->func_argN_copy_fn = func_argN_copy_fn;
  }

  if (ELEM(layout->root()->type, LayoutType::Panel, LayoutType::Toolbar) ||
      /* We never want a drop-down in menu! */
      (force_menu && layout->root()->type != LayoutType::Menu))
  {
    button_type_set_menu_from_pulldown(but);
  }

  return but;
}

Button *uiItem_simple(Layout *layout,
                             const StringRef name,
                             int icon,
                             std::optional<StringRef> tooltip,
                             const ButtonType but_type)
{
  Block *block = layout->block();

  block_layout_set_current(block, layout);
  block_new_button_group(block, ButtonGroupFlag(0));

  if (layout->root()->type == LayoutType::Menu && !icon) {
    icon = ICON_BLANK1;
  }

  const int w = text_icon_width_ex(layout, name, icon, text_pad_none, UI_FSTYLE_WIDGET);
  Button *but;
  if (icon && !name.is_empty()) {
    but = uiDefIconTextBut(block, but_type, icon, name, 0, 0, w, UI_UNIT_Y, nullptr, tooltip);
  }
  else if (icon) {
    but = uiDefIconBut(block, but_type, icon, 0, 0, w, UI_UNIT_Y, nullptr, 0.0, 0.0, tooltip);
  }
  else {
    but = uiDefBut(block, but_type, name, 0, 0, w, UI_UNIT_Y, nullptr, 0.0, 0.0, tooltip);
  }

  /* to compensate for string size padding in text_icon_width,
   * make text aligned right if the layout is aligned right.
   */
  if (layout->alignment() == LayoutAlign::Right) {
    but->drawflag &= ~BUT_TEXT_LEFT; /* default, needs to be unset */
    but->drawflag |= BUT_TEXT_RIGHT;
  }

  /* Mark as a label inside a list-box. */
  if (block->flag & BLOCK_LIST_ITEM) {
    but->flag |= BUT_LIST_ITEM;
  }

  if (layout->red_alert()) {
    button_flag_enable(but, BUT_REDALERT);
  }

  return but;
}

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
Button *uiItemL_ex(
    Layout *layout, const StringRef name, int icon, const bool highlight, const bool redalert)
{
  Button *but = uiItem_simple(layout, name, icon);

  if (highlight) {
    /* TODO: add another flag for this. */
    button_flag_enable(but, UI_SELECT_DRAW);
  }

  if (redalert) {
    button_flag_enable(but, BUT_REDALERT);
  }

  return but;
}

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
Layout *uiItemL_respect_property_split(Layout *layout, StringRef text, int icon)
{
  if (layout->use_property_split()) {
    Block *block = layout->block();
    const PropertySplitWrapper split_wrapper = uiItemPropertySplitWrapperCreate(layout);
    /* Further items added to 'layout' will automatically be added to split_wrapper.property_row */

    uiItem_simple(split_wrapper.label_column, text, icon);
    block_layout_set_current(block, split_wrapper.property_row);

    return split_wrapper.decorate_column;
  }

  char namestr[UI_MAX_NAME_STR];
  text = item_name_add_colon(text, namestr);
  uiItem_simple(layout, text, icon);

  return nullptr;
}

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
PropertySplitWrapper uiItemPropertySplitWrapperCreate(Layout *parent_layout)
{
  PropertySplitWrapper split_wrapper = {nullptr};

  Layout *layout_row = &parent_layout->row(true);
  Layout *layout_split = &layout_row->split(UI_ITEM_PROP_SEP_DIVIDE, true);

  split_wrapper.label_column = &layout_split->column(true);
  split_wrapper.label_column->alignment_set(LayoutAlign::Right);
  split_wrapper.property_row = LayoutInternal::item_prop_split_layout_hack(parent_layout,
                                                                           layout_split);
  split_wrapper.decorate_column = parent_layout->use_property_decorate() ?
                                      &layout_row->column(true) :
                                      nullptr;

  return split_wrapper;
}

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
void uiItemLDrag(Layout *layout, PointerRNA *ptr, StringRef name, int icon)
{
  Button *but = uiItem_simple(layout, name, icon);

  if (ptr && ptr->type) {
    if (RNA_struct_is_ID(ptr->type)) {
      button_drag_set_id(but, ptr->owner_id);
    }
  }
}

int menu_item_enum_opname_menu_active(bContext *C, Button *but, MenuItemLevel *lvl)
{
  wmOperatorType *ot = WM_operatortype_find(lvl->opname, true);

  if (!ot) {
    return -1;
  }

  const EnumPropertyItem *item_array = nullptr;
  bool free;
  int totitem;
  PointerRNA ptr = WM_operator_properties_create_ptr(ot);
  /* so the context is passed to itemf functions (some need it) */
  WM_operator_properties_sanitize(&ptr, false);
  PropertyRNA *prop = RNA_struct_find_property(&ptr, lvl->propname);
  if (!prop) {
    return -1;
  }
  RNA_property_enum_items_gettexted(C, &ptr, prop, &item_array, &totitem, &free);
  int active = RNA_enum_from_name(item_array, but->str.c_str());
  if (free) {
    MEM_delete(item_array);
  }

  return active;
}

void menu_item_enum_opname_menu(bContext *C, Layout *layout, void *arg)
{
  Button *but = static_cast<Button *>(arg);
  MenuItemLevel *lvl = static_cast<MenuItemLevel *>(but->func_argN);
  /* Use the operator properties from the button owning the menu. */
  BLI_assert(but->opptr);
  IDProperty *op_props = but->opptr->data_as<IDProperty>();

  /* The calling but's str _probably_ contains the active
   * menu item name, set in #Layout::op_menu_enum. */
  const int active = menu_item_enum_opname_menu_active(C, but, lvl);

  layout->operator_context_set(lvl->opcontext);
  layout->op_enum(lvl->opname, lvl->propname, op_props, lvl->opcontext, UI_ITEM_NONE, active);

  /* override default, needed since this was assumed pre 2.70 */
  block_direction_set(layout->block(), UI_DIR_DOWN);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layout Items
 * \{ */

int litem_min_width(int itemw)
{
  return std::min(2 * UI_UNIT_X, itemw);
}

int spaces_after_column_item(const Layout *litem,
                                    const Item *item,
                                    const Item *next_item,
                                    const bool is_box)
{
  if (next_item == nullptr) {
    return item->type() == ItemType::LayoutPanelHeader ? 1 : 0;
  }
  if (item->type() == ItemType::LayoutPanelHeader &&
      next_item->type() == ItemType::LayoutPanelHeader)
  {
    /* No extra space between layout panel headers. */
    return 0;
  }
  if (item->type() == ItemType::LayoutPanelHeader &&
      next_item->type() == ItemType::LayoutPanelBody)
  {
    /* One for the end of the panel header and one for the start of panel body. */
    return 2;
  }
  if (item->type() == ItemType::LayoutPanelBody &&
      next_item->type() == ItemType::LayoutPanelHeader)
  {
    /* One for the end of the panel body and one for the start of panel header. */
    return 2;
  }
  if (!is_box) {
    return 1;
  }
  if (item != litem->items().first()) {
    return 1;
  }
  return 0;
}

RadialDirection get_radialbut_vec(float vec[2], short itemnum)
{
  if (itemnum >= PIE_MAX_ITEMS) {
    itemnum %= PIE_MAX_ITEMS;
    printf("Warning: Pie menus with more than %i items are currently unsupported\n",
           PIE_MAX_ITEMS);
  }

  const RadialDirection dir = RadialDirection(radial_dir_order[itemnum]);
  button_pie_dir(dir, vec);

  return dir;
}

bool item_is_radial_displayable(Item *item)
{

  if ((item->type() == ItemType::Button) &&
      ((static_cast<ButtonItem *>(item))->but->type == ButtonType::Label))
  {
    return false;
  }

  return true;
}

bool item_is_radial_drawable(ButtonItem *bitem)
{

  if (ELEM(bitem->but->type, ButtonType::Sepr, ButtonType::SeprLine, ButtonType::SeprSpacer)) {
    return false;
  }

  return true;
}

void litem_grid_flow_compute(Span<Item *> items,
                                    const UILayoutGridFlowInput *parameters,
                                    UILayoutGridFlowOutput *results)
{
  float tot_w = 0.0f, tot_h = 0.0f;
  float global_avg_w = 0.0f, global_totweight_w = 0.0f;
  int global_max_h = 0;

  BLI_assert(parameters->tot_columns != 0 ||
             (results->cos_x_array == nullptr && results->widths_array == nullptr &&
              results->tot_w == nullptr));
  BLI_assert(parameters->tot_rows != 0 ||
             (results->cos_y_array == nullptr && results->heights_array == nullptr &&
              results->tot_h == nullptr));

  if (results->tot_items) {
    *results->tot_items = 0;
  }

  if (items.is_empty()) {
    if (results->global_avg_w) {
      *results->global_avg_w = 0.0f;
    }
    if (results->global_max_h) {
      *results->global_max_h = 0;
    }
    return;
  }

  Array<float, 64> avg_w(parameters->tot_columns, 0.0f);
  Array<float, 64> totweight_w(parameters->tot_columns, 0.0f);
  Array<int, 64> max_h(parameters->tot_rows, 0);

  int i = 0;
  for (const Item *item : items) {
    const int2 size = item->size();

    global_avg_w += float(size.x * size.x);
    global_totweight_w += float(size.x);
    global_max_h = max_ii(global_max_h, size.y);

    if (parameters->tot_rows != 0 && parameters->tot_columns != 0) {
      const int index_col = parameters->row_major ? i % parameters->tot_columns :
                                                    i / parameters->tot_rows;
      const int index_row = parameters->row_major ? i / parameters->tot_columns :
                                                    i % parameters->tot_rows;

      avg_w[index_col] += float(size.x * size.x);
      totweight_w[index_col] += float(size.x);

      max_h[index_row] = max_ii(max_h[index_row], size.y);
    }

    if (results->tot_items) {
      (*results->tot_items)++;
    }
    i++;
  }

  /* Finalize computing of column average sizes */
  global_avg_w /= global_totweight_w;
  if (parameters->tot_columns != 0) {
    for (i = 0; i < parameters->tot_columns; i++) {
      avg_w[i] /= totweight_w[i];
      tot_w += avg_w[i];
    }
    if (parameters->even_columns) {
      tot_w = ceilf(global_avg_w) * parameters->tot_columns;
    }
  }
  /* Finalize computing of rows max sizes */
  if (parameters->tot_rows != 0) {
    for (i = 0; i < parameters->tot_rows; i++) {
      tot_h += max_h[i];
    }
    if (parameters->even_rows) {
      tot_h = global_max_h * parameters->tot_columns;
    }
  }

  /* Compute positions and sizes of all cells. */
  if (results->cos_x_array != nullptr && results->widths_array != nullptr) {
    /* We enlarge/narrow columns evenly to match available width. */
    const float wfac = float(parameters->litem_w -
                             (parameters->tot_columns - 1) * parameters->space_x) /
                       tot_w;

    for (int col = 0; col < parameters->tot_columns; col++) {
      results->cos_x_array[col] = (col ? results->cos_x_array[col - 1] +
                                             results->widths_array[col - 1] + parameters->space_x :
                                         parameters->litem_x);
      if (parameters->even_columns) {
        /* (< remaining width > - < space between remaining columns >) / < remaining columns > */
        results->widths_array[col] = (((parameters->litem_w -
                                        (results->cos_x_array[col] - parameters->litem_x)) -
                                       (parameters->tot_columns - col - 1) * parameters->space_x) /
                                      (parameters->tot_columns - col));
      }
      else if (col == parameters->tot_columns - 1) {
        /* Last column copes width rounding errors... */
        results->widths_array[col] = parameters->litem_w -
                                     (results->cos_x_array[col] - parameters->litem_x);
      }
      else {
        results->widths_array[col] = int(avg_w[col] * wfac);
      }
    }
  }
  if (results->cos_y_array != nullptr && results->heights_array != nullptr) {
    for (int row = 0; row < parameters->tot_rows; row++) {
      if (parameters->even_rows) {
        results->heights_array[row] = global_max_h;
      }
      else {
        results->heights_array[row] = max_h[row];
      }
      results->cos_y_array[row] = (row ? results->cos_y_array[row - 1] - parameters->space_y -
                                             results->heights_array[row] :
                                         parameters->litem_y - results->heights_array[row]);
    }
  }

  if (results->global_avg_w) {
    *results->global_avg_w = global_avg_w;
  }
  if (results->global_max_h) {
    *results->global_max_h = global_max_h;
  }
  if (results->tot_w) {
    *results->tot_w = int(tot_w) + parameters->space_x * (parameters->tot_columns - 1);
  }
  if (results->tot_h) {
    *results->tot_h = tot_h + parameters->space_y * (parameters->tot_rows - 1);
  }
}

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
bool uiLayoutEndsWithPanelHeader(const Layout &layout)
{
  if (layout.items().is_empty()) {
    return false;
  }
  const Item *item = layout.items().last();
  return item->type() == ItemType::LayoutPanelHeader;
}

LayoutItemBx *layout_box(Layout *layout, ButtonType type)
{
  LayoutItemBx *box = MEM_new<LayoutItemBx>(__func__);
  LayoutInternal::init_from_parent(box, layout, false);

  LayoutInternal::layout_space_set(box, layout->root()->style->columnspace);

  block_layout_set_current(layout->block(), box);

  box->roundbox = uiDefBut(layout->block(), type, "", 0, 0, 0, 0, nullptr, 0.0, 0.0, "");

  return box;
}

/* TODO Jean-Silas: this is defined in interface_intern.hh, and should be moved elsewhere */
void layout_list_set_labels_active(Layout *layout)
{
  for (Item *item : layout->items()) {
    if (item->type() != ItemType::Button) {
      layout_list_set_labels_active(static_cast<Layout *>(item));
    }
    else {
      ButtonItem *bitem = static_cast<ButtonItem *>(item);
      if (bitem->but->flag & BUT_LIST_ITEM) {
        button_flag_enable(bitem->but, UI_SELECT);
      }
    }
  }
}

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
int uiLayoutListItemPaddingWidth()
{
  return 5 * UI_SCALE_FAC;
}

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
void uiLayoutListItemAddPadding(Layout *layout)
{
  Block *block = layout->block();
  Layout *row = &layout->row(true);
  row->fixed_size_set(true);

  uiDefBut(
      block, ButtonType::Sepr, "", 0, 0, uiLayoutListItemPaddingWidth(), 0, nullptr, 0.0, 0.0, "");

  /* Restore. */
  block_layout_set_current(block, layout);
}



/** \} */

/* -------------------------------------------------------------------- */
/** \name Block Layout Search Filtering
 * \{ */

bool block_search_panel_label_matches(const Block *block, const char *search_string)
{
  if ((block->panel != nullptr) && (block->panel->type != nullptr)) {
    if (BLI_strcasestr(block->panel->type->label, search_string)) {
      return true;
    }
  }
  return false;
}

bool button_matches_search_filter(Button *but, const char *search_filter)
{
  /* Do the shorter checks first for better performance in case there is a match. */
  if (BLI_strcasestr(but->str.c_str(), search_filter)) {
    return true;
  }

  if (but->optype != nullptr) {
    if (BLI_strcasestr(but->optype->name, search_filter)) {
      return true;
    }
  }

  if (but->rnaprop != nullptr) {
    if (BLI_strcasestr(RNA_property_ui_name(but->rnaprop), search_filter)) {
      return true;
    }
#ifdef PROPERTY_SEARCH_USE_TOOLTIPS
    if (BLI_strcasestr(RNA_property_description(but->rnaprop), search_filter)) {
      return true;
    }
#endif

    /* Search through labels of enum property items if they are in a drop-down menu.
     * Unfortunately we have no #bContext here so we cannot search through RNA enums
     * with dynamic entries (or "itemf" functions) which require context. */
    if (but->type == ButtonType::Menu) {
      PointerRNA *ptr = &but->rnapoin;
      PropertyRNA *enum_prop = but->rnaprop;
      int items_len;
      const EnumPropertyItem *items_array = nullptr;
      bool free;
      RNA_property_enum_items_gettexted(nullptr, ptr, enum_prop, &items_array, &items_len, &free);
      if (items_array == nullptr) {
        return false;
      }

      bool found = false;
      for (int i = 0; i < items_len; i++) {
        /* Check for nullptr name field which enums use for separators. */
        if (items_array[i].name == nullptr) {
          continue;
        }
        if (BLI_strcasestr(items_array[i].name, search_filter)) {
          found = true;
          break;
        }
      }
      if (free) {
        MEM_delete(const_cast<EnumPropertyItem *>(items_array));
      }
      if (found) {
        return true;
      }
    }
  }

  return false;
}

bool button_group_has_search_match(const ButtonGroup &group, const char *search_filter)
{
  for (Button *but : group.buttons) {
    if (button_matches_search_filter(but, search_filter)) {
      return true;
    }
  }

  return false;
}

bool block_search_filter_tag_buttons(Block *block, const char *search_filter)
{
  bool has_result = false;
  for (const ButtonGroup &group : block->button_groups) {
    if (button_group_has_search_match(group, search_filter)) {
      has_result = true;
    }
    else {
      for (Button *but : group.buttons) {
        but->flag |= UI_SEARCH_FILTER_NO_MATCH;
      }
    }
  }
  return has_result;
}

/* TODO Jean-Silas: this is declared in UI_interface_c.hh, and should be moved elsewhere */
bool block_apply_search_filter(Block *block, const char *search_filter)
{
  if (search_filter == nullptr || search_filter[0] == '\0') {
    return false;
  }

  Panel *panel = block->panel;

  if (panel != nullptr) {
    /* Panels for active blocks should always have a valid `panel->type`,
     * otherwise they wouldn't be created. */
    if (panel->type->flag & PANEL_TYPE_NO_SEARCH) {
      return false;
    }
  }

  const bool panel_label_matches = block_search_panel_label_matches(block, search_filter);

  const bool has_result = (panel_label_matches) ?
                              true :
                              block_search_filter_tag_buttons(block, search_filter);

  if (panel != nullptr) {
    if (has_result) {
      panel_tag_search_filter_match(block->panel);
    }
  }

  return has_result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layout
 * \{ */

void item_scale(Layout *litem, const float scale[2])
{
  for (Item *item : litem->items()) {
    if (item->type() != ItemType::Button) {
      Layout *subitem = static_cast<Layout *>(item);
      item_scale(subitem, scale);
    }

    int2 size = item->size();
    int2 offset = item->offset();

    if (scale[0] != 0.0f) {
      offset.x *= scale[0];
      size.x *= scale[0];
    }

    if (scale[1] != 0.0f) {
      offset.y *= scale[1];
      size.y *= scale[1];
    }

    item_position(item, offset.x, offset.y, size.x, size.y);
  }
}

void item_align(Layout *litem, short nr)
{
  for (Item *item : litem->items()) {
    if (item->type() == ItemType::Button) {
      ButtonItem *bitem = static_cast<ButtonItem *>(item);
      if (!bitem->but->alignnr) {
        bitem->but->alignnr = nr;
      }
    }
    else if (item->type() == ItemType::LayoutAbsolute) {
      /* pass */
    }
    else if (item->type() == ItemType::LayoutOverlap) {
      /* pass */
    }
    else if (item->type() == ItemType::LayoutBox) {
      LayoutItemBx *box = static_cast<LayoutItemBx *>(item);
      if (!box->roundbox->alignnr) {
        box->roundbox->alignnr = nr;
      }
    }
    else {
      Layout *litem = static_cast<Layout *>(item);
      if (litem->align()) {
        item_align(litem, nr);
      }
    }
  }
}

void item_flag(Layout *litem, int flag)
{
  for (Item *item : litem->items()) {
    if (item->type() == ItemType::Button) {
      ButtonItem *bitem = static_cast<ButtonItem *>(item);
      bitem->but->flag |= flag;
    }
    else {
      item_flag(static_cast<Layout *>(item), flag);
    }
  }
}

int2 layout_end(Layout *layout)
{
  LayoutInternal::layout_estimate(layout);
  LayoutInternal::layout_resolve(layout);
  return layout->offset();
}

void layout_free(Layout *layout)
{
  for (Item *item : layout->items()) {
    if (item->type() == ItemType::Button) {
      ButtonItem *bitem = static_cast<ButtonItem *>(item);

      bitem->but->layout = nullptr;
      MEM_delete(item);
    }
    else {
      Layout *litem = static_cast<Layout *>(item);
      layout_free(litem);
    }
  }

  MEM_delete(layout);
}

void layout_add_padding_button(LayoutRoot *root)
{
  if (root->padding) {
    /* add an invisible button for padding */
    Block *block = root->block;
    Layout *prev_layout = block->curlayout;

    block->curlayout = root->layout;
    uiDefBut(
        block, ButtonType::Sepr, "", 0, 0, root->padding, root->padding, nullptr, 0.0, 0.0, "");
    block->curlayout = prev_layout;
  }
}

/* TODO Jean-Silas: Block-related functions belong in their own file somewhere */
Layout &block_layout(Block *block,
                     LayoutDirection dir,
                     LayoutType type,
                     int x,
                     int y,
                     int size,
                     int em,
                     int padding,
                     const uiStyle *style)
{
  LayoutRoot *root = MEM_new_zeroed<LayoutRoot>(__func__);
  root->type = type;
  root->style = style;
  root->block = block;
  root->padding = padding;
  root->opcontext = wm::OpCallContext::InvokeRegionWin;
  const char *func = __func__;
  Layout *layout = [&]() -> Layout * {
    switch (type) {
      case LayoutType::VerticalBar:
        return MEM_new<LayoutColumn>(func, root);
      case LayoutType::PieMenu:
        BLI_assert(block->pie_data);
        return MEM_new<LayoutRootPieMenu>(func, root);
      case LayoutType::Header:
        return MEM_new<LayoutRow>(func, ItemType::LayoutRoot, root);
      default:
        return MEM_new<LayoutColumn>(func, ItemType::LayoutRoot, root);
    }
  }();

  /* Only used when 'ItemInternalFlag::PropSep' is set. */
  layout->use_property_decorate_set(true);

  LayoutInternal::layout_space_set(layout, style->templatespace);
  layout->active_set(true);
  layout->enabled_set(true);
  layout->emboss_set(EmbossType::Undefined);
  int w = 0, h = 0;
  if (ELEM(type, LayoutType::Menu, LayoutType::PieMenu)) {
    LayoutInternal::layout_space_set(layout, 0);
  }

  if (dir == LayoutDirection::Horizontal) {
    h = size;
    layout->root()->emh = em * UI_UNIT_Y;
  }
  else {
    w = size;
    layout->root()->emw = em * UI_UNIT_X;
  }
  LayoutInternal::layout_offset_size_set(layout, x, y, w, h);

  block->curlayout = layout;
  root->layout = layout;
  BLI_addtail(&block->layouts, root);

  layout_add_padding_button(root);

  return *layout;
}

/* TODO Jean-Silas: this is defined in interface_intern.hh, and should be moved elsewhere */
void layout_add_but(Layout *layout, Button *but)
{
  LayoutInternal::layout_add_but(layout, but);
};

/* TODO Jean-Silas: this is defined in interface_intern.hh, and should be moved elsewhere */
void layout_remove_but(Layout *layout, const Button *but)
{
  LayoutInternal::layout_remove_but(layout, but);
}

/* TODO Jean-Silas: this is defined in interface_intern.hh, and should be moved elsewhere */
bool layout_replace_but_ptr(Layout *layout, const void *old_but_ptr, Button *new_but)
{
  ButtonItem *bitem = LayoutInternal::layout_find_button_item(
      layout, static_cast<const Button *>(old_but_ptr));
  if (!bitem) {
    return false;
  }

  bitem->but = new_but;
  return true;
}

/* TODO Jean-Silas: Block-related functions belong in their own file somewhere */
void block_layout_set_current(Block *block, Layout *layout)
{
  block->curlayout = layout;
}

/* TODO Jean-Silas: Block-related functions belong in their own file somewhere */
void block_layout_free(Block *block)
{
  for (LayoutRoot &root : block->layouts.items_mutable()) {
    layout_free(root.layout);
    MEM_delete(&root);
  }
}

/* TODO Jean-Silas: Block-related functions belong in their own file somewhere */
int2 block_layout_resolve(Block *block)
{
  BLI_assert(block->active);
  int2 block_size = {0, 0};

  block->curlayout = nullptr;

  for (LayoutRoot &root : block->layouts.items_mutable()) {
    layout_add_padding_button(&root);

    /* nullptr in advance so we don't interfere when adding button */
    block_size = layout_end(root.layout);
    layout_free(root.layout);
    MEM_delete(&root);
  }

  BLI_listbase_clear(&block->layouts);
  return block_size;
}

/* TODO Jean-Silas: Block-related functions belong in their own file somewhere */
bool block_layout_needs_resolving(const Block *block)
{
  return !BLI_listbase_is_empty(&block->layouts);
}

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
void uiLayoutSetTooltipFunc(
    Layout *layout, ButtonToolTipFunc func, void *arg, CopyArgFunc copy_arg, FreeArgFunc free_arg)
{
  bool arg_used = false;

  for (Item *item : layout->items()) {
    /* Each button will call free_arg for "its" argument, so we need to
     * duplicate the allocation for each button after the first. */
    if (copy_arg != nullptr && arg_used) {
      arg = copy_arg(arg);
    }

    if (item->type() == ItemType::Button) {
      ButtonItem *bitem = static_cast<ButtonItem *>(item);
      if (bitem->but->type == ButtonType::Decorator) {
        continue;
      }
      button_func_tooltip_set(bitem->but, func, arg, free_arg);
      arg_used = true;
    }
    else {
      uiLayoutSetTooltipFunc(static_cast<Layout *>(item), func, arg, copy_arg, free_arg);
      arg_used = true;
    }
  }

  if (free_arg != nullptr && !arg_used) {
    /* Free the original copy of arg in case the layout is empty. */
    free_arg(arg);
  }
}

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
void uiLayoutSetTooltipCustomFunc(Layout *layout,
                                  ButtonToolTipCustomFunc func,
                                  void *arg,
                                  CopyArgFunc copy_arg,
                                  FreeArgFunc free_arg)
{
  bool arg_used = false;

  for (Item *item : layout->items()) {
    /* Each button will call free_arg for "its" argument, so we need to
     * duplicate the allocation for each button after the first. */
    if (copy_arg != nullptr && arg_used) {
      arg = copy_arg(arg);
    }

    if (item->type() == ItemType::Button) {
      ButtonItem *bitem = static_cast<ButtonItem *>(item);
      if (bitem->but->type == ButtonType::Decorator) {
        continue;
      }
      button_func_tooltip_custom_set(bitem->but, func, arg, free_arg);
    }
    else {
      uiLayoutSetTooltipCustomFunc(static_cast<Layout *>(item), func, arg, copy_arg, free_arg);
    }
    arg_used = true;
  }

  if (free_arg != nullptr && !arg_used) {
    /* Free the original copy of arg in case the layout is empty. */
    free_arg(arg);
  }
}

/* TODO Jean-Silas: this is defined in UI_interface_c.hh, and should be moved elsewhere */
wmOperatorType *button_operatortype_get_from_enum_menu(Button *but, PropertyRNA **r_prop)
{
  if (r_prop != nullptr) {
    *r_prop = nullptr;
  }

  if (but->menu_create_func == menu_item_enum_opname_menu) {
    MenuItemLevel *lvl = static_cast<MenuItemLevel *>(but->func_argN);
    wmOperatorType *ot = WM_operatortype_find(lvl->opname, false);
    if ((ot != nullptr) && (r_prop != nullptr)) {
      *r_prop = RNA_struct_type_find_property(ot->srna, lvl->propname);
    }
    return ot;
  }
  return nullptr;
}

/* TODO Jean-Silas: this is defined in UI_interface_c.hh, and should be moved elsewhere */
MenuType *button_menutype_get(const Button *but)
{
  if (but->menu_create_func == item_menutype_func) {
    return reinterpret_cast<MenuType *>(but->poin);
  }
  return nullptr;
}

/* TODO Jean-Silas: this is defined in UI_interface_c.hh, and should be moved elsewhere */
PanelType *button_paneltype_get(const Button *but)
{
  if (but->menu_create_func == item_paneltype_func) {
    return reinterpret_cast<PanelType *>(but->poin);
  }
  return nullptr;
}

/* TODO Jean-Silas: this is defined in UI_interface_c.hh, and should be moved elsewhere */
std::optional<StringRefNull> button_asset_shelf_type_idname_get(const Button *but)
{
  return asset_shelf_idname_from_button_context(but);
}

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
void menutype_draw(bContext *C, MenuType *mt, Layout *layout)
{
  Menu menu{};
  menu.layout = layout;
  menu.type = mt;

  if (G.debug & G_DEBUG_WM) {
    printf("%s: opening menu \"%s\"\n", __func__, mt->idname);
  }

  Block *block = layout->block();
  if (flag_is_set(mt->flag, MenuTypeFlag::SearchOnKeyPress)) {
    block_flag_enable(block, BLOCK_NO_ACCELERATOR_KEYS);
  }
  if (mt->listener) {
    /* Forward the menu type listener to the block we're drawing in. */
    block_add_dynamic_listener(block, mt->listener);
  }

  bContextStore context_store;
  if (layout->context()) {
    context_store = *layout->context();
  }
  const bContextStore *previous_context_store = CTX_store_get(C);
  if (previous_context_store) {
    context_store.entries.extend(previous_context_store->entries);
  }
  CTX_store_set(C, &context_store);

  mt->draw(C, &menu);

  CTX_store_set(C, previous_context_store);
}


bool layout_has_panel_label(const Layout *layout, const PanelType *pt)
{
  for (Item *subitem : layout->items()) {
    if (subitem->type() == ItemType::Button) {
      ButtonItem *bitem = static_cast<ButtonItem *>(subitem);
      if (!(bitem->but->flag & UI_HIDDEN) &&
          bitem->but->str == CTX_IFACE_(pt->translation_context, pt->label))
      {
        return true;
      }
    }
    else {
      Layout *litem = static_cast<Layout *>(subitem);
      if (layout_has_panel_label(litem, pt)) {
        return true;
      }
    }
  }

  return false;
}

void paneltype_draw_impl(bContext *C, PanelType *pt, Layout *layout, bool show_header)
{
  Block *block = layout->block();
  Panel *panel = BKE_panel_new(pt);
  panel->flag = PNL_POPOVER;

  if (pt->listener) {
    block_add_dynamic_listener(block, pt->listener);
  }

  /* This check may be paranoid, this function might run outside the context of a popup or can run
   * in popovers that are not supposed to support refreshing, see #popover_create_block. */
  const bool support_layout_panel = block->handle && block->handle->region;
  if (support_layout_panel) {
    /* Allow popovers to contain collapsible sections, see #Layout::popover. */
    popup_dummy_panel_set(block->handle->region, block, pt->idname);
  }

  Layout *body = nullptr;
  /* Draw main panel. */
  if (show_header) {
    Layout *header = nullptr;
    if (support_layout_panel && !(pt->flag & PANEL_TYPE_NO_HEADER)) {
      PanelLayout panel_layout = layout->panel(
          C, panel->type->idname, panel->type->flag & PANEL_TYPE_DEFAULT_CLOSED);
      header = panel_layout.header;
      body = panel_layout.body;
    }
    else {
      header = &layout->row(false);
      body = &layout->column(false);
    }
    if (pt->draw_header) {
      panel->layout = header;
      pt->draw_header(C, panel);
      panel->layout = nullptr;
    }

    /* draw_header() is often used to add a checkbox to the header. If we add the label like below
     * the label is disconnected from the checkbox, adding a weird looking gap. As workaround, let
     * the checkbox add the label instead. */
    if (!layout_has_panel_label(header, pt)) {
      header->label(CTX_IFACE_(pt->translation_context, pt->label), ICON_NONE);
    }
  }
  else {
    body = layout;
  }

  if (body) {
    panel->layout = body;
    pt->draw(C, panel);
    panel->layout = nullptr;
  }
  BLI_assert(panel->runtime->custom_data_ptr == nullptr);

  BKE_panel_free(panel);
  if (!body) {
    return;
  }
  /* Draw child panels. */
  for (LinkData &link : pt->children) {
    PanelType *child_pt = static_cast<PanelType *>(link.data);
    if (child_pt->poll == nullptr || child_pt->poll(C, child_pt)) {
      paneltype_draw_impl(C, child_pt, body, true);
    }
  }
}

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
void UI_paneltype_draw(bContext *C, PanelType *pt, Layout *layout)
{
  if (layout->context()) {
    CTX_store_set(C, layout->context());
  }

  paneltype_draw_impl(C, pt, layout, false);

  if (layout->context()) {
    CTX_store_set(C, nullptr);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layout (Debugging/Introspection)
 *
 * Serialize the layout as a Python compatible dictionary,
 *
 * \note Proper string escaping isn't used,
 * triple quotes are used to prevent single quotes from interfering with Python syntax.
 * If we want this to be fool-proof, we would need full Python compatible string escape support.
 * As we don't use triple quotes in the UI it's good-enough in practice.
 * \{ */

void layout_introspect_button(fmt::appender ds, const ButtonItem *bitem)
{
  Button *but = bitem->but;
  fmt::format_to(ds, "'type':{}, ", int(but->type));
  fmt::format_to(ds, "'draw_string':'''{}''', ", but->drawstr);
  /* Not exactly needed, rna has this. */
  fmt::format_to(ds, "'tip':'''{}''', ", but->tip);

  if (but->optype) {
    std::string opstr = WM_operator_pystring_ex(static_cast<bContext *>(but->block->evil_C),
                                                nullptr,
                                                false,
                                                true,
                                                but->optype,
                                                but->opptr);
    fmt::format_to(ds, "'operator':'''{}''', ", opstr);
  }

  {
    PropertyRNA *prop = nullptr;
    wmOperatorType *ot = button_operatortype_get_from_enum_menu(but, &prop);
    if (ot) {
      std::string opstr = WM_operator_pystring_ex(
          static_cast<bContext *>(but->block->evil_C), nullptr, false, true, ot, nullptr);
      fmt::format_to(ds, "'operator':'''{}''', ", opstr);
      fmt::format_to(ds, "'property':'''{}''', ", prop ? RNA_property_identifier(prop) : "");
    }
  }

  if (but->rnaprop) {
    fmt::format_to(ds,
                   "'rna':'{}.{}[{}]', ",
                   RNA_struct_identifier(but->rnapoin.type),
                   RNA_property_identifier(but->rnaprop),
                   but->rnaindex);
  }
}

void layout_introspect_items(fmt::appender ds, Span<const Item *> items)
{
  fmt::format_to(ds, "[");

  for (const Item *item : items) {

    fmt::format_to(ds, "{{");

#define CASE_ITEM(type, name) \
case type: { \
fmt::format_to(ds, "'type': '{}', ", name); \
break; \
} \
((void)0)

    switch (item->type()) {
      CASE_ITEM(ItemType::Button, "BUTTON");
      CASE_ITEM(ItemType::LayoutRow, "LAYOUT_ROW");
      CASE_ITEM(ItemType::LayoutPanelHeader, "LAYOUT_PANEL_HEADER");
      CASE_ITEM(ItemType::LayoutPanelBody, "LAYOUT_PANEL_BODY");
      CASE_ITEM(ItemType::LayoutColumn, "LAYOUT_COLUMN");
      CASE_ITEM(ItemType::LayoutColumnFlow, "LAYOUT_COLUMN_FLOW");
      CASE_ITEM(ItemType::LayoutRowFlow, "LAYOUT_ROW_FLOW");
      CASE_ITEM(ItemType::LayoutBox, "LAYOUT_BOX");
      CASE_ITEM(ItemType::LayoutAbsolute, "LAYOUT_ABSOLUTE");
      CASE_ITEM(ItemType::LayoutSplit, "LAYOUT_SPLIT");
      CASE_ITEM(ItemType::LayoutOverlap, "LAYOUT_OVERLAP");
      CASE_ITEM(ItemType::LayoutRoot, "LAYOUT_ROOT");
      CASE_ITEM(ItemType::LayoutGridFlow, "LAYOUT_GRID_FLOW");
      CASE_ITEM(ItemType::LayoutRadial, "LAYOUT_RADIAL");
    }

#undef CASE_ITEM

    switch (item->type()) {
      case ItemType::Button:
        layout_introspect_button(ds, static_cast<const ButtonItem *>(item));
        break;
      default:
        fmt::format_to(ds, "'items':");
        layout_introspect_items(ds, (static_cast<const Layout *>(item))->items());
        break;
    }

    fmt::format_to(ds, "}}");

    if (item != items.last()) {
      fmt::format_to(ds, ", ");
    }
  }
  /* Don't use a comma here as it's not needed and
   * causes the result to evaluate to a tuple of 1. */
  fmt::format_to(ds, "]");
}

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
std::string layout_introspect(Layout *layout)
{
  fmt::memory_buffer buffer;
  Vector<Item *> layout_dummy_list(1, layout);
  layout_introspect_items(fmt::appender(buffer), layout_dummy_list);
  return fmt::to_string(buffer);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Alert Box with Big Icon
 * \{ */

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
Layout *uiItemsAlertBox(Block *block,
                        const uiStyle *style,
                        const int dialog_width,
                        const AlertIcon icon,
                        const int icon_size)
{
  /* By default, the space between icon and text/buttons will be equal to the 'columnspace',
   * this extra padding will add some space by increasing the left column width,
   * making the icon placement more symmetrical, between the block edge and the text. */
  const float icon_padding = 5.0f * UI_SCALE_FAC;
  /* Calculate the factor of the fixed icon column depending on the block width. */
  const float split_factor = (float(icon_size) + icon_padding) /
                             float(dialog_width - style->columnspace);

  Layout &block_layout = ui::block_layout(
      block, LayoutDirection::Vertical, LayoutType::Panel, 0, 0, dialog_width, 0, 0, style);

  if (icon == AlertIcon::Info) {
    block->alert_level = BlockAlertLevel::Info;
  }
  else if (icon == AlertIcon::Warning) {
    block->alert_level = BlockAlertLevel::Warning;
  }
  else if (icon == AlertIcon::Question) {
    block->alert_level = BlockAlertLevel::Warning;
  }
  else if (icon == AlertIcon::Error) {
    block->alert_level = BlockAlertLevel::Error;
  }
  else {
    block->alert_level = BlockAlertLevel::None;
  }

  /* Split layout to put alert icon on left side. */
  Layout *split_block = &block_layout.split(split_factor, false);

  /* Alert icon on the left. */
  Layout *layout = &split_block->row(false);
  /* Using 'align_left' with 'row' avoids stretching the icon along the width of column. */
  layout->alignment_set(LayoutAlign::Left);
  uiDefButAlert(block, icon, 0, 0, icon_size, icon_size);

  /* The rest of the content on the right. */
  layout = &split_block->column(false);

  return layout;
}

/* TODO Jean-Silas: this is defined in UI_interface_layout.hh, and should be moved elsewhere */
Layout *uiItemsAlertBox(Block *block, const int size, const AlertIcon icon)
{
  const uiStyle *style = style_get_dpi();
  const short icon_size = 40 * UI_SCALE_FAC;
  const int dialog_width = icon_size + (style->widget.points * size * UI_SCALE_FAC);
  return uiItemsAlertBox(block, style, dialog_width, icon, icon_size);
}

/** \} */

}