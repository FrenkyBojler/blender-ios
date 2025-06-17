/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node_runtime.hh"

#include "BLT_translation.hh"

#include "NOD_node_declaration.hh"

#include "node_intern.hh"

namespace blender::ed::space_node {

static std::optional<std::string> create_description_inspection_string(const bNodeSocket &socket)
{
  if (socket.runtime->declaration == nullptr) {
    if (socket.description[0]) {
      return socket.description;
    }
    return std::nullopt;
  }
  const blender::nodes::SocketDeclaration &socket_decl = *socket.runtime->declaration;
  blender::StringRefNull description = socket_decl.description;
  if (description.is_empty()) {
    return std::nullopt;
  }

  return TIP_(description);
}

void build_socket_tooltip(uiTooltipData &tip_data,
                          bContext & /*C*/,
                          const bNodeTree & /*tree*/,
                          const bNodeSocket &socket)
{
  /* Add label. */
  const StringRefNull translated_socket_label = node_socket_get_label(&socket, nullptr);
  UI_tooltip_text_field_add(
      tip_data, translated_socket_label, {}, UI_TIP_STYLE_HEADER, UI_TIP_LC_MAIN);
  UI_tooltip_text_field_add(tip_data, {}, {}, UI_TIP_STYLE_SPACER, UI_TIP_LC_NORMAL);

  /* Add description. */
  if (std::optional<std::string> description_opt = create_description_inspection_string(socket)) {
    std::string description = std::move(*description_opt);
    if (!description.empty()) {
      if (description[description.size() - 1] != '.') {
        description += '.';
      }
      UI_tooltip_text_field_add(tip_data, description, {}, UI_TIP_STYLE_NORMAL, UI_TIP_LC_NORMAL);
    }
  }

  /* Add last value. */

  /* Add last type. */

  /* Add allowed type. */
}

}  // namespace blender::ed::space_node
