/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup nodes
 */

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "NOD_geometry_nodes_execute.hh"

namespace blender::nodes {

using PropertiesVectorSet = CustomIDVectorSet<IDProperty *, IDPropNameGetter, 16>;

namespace geo_eval_log {
struct GeoTreeLog;
};
namespace geo_log = geo_eval_log;

struct PanelOpenProperty {
  PointerRNA ptr;
  StringRefNull name;
};

struct SearchInfo {
  geo_log::GeoTreeLog *tree_log = nullptr;
  bNodeTree *tree = nullptr;
  IDProperty *properties = nullptr;
};

struct ModifierSearchData {
  uint32_t object_session_uid;
  char modifier_name[MAX_NAME];
};

struct OperatorSearchData {
  /** Can store this data directly, because it's more persistent than for the modifier. */
  SearchInfo info;
};

struct SocketSearchData {
  std::variant<ModifierSearchData, OperatorSearchData> search_data;
  char socket_identifier[MAX_NAME];
  bool is_output;

  SearchInfo info(const bContext &C) const;
};
/* This class must not have a destructor, since it is used by buttons and freed with #MEM_freeN. */
BLI_STATIC_ASSERT(std::is_trivially_destructible_v<SocketSearchData>, "");

struct DrawGroupInputsContext {
  const bContext &C;
  bNodeTree *tree;
  geo_log::GeoTreeLog *tree_log;
  nodes::PropertiesVectorSet properties;
  PointerRNA *properties_ptr;
  PointerRNA *bmain_ptr;
  Array<nodes::socket_usage_inference::SocketUsage> input_usages;
  bool use_name_for_ids = false;
  std::function<PanelOpenProperty(const bNodeTreeInterfacePanel &)> panel_open_property_fn;
  std::function<SocketSearchData(const bNodeTreeInterfaceSocket &)> socket_search_data_fn;
  std::function<void(uiLayout &, int icon, const bNodeTreeInterfaceSocket &)>
      draw_attribute_toggle_fn;

  bool input_is_visible(const bNodeTreeInterfaceSocket &socket) const
  {
    return this->input_usages[this->tree->interface_input_index(socket)].is_visible;
  }

  bool input_is_active(const bNodeTreeInterfaceSocket &socket) const
  {
    return this->input_usages[this->tree->interface_input_index(socket)].is_used;
  }
};

void draw_interface_panel_content(DrawGroupInputsContext &ctx,
                                  uiLayout *layout,
                                  const bNodeTreeInterfacePanel &interface_panel,
                                  const bool skip_first = false,
                                  const std::optional<StringRef> parent_name = std::nullopt);

}  // namespace blender::nodes
