/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLT_translation.hh"

#include "node_intern.hh"

namespace blender::ed::space_node {

class NodeTooltipBuilder {
 private:
  ui::TooltipData &tip_data_;
  const bNodeTree &tree_;
  const bNode &node_;
  ui::Button *but_ = nullptr;
  bContext &C_;
  int indentation_ = 0;

 public:
  NodeTooltipBuilder(ui::TooltipData &tip_data,
                     bContext &C,
                     ui::Button *but,
                     const bNodeTree &tree,
                     const bNode &node)
      : tip_data_(tip_data), tree_(tree), node_(node), but_(but), C_(C)
  {
  }

  void build()
  {
    this->add_text_field(TIP_("Hello world"));
  }

  void add_text_field(std::string text, const ui::TooltipColorID color_id = ui::TIP_LC_NORMAL)
  {
    ui::tooltip_text_field_add(tip_data_, this->indent(text), {}, ui::TIP_STYLE_NORMAL, color_id);
  }

  void add_text_field_mono(std::string text, const ui::TooltipColorID color_id = ui::TIP_LC_VALUE)
  {
    ui::tooltip_text_field_add(tip_data_, this->indent(text), {}, ui::TIP_STYLE_MONO, color_id);
  }

  std::string indent(std::string text)
  {
    if (indentation_ == 0) {
      return text;
    }
    return fmt::format("{: <{}}{}", "", indentation_, text);
  }
};

void build_node_tooltip(ui::TooltipData &tip_data,
                        bContext &C,
                        ui::Button *but,
                        const bNodeTree &tree,
                        const bNode &node)
{
  NodeTooltipBuilder builder(tip_data, C, but, tree, node);
  builder.build();
}

}  // namespace blender::ed::space_node
