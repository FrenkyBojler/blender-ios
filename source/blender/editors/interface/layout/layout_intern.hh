/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#pragma once

#include "RNA_access.hh"
#include "WM_types.hh"
#include "BLI_enum_flags.hh"

namespace blender::ui {
struct LayoutItemBx;
/* Forward Declarations */
struct Layout;
struct ButtonItem;

/**
 * \param _caller_fn_name: A friendly function name of the caller for tracing layout item operator
 * warnings, matching the RNA struct function name. For example `"UILayout.operator()"`
 */
#define UI_OPERATOR_ERROR_RET(_ot, _opname, _caller_fn_name) \
  if (ot == nullptr) { \
    item_disabled(this, _opname); \
    RNA_warning_bare("%s: '%s' unknown operator", _caller_fn_name, _opname); \
    return PointerRNA_NULL; \
  } \
  (void)0

#define UI_ITEM_PROP_SEP_DIVIDE 0.4f

/* uiLayoutRoot */

struct LayoutRoot {
  LayoutRoot *next, *prev;

  LayoutType type;
  wm::OpCallContext opcontext;

  int emw, emh;
  int padding;

  const uiStyle *style;
  Block *block;
  Layout *layout;
};

/* Item */

enum class ItemType : int8_t {
  Button,

  LayoutRow,
  LayoutPanelHeader,
  LayoutPanelBody,
  LayoutColumn,
  LayoutColumnFlow,
  LayoutRowFlow,
  LayoutGridFlow,
  LayoutBox,
  LayoutAbsolute,
  LayoutSplit,
  LayoutOverlap,
  LayoutRadial, /* AKA: menu pie. */

  LayoutRoot,
#if 0
  TemplateColumnFlow,
  TemplateSplit,
  TemplateBox,

  TemplateHeader,
  TemplateHeaderID,
#endif
};

enum class ItemInternalFlag : uint8_t {
  AutoFixedSize = 1 << 0,
  FixedSize = 1 << 1,

  BoxItem = 1 << 2, /* The item is "inside" a box item */
  PropSep = 1 << 3,
  InsidePropSep = 1 << 4,
  /* Show an icon button next to each property (to set keyframes, show status).
   * Enabled by default, depends on 'ItemInternalFlag::PropSep'. */
  PropDecorate = 1 << 5,
  PropDecorateNoPad = 1 << 6,
};
ENUM_OPERATORS(ItemInternalFlag)

/** Helper internal struct to provide #uiItem private/protected access. */
struct ItemInternal {
  static void inside_property_split_set(Item *item, bool inside_prop_sep)
  {
    SET_FLAG_FROM_TEST(item->flag_, inside_prop_sep, ItemInternalFlag::InsidePropSep);
  }

  [[nodiscard]] static bool use_property_decorate_no_pad(const Item *item)
  {
    return flag_is_set(item->flag_, ItemInternalFlag::PropDecorateNoPad);
  };

  [[nodiscard]] static bool box_item(const Item *item)
  {
    return flag_is_set(item->flag_, ItemInternalFlag::BoxItem);
  }
  static void box_item_set(Item *item, bool box_item)
  {
    SET_FLAG_FROM_TEST(item->flag_, box_item, ItemInternalFlag::BoxItem);
  }

  [[nodiscard]] static bool auto_fixed_size(const Item *item)
  {
    return flag_is_set(item->flag_, ItemInternalFlag::AutoFixedSize);
  }
  static void auto_fixed_size_set(Item *item, bool auto_fixed_size)
  {
    SET_FLAG_FROM_TEST(item->flag_, auto_fixed_size, ItemInternalFlag::AutoFixedSize);
  }
};

/** Helper internal struct to provide #Layout private/protected access. */
struct LayoutInternal {
  static void init_from_parent(Layout *item, Layout *layout, int align);

