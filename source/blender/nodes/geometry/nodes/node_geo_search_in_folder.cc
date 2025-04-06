/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_fileops.h"
#include "BLI_string_utf8.h"

#include "BLI_string_utf8.h"

#include "node_geometry_util.hh"

#include <filesystem>

namespace blender::nodes::node_geo_search_in_folder_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Path")
      .subtype(PROP_FILEPATH)
      .path_filter("*.*")
      .hide_label()
      .description("Path to Folder");
  b.add_input<decl::String>("File Last Name");
  b.add_input<decl::Bool>("Deep Search");

  b.add_output<decl::String>("Files");
  b.add_output<decl::String>("Folders");
}

void append_path_to_string(std::string &target, const std::string &entry_path)
{
  if (target.empty()) {
    target = entry_path;
  }
  else {
    target += "\n" + entry_path;
  }
}
void fliter_path_2s(const std::filesystem::directory_entry &entry,
                    const std::string &extension,
                    std::string &filepaths,
                    std::string &folders)
{
  if (entry.is_regular_file() && entry.path().extension() == extension) {
    append_path_to_string(filepaths, entry.path().string());
  }
  else if (entry.is_directory()) {
    append_path_to_string(folders, entry.path().string());
  }
}
void search_in_folder(const std::string &path,
                      const std::string &lastname,
                      bool deep,
                      std::string &filepaths,
                      std::string &folders)
{
  namespace fs = std::filesystem;

  std::string extension = lastname;
  if (!extension.empty() && extension[0] != '.') {
    extension = "." + extension;
  }

  try {
    if (deep) {
      for (const std::filesystem::directory_entry &entry : fs::recursive_directory_iterator(path))
      {
        fliter_path_2s(entry, extension, filepaths, folders);
      }
    }
    else {
      for (const std::filesystem::directory_entry &entry : fs::directory_iterator(path)) {
        fliter_path_2s(entry, extension, filepaths, folders);
      }
    }
  }
  catch (const fs::filesystem_error &e) {
    filepaths += "error";
  }
}
//////////////////////////////////////////////
static void node_geo_exec(GeoNodeExecParams params)
{
  const std::optional<std::string> path = params.ensure_absolute_path(
      params.extract_input<std::string>("Path"));
  if (!path) {
    params.set_default_remaining_outputs();
    return;
  }
  std::string files;
  std::string folders;

  search_in_folder(*path, params.extract_input<std::string>("File Last Name"),
                   params.extract_input<bool>("Deep Search"), files, folders);

  params.set_output("Files", files);
  params.set_output("Folders", folders);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSearchInFolder", GEO_NODE_SEARCH_IN_FOLDER);
  ntype.ui_name = "Search In Folder";
  ntype.enum_name_legacy = "SEARCH_IN_FOLDER";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_search_in_folder_cc
