/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_context.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "BLI_string_utf8.h"

#include "ED_screen.hh"

#include "NOD_geometry_nodes_behaviors.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_string_search.hh"

namespace blender::ed::space_node {

struct BehaviorSocketSeachData {
  int32_t node_id;
  char socket_identifier[MAX_NAME];
};
/* This class must not have a destructor, since it is used by buttons and freed with #MEM_freeN. */
static_assert(std::is_trivially_destructible_v<BehaviorSocketSeachData>);

static Vector<std::string> get_type_names_from_context(const bContext &C,
                                                       const BehaviorSocketSeachData &data)
{
  SpaceNode *snode = CTX_wm_space_node(&C);
  if (!snode) {
    BLI_assert_unreachable();
    return {};
  }
  bNodeTree *node_tree = snode->edittree;
  if (node_tree == nullptr) {
    BLI_assert_unreachable();
    return {};
  }
  const bNode *node = node_tree->node_by_id(data.node_id);
  if (node == nullptr) {
    BLI_assert_unreachable();
    return {};
  }

  VectorSet<std::string> names = nodes::get_behavior_registry().get_all_behavior_names();
  return names.extract_vector();
}

static void behavior_type_string_search(
    const bContext *C, void *arg, const char *str_ptr, uiSearchItems *items, const bool is_first)
{
  if (ED_screen_animation_playing(CTX_wm_manager(C))) {
    return;
  }

  StringRef str = str_ptr;

  const auto *data = static_cast<BehaviorSocketSeachData *>(arg);
  const Vector<std::string> names = get_type_names_from_context(*C, *data);

  /* Any string is valid, so add the current search string along with the hints. */
  if (!str.is_empty()) {
    if (!names.contains(str)) {
      UI_search_item_add(items, str, nullptr, ICON_NONE, 0, 0);
    }
  }

  if (str.is_empty() && !is_first) {
    /* Allow clearing the text field when the string is empty, but not on the first pass. */
    UI_search_item_add(items, str, nullptr, ICON_X, 0, 0);
  }

  const StringRef search_string = is_first ? "" : str;
  ui::string_search::StringSearch<const std::string> search;
  for (const std::string &name : names) {
    search.add(name, &name);
  }
  const Vector<const std::string *> filtered_items = search.query(search_string);

  for (const std::string *item : filtered_items) {
    if (!UI_search_item_add(items, *item, nullptr, ICON_NONE, 0, 0)) {
      break;
    }
  }
}

// static void behavior_type_string_search_exec(bContext *C, void *data_v, void *item_v) {}

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

  const bNodeSocket &socket = *socket_ptr.data_as<bNodeSocket>();
  BehaviorSocketSeachData *data = MEM_callocN<BehaviorSocketSeachData>(__func__);
  data->node_id = node.identifier;
  STRNCPY_UTF8(data->socket_identifier, socket.identifier);

  UI_but_func_search_set_results_are_suggestions(but, true);
  UI_but_func_search_set_sep_string(but, UI_MENU_ARROW_SEP);
  UI_but_func_search_set(
      but, nullptr, behavior_type_string_search, data, true, nullptr, nullptr, nullptr);
}

}  // namespace blender::ed::space_node
