/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_utf8.h"

#include "node_function_util.hh"

#include <filesystem>

namespace blender::nodes::node_fn_search_in_folder_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Path")
      .subtype(PROP_FILEPATH)
      .path_filter("*.*")
      .hide_label()
      .description("Path to Folder");
  b.add_input<decl::String>("LastName");
  b.add_input<decl::Bool>("Deep Search");
  b.add_output<decl::String>("File Pathes");
}

std::string search_in_folder(const std::string &path, const std::string &lastname, bool deep)
{
  std::string filepaths;
  namespace fs = std::filesystem;

  std::string extension = lastname;
  if (!extension.empty() && extension[0] != '.') {
    extension = "." + extension;
  }

  try {
    if (deep) {
      for (const auto &entry : fs::recursive_directory_iterator(path)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
          if (filepaths.empty()) {
            filepaths = entry.path().string();
          }
          else {
            filepaths += "\n" + entry.path().string();
          }
        }
      }
    }
    else {
      for (const auto &entry : fs::directory_iterator(path)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
          if (filepaths.empty()) {
            filepaths = entry.path().string();
          }
          else {
            filepaths += "\n" + entry.path().string();
          }
        }
      }
    }
  }
  catch (const fs::filesystem_error &e) {
    filepaths += "error";
  }

  return filepaths;
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  static auto slice_fn = mf::build::SI3_SO<std::string, std::string, bool, std::string>(
      "Files", [](const std::string &p, std::string l, bool d) {
        std::string files = search_in_folder(p, l, d);
        return files;
      });
  builder.set_matching_fn(&slice_fn);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  fn_node_type_base(&ntype, "FunctionNodeSearchInFolder", FN_NODE_SEARCH_IN_FOLDER);
  ntype.ui_name = "Search In Folder";
  ntype.enum_name_legacy = "SEARCH_IN_FOLDER";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_search_in_folder_cc
