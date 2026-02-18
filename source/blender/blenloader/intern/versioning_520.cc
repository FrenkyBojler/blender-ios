/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"

#include "BLI_listbase_iterator.hh"
#include "BLI_sys_types.h"

#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"

#include "readfile.hh"

#include "versioning_common.hh"

// #include "CLG_log.h"

namespace blender {

// static CLG_LogRef LOG = {"blend.doversion"};

/* Saving file extension is now a property of the the File Output node. So inherit this
 * setting from the active scene to restore the old behavior.
 * Note: One limitation is that node groups containing file outputs that are not part of any
 * scene are not affected by versioning. */
static void do_version_file_output_use_file_extension_recursive(bNodeTree &node_tree,
                                                                const Scene &scene)
{
  for (bNode &node : node_tree.nodes) {
    if (node.type_legacy == CMP_NODE_OUTPUT_FILE) {
      NodeCompositorFileOutput *data = static_cast<NodeCompositorFileOutput *>(node.storage);
      data->use_file_extension = (scene.r.scemode & R_EXTENSION) != 0;
    }
    else if (node.type_legacy == NODE_GROUP) {
      bNodeTree *ngroup = id_cast<bNodeTree *>(node.id);
      if (ngroup) {
        do_version_file_output_use_file_extension_recursive(*ngroup, scene);
      }
    }
  }
}

void do_versions_after_linking_520(FileData * /*fd*/, Main *bmain)
{
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 2)) {
    for (Scene &scene : bmain->scenes) {
      bNodeTree *node_tree = version_get_scene_compositor_node_tree(bmain, &scene);
      if (node_tree == nullptr) {
        continue;
      }
      do_version_file_output_use_file_extension_recursive(*node_tree, scene);
    }
  }
  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

static void use_geometry_nodes_default_attribute_input(bNodeTree &tree)
{
  /* Previously, the default attribute name only affected modifiers. */
  if (!tree.geometry_node_asset_traits) {
    return;
  }
  if (!(tree.geometry_node_asset_traits->flag & GEO_NODE_ASSET_MODIFIER)) {
    return;
  }

  tree.tree_interface.foreach_item([&](bNodeTreeInterfaceItem &item) {
    if (item.item_type != NODE_INTERFACE_SOCKET) {
      return true;
    }
    auto &io_socket = reinterpret_cast<bNodeTreeInterfaceSocket &>(item);
    if (StringRef(io_socket.default_attribute_name).is_empty()) {
      return true;
    }
    io_socket.default_input = NODE_DEFAULT_INPUT_ATTRIBUTE_FIELD;
    return true;
  });
}

static void use_geometry_nodes_default_attribute_input__forward_compatibility(bNodeTree &tree)
{
  tree.tree_interface.foreach_item([&](bNodeTreeInterfaceItem &item) {
    if (item.item_type != NODE_INTERFACE_SOCKET) {
      return true;
    }
    auto &io_socket = reinterpret_cast<bNodeTreeInterfaceSocket &>(item);
    if (io_socket.use_attribute_name_as_default) {
      io_socket.default_input = NODE_DEFAULT_INPUT_ATTRIBUTE_FIELD;
      io_socket.use_attribute_name_as_default = false;
    }
    return true;
  });
}

void blo_do_versions_520(FileData * /*fd*/, Library * /*lib*/, Main *bmain)
{
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 1)) {
    for (Scene &scene : bmain->scenes) {
      scene.r.mode |= R_SAVE_OUTPUT;
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 4)) {
    for (bNodeTree &tree : bmain->nodetrees) {
      use_geometry_nodes_default_attribute_input(tree);
    }
  }
  /* This is intentionally not in a version check because it reverts changes done for only for
   * forward compatibility. Those changes will also be in newer files. */
  for (bNodeTree &tree : bmain->nodetrees) {
    use_geometry_nodes_default_attribute_input__forward_compatibility(tree);
  }

  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

}  // namespace blender
