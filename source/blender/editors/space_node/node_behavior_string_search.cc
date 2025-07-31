/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_context.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "ED_screen.hh"
#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::ed::space_node {

static void behavior_type_string_search(
    const bContext *C, void *arg, const char *str, uiSearchItems *items, const bool is_first)
{
  if (ED_screen_animation_playing(CTX_wm_manager(C))) {
    return;
  }

  UI_search_item_add(items, "Hello World", nullptr, ICON_NONE, 0, 0);
  UI_search_item_add(items, "Another Hello", nullptr, ICON_NONE, 0, 0);
}

static void behavior_type_string_search_exec(bContext *C, void *data_v, void *item_v) {}

void node_behavior_add_string_search_button(const bContext &C,
                                            const bNode &node,
                                            PointerRNA &socket_ptr,
                                            uiLayout &layout,
                                            const StringRef placeholder)
{
  uiBlock *block = layout.block();
  uiBut *but = uiDefIconTextButR(block,
                                 ButType::SearchMenu,
                                 0,
                                 ICON_NONE,
                                 "",
                                 0,
                                 0,
                                 10 * UI_UNIT_X, /* Dummy value, replaced by layout system. */
                                 UI_UNIT_Y,
                                 &socket_ptr,
                                 "default_value",
                                 0,
                                 "");
  UI_but_placeholder_set(but, placeholder);

  UI_but_func_search_set_results_are_suggestions(but, true);
  UI_but_func_search_set_sep_string(but, UI_MENU_ARROW_SEP);
  UI_but_func_search_set(but,
                         nullptr,
                         behavior_type_string_search,
                         nullptr,
                         true,
                         nullptr,
                         behavior_type_string_search_exec,
                         nullptr);
}

}  // namespace blender::ed::space_node