  static void layout_add_but(Layout *layout, Button *but);
  static void layout_remove_but(Layout *layout, const Button *but);
  static void layout_estimate(Layout *layout);
  static void layout_resolve(Layout *layout);
  static ButtonItem *layout_find_button_item(const Layout *layout, const Button *but);
  static Layout *item_prop_split_layout_hack(Layout *layout_parent, Layout *layout_split);
  static void layout_offset_size_set(Layout *layout, int x, int y, int w, int h);
  static void layout_move(Layout *layout, int delta_xmin, int delta_xmax);
  static void layout_space_set(Layout *layout, int space);
  static int layout_space_get(Layout *layout);
};

/* -------------------------------------------------------------------- */
/** \name Item
 * \{ */

/* variable button size in which direction? */
#define UI_ITEM_VARY_X 1
#define UI_ITEM_VARY_Y 2

/**
 * Factors to apply to #UI_UNIT_X when calculating button width.
 * This is used when the layout is a varying size, see #layout_variable_size.
 */
struct TextIconPadFactor {
  float text;
  float icon;
  float icon_only;
};

/**
 * This adds over an icons width of padding even when no icon is used,
 * this is done because most buttons need additional space (drop-down chevron for example).
 * menus and labels use much smaller `text` values compared to this default.
 *
 * \note It may seem odd that the icon only adds 0.25, but taking margins into account it's fine,
 * except for #text_pad_compact where a bit more margin is required.
 */
constexpr TextIconPadFactor text_pad_default = {1.50f, 0.25f, 0.0f};

/** #text_pad_default scaled down. */
constexpr TextIconPadFactor text_pad_compact = {1.25f, 0.35f, 0.0f};

/** Least amount of padding not to clip the text or icon. */
constexpr TextIconPadFactor text_pad_none = {0.25f, 1.50f, 0.0f};

StringRef item_name_add_colon(StringRef name, char namestr[UI_MAX_NAME_STR]);
StringRefNull item_name_add_colon(StringRefNull name, char namestr[UI_MAX_NAME_STR]);
int item_fit(const int item,
             const int pos,
             const int all,
             const int available,
             const bool is_last,
             const LayoutAlign alignment,
             float *extra_pixel);

int layout_vary_direction(Layout *layout);
bool layout_variable_size(Layout *layout);

/**
 * Estimated size of text + icon.
 */
int text_icon_width_ex(Layout *layout,
                              const StringRef name,
                              int icon,
                              const TextIconPadFactor &pad_factor,
                              const uiFontStyle *fstyle);


int text_icon_width(Layout *layout,
                           const StringRef name,
                           const int icon,
                           const bool compact);

void item_position(Item *item, const int x, const int y, const int w, const int h);
void item_move(Item *item, const int delta_xmin, const int delta_xmax);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Special RNA Items
 * \{ */

Layout *item_local_sublayout(Layout *test, Layout *layout, bool align);
void layer_but_cb(bContext *C, void *arg_but, void *arg_index);

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
                       const bool show_text);

void item_enum_expand_handle(bContext *C, void *arg1, void *arg2);

void item_enum_expand_elem_exec(Layout *layout,
                                       Block *block,
                                       PointerRNA *ptr,
                                       PropertyRNA *prop,
                                       const std::optional<StringRef> uiname,
                                       const int h,
                                       const ButtonType but_type,
                                       const bool icon_only,
                                       const EnumPropertyItem *item,
                                       const bool is_first);

void item_enum_expand_exec(Layout *layout,
                                  Block *block,
                                  PointerRNA *ptr,
                                  PropertyRNA *prop,
                                  const std::optional<StringRef> uiname,
                                  const int h,
                                  const ButtonType but_type,
                                  const bool icon_only);

void item_enum_expand(Layout *layout,
                             Block *block,
                             PointerRNA *ptr,
                             PropertyRNA *prop,
                             const std::optional<StringRef> uiname,
                             const int h,
                             const bool icon_only);

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
                                  EnumTabExpand expand_as);

/* callback for keymap item change button */
void keymap_but_cb(bContext * /*C*/, void *but_v, void * /*key_v*/);

