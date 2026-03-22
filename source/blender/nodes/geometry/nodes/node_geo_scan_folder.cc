/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_fileops.h"
#include "BLI_string_utf8.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_scan_folder_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Path")
      .subtype(PROP_FILEPATH)
      .path_filter("*.*")
      .description("Path to Folder");
  b.add_input<decl::Bool>("Deep Search");

  b.add_output<decl::String>("Files")
  .structure_type(StructureType::List);
  b.add_output<decl::String>("Folders")
  .structure_type(StructureType::List);
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
void bli_scan_folder(const StringRef path,
                     bool deep,
                     Vector<std::string> &filepaths,
                     Vector<std::string> &folders)
{

  direntry *filelist = nullptr;
  uint num_files = BLI_filelist_dir_contents(path.data(), &filelist);

  if (num_files == 0) {
    return;
  }

  for (uint i = 0; i < num_files; i++) {
    direntry *entry = &filelist[i];
    const char *filename = entry->relname;

    if (S_ISREG(entry->type))
    {
      //append_path_to_string(filepaths, path + entry->relname);
      filepaths.append(path + entry->relname);
    }
    else if (S_ISDIR(entry->type) && strcmp(filename, ".") != 0 && strcmp(filename, "..") != 0) {
      std::string sub_path = path + entry->relname + "/";
      //append_path_to_string(folders, sub_path);
      folders.append(sub_path);
      if (deep) {
        bli_scan_folder(sub_path, deep, filepaths, folders);
      }
    }
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const std::optional<std::string> path = params.ensure_absolute_path(
      params.extract_input<std::string>("Path"));
  if (!path) {
    params.set_default_remaining_outputs();
    return;
  }
  Vector<std::string> files;
  Vector<std::string> folders;

  bli_scan_folder(*path,
                       params.extract_input<bool>("Deep Search"),
                       files,
                       folders);

  params.set_output("Files", List::from_container(std::move(files)));
  params.set_output("Folders", List::from_container(std::move(folders)));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeScanFolder", GEO_NODE_SCAN_FOLDER);
  ntype.ui_name = "Scan Folder";
  ntype.enum_name_legacy = "SEARCH_IN_FOLDER";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_search_in_folder_cc
