/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_node_runtime.hh"
#include "BLT_translation.hh"

#include "NOD_eval_log.hh"
#include "RNA_access.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"
#include "node_intern.hh"

namespace blender::ed::space_node {

namespace eval_log = nodes::eval_log;

class NodeTooltipBuilder {
 private:
  ui::TooltipData &tip_data_;
  const bNodeTree &tree_;
  const bNode &node_;
  ui::Button *but_ = nullptr;
  bContext &C_;
  int indentation_ = 0;
  PointerRNA node_ptr_;

  enum class TooltipBlockType {
    Name,
    Description,
    Warnings,
    Python,
  };

  std::optional<TooltipBlockType> last_block_type_;

 public:
  NodeTooltipBuilder(ui::TooltipData &tip_data,
                     bContext &C,
                     ui::Button *but,
                     const bNodeTree &tree,
                     const bNode &node)
      : tip_data_(tip_data), tree_(tree), node_(node), but_(but), C_(C)
  {
    node_ptr_ = RNA_pointer_create_discrete(
        const_cast<ID *>(&tree.id), RNA_Node, const_cast<bNode *>(&node));
  }

  void build()
  {
    this->build_tooltip_name();
    this->build_tooltip_description();
    this->build_tooltip_warnings();
    this->build_tooltip_python();
  }

 private:
  void build_tooltip_name()
  {
    const std::string ui_name = this->get_ui_name();
    if (ui_name.empty()) {
      return;
    }
    this->start_block(TooltipBlockType::Name);
    this->add_text_field_header(ui_name);
  }

  std::string get_ui_name()
  {
    if (node_.is_group()) {
      if (bNodeTree *group = id_cast<bNodeTree *>(node_.id)) {
        return BKE_id_name(group->id);
      }
    }
    return node_.typeinfo->ui_name;
  }

  void build_tooltip_description()
  {
    const std::string description = node_.typeinfo->ui_description_fn ?
                                        TIP_(node_.typeinfo->ui_description_fn(node_)) :
                                        TIP_(node_.typeinfo->ui_description);
    if (description.empty()) {
      return;
    }
    this->start_block(TooltipBlockType::Description);
    this->add_text_field(description);
  }

  struct NodeWarnings {
    Vector<std::string> info_messages;
    Vector<std::string> warning_messages;
    Vector<std::string> error_messages;
  };

  void build_tooltip_warnings()
  {
    const NodeWarnings warnings = this->gather_node_warnings();
    if (warnings.info_messages.is_empty() && warnings.warning_messages.is_empty() &&
        warnings.error_messages.is_empty())
    {
      return;
    }
    this->start_block(TooltipBlockType::Warnings);
    if (!warnings.error_messages.is_empty()) {
      this->add_text_field(TIP_("Errors:"), ui::TIP_LC_ALERT);
      indentation_++;
      BLI_SCOPED_DEFER([&]() { indentation_--; });
      for (const StringRef message : warnings.error_messages) {
        this->add_text_field(fmt::format("\u2022 {}", message));
      }
    }
    if (!warnings.warning_messages.is_empty()) {
      this->add_text_field(TIP_("Warnings:"), ui::TIP_LC_ALERT);
      indentation_++;
      BLI_SCOPED_DEFER([&]() { indentation_--; });
      for (const StringRef message : warnings.warning_messages) {
        this->add_text_field(fmt::format("\u2022 {}", message));
      }
    }
    if (!warnings.info_messages.is_empty()) {
      this->add_text_field(TIP_("Info:"));
      indentation_++;
      BLI_SCOPED_DEFER([&]() { indentation_--; });
      for (const StringRef message : warnings.info_messages) {
        this->add_text_field(fmt::format("\u2022 {}", message));
      }
    }
  }

  NodeWarnings gather_node_warnings()
  {
    NodeWarnings warnings;
    Main &bmain = *CTX_data_main(&C_);
    SpaceNode *snode = CTX_wm_space_node(&C_);
    if (ELEM(tree_.type, NTREE_GEOMETRY, NTREE_COMPOSIT)) {
      eval_log::ContextualNodeTreeLogs tree_logs;
      if (snode) {
        tree_logs = eval_log::NodesEvalLog::get_contextual_tree_logs(*snode);
      }
      eval_log::NodeTreeLog *tree_log = tree_logs.get_main_tree_log(node_);
      if (!tree_log) {
        return warnings;
      }
      tree_log->ensure_node_warnings(bmain);
      eval_log::NodeLog *node_log = tree_log->nodes.lookup_ptr(node_.identifier);
      if (!node_log) {
        return warnings;
      }
      for (const eval_log::NodeWarning &warning : node_log->warnings) {
        switch (warning.type) {
          case nodes::NodeWarningType::Error: {
            warnings.error_messages.append(warning.message);
            break;
          }
          case nodes::NodeWarningType::Warning: {
            warnings.warning_messages.append(warning.message);
            break;
          }
          case nodes::NodeWarningType::Info: {
            warnings.info_messages.append(warning.message);
            break;
          }
        }
      }
      return warnings;
    }
    if (tree_.type == NTREE_SHADER) {
      std::lock_guard lock(tree_.runtime->shader_node_errors_mutex);
      const VectorSet<std::string> *node_errors = tree_.runtime->shader_node_errors.lookup_ptr(
          node_.identifier);
      if (!node_errors) {
        return warnings;
      }
      for (const std::string &error : *node_errors) {
        warnings.error_messages.append(error);
      }
    }
    return warnings;
  }

  void build_tooltip_python()
  {
    if (!(U.flag & USER_TOOLTIPS_PYTHON)) {
      return;
    }
    this->start_block(TooltipBlockType::Python);
    this->add_text_field_mono(fmt::format(
        "Python: {}\n{}", node_.idname, RNA_path_full_struct_py(&node_ptr_).value_or("")));
  }

  void start_block(const TooltipBlockType new_block_type)
  {
    if (last_block_type_.has_value()) {
      this->add_space(2);
    }
    last_block_type_ = new_block_type;
  }

  void add_text_field_header(std::string text)
  {
    ui::tooltip_text_field_add(
        tip_data_, this->indent(text), {}, ui::TIP_STYLE_HEADER, ui::TIP_LC_MAIN);
  }

  void add_text_field(std::string text, const ui::TooltipColorID color_id = ui::TIP_LC_NORMAL)
  {
    ui::tooltip_text_field_add(tip_data_, this->indent(text), {}, ui::TIP_STYLE_NORMAL, color_id);
  }

  void add_text_field_mono(std::string text, const ui::TooltipColorID color_id = ui::TIP_LC_VALUE)
  {
    ui::tooltip_text_field_add(tip_data_, this->indent(text), {}, ui::TIP_STYLE_MONO, color_id);
  }

  void add_space(const int amount = 1)
  {
    for ([[maybe_unused]] const int i : IndexRange(amount)) {
      ui::tooltip_text_field_add(tip_data_, {}, {}, ui::TIP_STYLE_SPACER, ui::TIP_LC_NORMAL);
    }
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