/**
 * Create label + button for RNA property
 *
 * \param w_hint: For varying width layout, this becomes the label width.
 *                Otherwise it's used to fit both items into it.
 * \param button_type: Overrides the default button type for \a prop, see #uiDefAutoButR.
 * \param caller_fn_name: A friendly function name of the caller for tracing keymap item warnings,
 * matching the RNA struct function name. For example `"UILayout.prop()"`.
 */
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
                               const char *caller_fn_name);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Button Items
 * \{ */

/**
 * Update a buttons tip with an enum's description if possible.
 */
void but_tip_from_enum_item(Button *but, const EnumPropertyItem *item);

/* disabled item */
void item_disabled(Layout *layout, const char *name);

/**
 * Operator Item
 * \param r_opptr: Optional, initialize with operator properties when not nullptr.
 * Will always be written to even in the case of errors.
 */
Button *uiItemFullO_ptr_ex(Layout *layout,
                                  wmOperatorType *ot,
                                  std::optional<StringRef> name,
                                  int icon,
                                  const wm::OpCallContext context,
                                  const eUI_Item_Flag flag,
                                  PointerRNA *r_opptr);

void item_menu_hold(bContext *C, ARegion *butregion, Button *but);

/* RNA property items */
void item_rna_size(Layout *layout,
                          StringRef name,
                          int icon,
                          PointerRNA *ptr,
                          PropertyRNA *prop,
                          int index,
                          bool icon_only,
                          bool compact,
                          int *r_w,
                          int *r_h);

bool item_rna_is_expand(PropertyRNA *prop, int index, const eUI_Item_Flag item_flag);

/**
 * Find first layout ancestor (or self) with a heading set.
 *
 * \returns the layout to add the heading to as a fallback (i.e. if it can't be placed in a split
 *          layout). Its #Layout.heading member can be cleared to mark the heading as added (so
 *          it's not added multiple times). Returns a pointer to the heading
 */
Layout *layout_heading_find(Layout *cur_layout);

void layout_heading_label_add(Layout *layout,
                                     Layout *heading_layout,
                                     bool right_align,
                                     bool respect_prop_split);

/* Pointer RNA button with search */
void search_id_collection(StructRNA *ptype, PointerRNA *r_ptr, PropertyRNA **r_prop);
void rna_collection_search_arg_free_fn(void *ptr);

Button *item_menu(Layout *layout,
                         const StringRef name,
                         int icon,
                         MenuCreateFunc func,
                         void *arg,
                         void *argN,
                         const std::optional<StringRef> tip,
                         bool force_menu,
                         ButtonArgNFree func_argN_free_fn = MEM_delete_void,
                         ButtonArgNCopy func_argN_copy_fn = MEM_dupalloc_void);

/**
 * Single button with an icon and/or text, using the given button type and no further data/behavior
 * attached.
 */
Button *uiItem_simple(Layout *layout,
                             const StringRef name,
                             int icon,
                             std::optional<StringRef> tooltip = std::nullopt,
                             const ButtonType but_type = ButtonType::Label);


struct MenuItemLevel {
  wm::OpCallContext opcontext;
  /* don't use pointers to the strings because python can dynamically
   * allocate strings and free before the menu draws, see #27304. */
  char opname[OP_MAX_TYPENAME];
  char propname[MAX_IDPROP_NAME];
  PointerRNA rnapoin;
};

/* Obtain the active menu item based on the calling button's text. */
int menu_item_enum_opname_menu_active(bContext *C, Button *but, MenuItemLevel *lvl);
void menu_item_enum_opname_menu(bContext *C, Layout *layout, void *arg);
/** \} */

/* -------------------------------------------------------------------- */
/** \name Layout Items
 * \{ */

int litem_min_width(int itemw);
int spaces_after_column_item(const Layout *litem,
                                    const Item *item,
                                    const Item *next_item,
                                    const bool is_box);

/* calculates the angle of a specified button in a radial menu,
 * stores a float vector in unit circle */
RadialDirection get_radialbut_vec(float vec[2], short itemnum);

bool item_is_radial_displayable(Item *item);
bool item_is_radial_drawable(ButtonItem *bitem);

/* multi-column and multi-row layout. */
struct UILayoutGridFlowInput {
  /* General layout control settings. */
  bool row_major : 1;    /* Fill rows before columns */
  bool even_columns : 1; /* All columns will have same width. */
  bool even_rows : 1;    /* All rows will have same height. */
  int space_x;           /* Space between columns. */
  int space_y;           /* Space between rows. */
  /* Real data about current position and size of this layout item
   * (either estimated, or final values). */
  int litem_w;           /* Layout item width. */
  int litem_x;           /* Layout item X position. */
  int litem_y;           /* Layout item Y position. */
  /* Actual number of columns and rows to generate (computed from first pass usually). */
  int tot_columns; /* Number of columns. */
  int tot_rows;    /* Number of rows. */
};

struct UILayoutGridFlowOutput {
  int *tot_items; /* Total number of items in this grid layout. */
  /* Width / X pos data. */
  float *global_avg_w; /* Computed average width of the columns. */
  int *cos_x_array;    /* Computed X coordinate of each column. */
  int *widths_array;   /* Computed width of each column. */
  int *tot_w;          /* Computed total width. */
  /* Height / Y pos data. */
  int *global_max_h;  /* Computed height of the tallest item in the grid. */
  int *cos_y_array;   /* Computed Y coordinate of each column. */
  int *heights_array; /* Computed height of each column. */
  int *tot_h;         /* Computed total height. */
};

void litem_grid_flow_compute(Span<Item *> items,
                                    const UILayoutGridFlowInput *parameters,
                                    UILayoutGridFlowOutput *results);

LayoutItemBx *layout_box(Layout *layout, ButtonType type);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Block Layout Search Filtering
 * \{ */

/* Disabled for performance reasons, but this could be turned on in the future. */
// #define PROPERTY_SEARCH_USE_TOOLTIPS

bool block_search_panel_label_matches(const Block *block, const char *search_string);

/**
 * Returns true if a button or the data / operator it represents matches the search filter.
 */
bool button_matches_search_filter(Button *but, const char *search_filter);

/**
 * Test for a search result within a specific button group.
 */
bool button_group_has_search_match(const ButtonGroup &group, const char *search_filter);

/**
 * Apply the search filter, tagging all buttons with whether they match or not.
 * Tag every button in the group as a result if any button in the group matches.
 *
 * \note It would be great to return early here if we found a match, but because
 * the results may be visible we have to continue searching the entire block.
 *
 * \return True if the block has any search results.
 */
bool block_search_filter_tag_buttons(Block *block, const char *search_filter);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layout
 * \{ */

void item_scale(Layout *litem, const float scale[2]);

void item_align(Layout *litem, short nr);

void item_flag(Layout *litem, int flag);

int2 layout_end(Layout *layout);

void layout_free(Layout *layout);

void layout_add_padding_button(LayoutRoot *root);

bool layout_has_panel_label(const Layout *layout, const PanelType *pt);

void paneltype_draw_impl(bContext *C, PanelType *pt, Layout *layout, bool show_header);

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

void layout_introspect_button(fmt::appender ds, const ButtonItem *bitem);

void layout_introspect_items(fmt::appender ds, Span<const Item *> items);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Alert Box with Big Icon
 * \{ */

/** \} */

} // blender::ui