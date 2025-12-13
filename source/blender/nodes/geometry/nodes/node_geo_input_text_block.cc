/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>

#include "node_geometry_util.hh"

#include "DNA_text_types.h"

#include "BLI_listbase.h"

namespace blender::nodes::node_geo_input_text_block_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Text>("Text").optional_label();
  b.add_output<decl::String>("String");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Text *text = params.get_input<Text *>("Text");
  if (!text) {
    params.set_default_remaining_outputs();
    return;
  }
  fmt::memory_buffer buffer;
  LISTBASE_FOREACH (const TextLine *, line, &text->lines) {
    fmt::format_to(fmt::appender(buffer), "{}\n", line->line);
  }
  std::string str = fmt::to_string(buffer);
  params.set_output("String", std::move(str));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputTextBlock");
  ntype.ui_name = "Text Block";
  ntype.ui_description = "Get the content of a text block as string";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_text_block_cc
