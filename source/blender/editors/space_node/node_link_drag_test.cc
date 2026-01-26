/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "tests/blendfile_loading_base_test.h"

#include "testing/testing.h"

#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_path_utils.hh"
#include "BLI_struct_equality_utils.hh"

#include "BKE_blender.hh"
#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_screen.hh"
#include "BKE_shader_fx.hh"

#include "BLO_readfile.hh"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "NOD_composite.hh"
#include "NOD_geometry.hh"
#include "NOD_shader.h"
#include "NOD_socket_search_link.hh"

#include "ED_space_api.hh"

#include "RE_engine.h"

#include "UI_interface.hh"

#include "WM_api.hh"
#include "wm.hh"
#include "wm_event_system.hh"

#include <fmt/format.h>

DEFINE_string(skip_compositor_ops, "", "Compositor link operations to skip when testing.");
DEFINE_string(skip_geometry_ops, "", "Geometry link operations to skip when testing.");
DEFINE_string(skip_shader_ops, "", "Shader link operations to skip when testing.");

namespace blender::ed::space_node::tests {

using bke::bNodeSocketType;
using bke::bNodeTreeType;

struct LinkOpFilter {
  std::string tree_idname;
  std::string link_op_name;
  /* Optional, if empty all socket types will skip this link op. */
  std::string socket_type;

  BLI_STRUCT_EQUALITY_OPERATORS_2(LinkOpFilter, link_op_name, socket_type);

  uint64_t hash() const
  {
    return get_default_hash(link_op_name, socket_type);
  }
};

static Set<LinkOpFilter> create_skipped_link_ops()
{
  Set<LinkOpFilter> ops = {};

  auto add_skipped_ops = [&](const StringRef tree_idname, const StringRef arg) {
    const std::string delimiter = ":";
    std::string s = arg;
    size_t pos = 0;
    std::string token;
    while ((pos = s.find(delimiter)) != std::string::npos) {
      token = s.substr(0, pos);
      ops.add({tree_idname, token});
      s.erase(0, pos + delimiter.length());
    }
    if (!s.empty()) {
      ops.add({tree_idname, s});
    }
  };

  add_skipped_ops("CompositorNodeTree", FLAGS_skip_compositor_ops);
  add_skipped_ops("GeometryNodeTree", FLAGS_skip_geometry_ops);
  add_skipped_ops("ShaderNodeTree", FLAGS_skip_shader_ops);

  return ops;
}

/* Skipped failing tests. */
static const Set<LinkOpFilter> &get_skipped_link_ops()
{
  static Set<LinkOpFilter> skipped_link_ops = create_skipped_link_ops();
  return skipped_link_ops;
}

class NodeLinkDragTest : public BlendfileLoadingBaseTest {
 public:
  static void SetUpTestCase()
  {
    BlendfileLoadingBaseTest::SetUpTestCase();
    BKE_shaderfx_init();
    ED_spacetypes_init();
    RE_engines_init();
    ui::style_init_default();
  }

  static void TearDownTestCase()
  {
    wm_operatortype_free();
    WM_menutype_free();
    WM_uilisttype_free();
    wm_dropbox_free();
    wm_gizmotype_free();
    wm_gizmogrouptype_free();
    wm_gizmomaptypes_free();
    BKE_spacetypes_free();
    RE_engines_exit();
    BKE_blender_userdef_data_free(&U, false);
    BlendfileLoadingBaseTest::TearDownTestCase();
  }

  void SetUp() override
  {
    BlendfileLoadingBaseTest::SetUp();
  }

  void TearDown() override
  {
    BlendfileLoadingBaseTest::TearDown();
  }
};

static bool set_space_node_context(const Main &bmain, const StringRef tree_idname, bContext &C)
{
  for (bScreen &screen : bmain.screens) {
    for (ScrArea &area : screen.areabase) {
      for (SpaceLink &sl : area.spacedata) {
        if (sl.spacetype == SPACE_NODE) {
          SpaceNode &snode = reinterpret_cast<SpaceNode &>(sl);
          if (tree_idname == snode.tree_idname) {
            CTX_wm_screen_set(&C, &screen);
            CTX_wm_area_set(&C, &area);
            // for (ARegion &region : sl.regionbase) {
            //   if (region.regiontype == RGN_TYPE_WINDOW) {
            //     CTX_wm_region_set(&C, &region);
            //     break;
            //   }
            // }
            return true;
          }
        }
      }
    }
  }
  return false;
}

using nodes::SocketLinkOperation;

/**
 * Call the callback to gather compatible socket connections for all node types, and the operations
 * that will actually make the connections. Also add some custom operations like connecting a group
 * output node.
 */
static void gather_socket_link_operations(const bContext &C,
                                          bNodeTree &node_tree,
                                          const bNodeSocket &socket,
                                          Vector<SocketLinkOperation> &search_link_ops)
{
  const SpaceNode &snode = *CTX_wm_space_node(&C);
  for (const bke::bNodeType *node_type : bke::node_types_get()) {
    const char *disabled_hint;
    if (node_type->poll && !node_type->poll(node_type, &node_tree, &disabled_hint)) {
      continue;
    }
    if (node_type->add_ui_poll && !node_type->add_ui_poll(&C)) {
      continue;
    }
    if (StringRefNull(node_type->ui_name).endswith("(Legacy)")) {
      continue;
    }
    if (node_type->gather_link_search_ops) {
      nodes::GatherLinkSearchOpParams params{
          *node_type, snode, node_tree, socket, search_link_ops};
      node_type->gather_link_search_ops(params);
    }
  }

  /* Only base node type search items are used, excluding node groups and assets. */
#if 0
  search_link_ops.append({IFACE_("Reroute"), add_reroute_node_fn});

  const bool is_node_group = !(node_tree.id.flag & ID_FLAG_EMBEDDED_DATA);

  if (is_node_group && socket.in_out == SOCK_IN) {
    search_link_ops.append({IFACE_("Group Input"), add_group_input_node_fn});

    int weight = -1;
    node_tree.tree_interface.foreach_item([&](const bNodeTreeInterfaceItem &item) {
      if (item.item_type != NODE_INTERFACE_SOCKET) {
        return true;
      }
      const bNodeTreeInterfaceSocket &interface_socket =
          reinterpret_cast<const bNodeTreeInterfaceSocket &>(item);
      if (!(interface_socket.flag & NODE_INTERFACE_SOCKET_INPUT)) {
        return true;
      }
      {
        const bke::bNodeSocketType *from_typeinfo = bke::node_socket_type_find(
            interface_socket.socket_type);
        const eNodeSocketDatatype from = from_typeinfo ? from_typeinfo->type : SOCK_CUSTOM;
        const eNodeSocketDatatype to = socket.typeinfo->type;
        if (node_tree.typeinfo->validate_link && !node_tree.typeinfo->validate_link(from, to)) {
          return true;
        }
      }
      search_link_ops.append({std::string(IFACE_("Group Input")) + " " + UI_MENU_ARROW_SEP +
                                  (interface_socket.name ? interface_socket.name : ""),
                              [interface_socket](nodes::LinkSearchOpParams &params) {
                                add_existing_group_input_fn(params, interface_socket);
                              },
                              weight});
      weight--;
      return true;
    });
  }

  gather_search_link_ops_for_all_assets(C, node_tree, socket, search_link_ops);
#endif
}

TEST_F(NodeLinkDragTest, NodeLinkDrag)
{
  /* Load the test blend file. */
  if (!blendfile_load("node_group" SEP_STR "link_drag_search.blend")) {
    return;
  }

  Main *bmain = bfile->main;

  bContext *C = CTX_create();
  CTX_data_main_set(C, bmain);
  CTX_data_scene_set(C, bfile->curscene);
  CTX_wm_screen_set(C, bfile->curscreen);

  const Vector<std::string> tree_idnames = {
      ntreeType_Composite->idname, ntreeType_Geometry->idname, ntreeType_Shader->idname};
  const Vector<std::string> group_node_idnames = {ntreeType_Composite->group_idname,
                                                  ntreeType_Geometry->group_idname,
                                                  ntreeType_Shader->group_idname};
  const Vector<Vector<NodeSocketInterfaceStructureType>> structure_types = {
      {
          NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO,
      },
      {
          NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO,
          NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_SINGLE,
          NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_DYNAMIC,
          NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_FIELD,
          NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_GRID,
          NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_LIST,
      },
      {
          NODE_INTERFACE_SOCKET_STRUCTURE_TYPE_AUTO,
      }};
  const Vector<std::string> structure_type_names = {
      "Auto",
      "Single",
      "Dynamic",
      "Field",
      "Grid",
      "List",
  };

  for (const int tree_type_i : tree_idnames.index_range()) {
    const StringRef tree_idname = tree_idnames[tree_type_i];
    const StringRef group_node_idname = group_node_idnames[tree_type_i];

    bool context_ok = set_space_node_context(*bmain, tree_idname, *C);
    ASSERT_TRUE(context_ok);

    bNodeTree *tree = bke::node_tree_add_tree(bmain, "NodeTree", tree_idname);
    const bNodeTreeType *tree_type = tree->typeinfo;
    SpaceNode *snode = CTX_wm_space_node(C);
    ASSERT_NE(snode, nullptr);
    snode->edittree = tree;

    /* Add a node group with one socket for each supported type. */
    bNodeTree *group_tree = bke::node_tree_add_tree(bmain, "NodeGroup", tree_idname);
    bNode *group_node = bke::node_add_node(C, *tree, group_node_idname);
    group_node->id = &group_tree->id;
    BKE_ntree_update_tag_node_property(tree, group_node);

    int num_socket_types = 0;
    for (const bNodeSocketType *socket_type : bke::node_socket_types_get()) {
      /* Only test base socket types, the subtype should not affect link operations and testing
       * every subtype generates excessive test cases. */
      if (socket_type != bke::node_socket_type_find_static(socket_type->type, PROP_NONE)) {
        continue;
      }
      if (tree_type->valid_socket_type &&
          !tree_type->valid_socket_type(const_cast<bNodeTreeType *>(tree_type),
                                        const_cast<bNodeSocketType *>(socket_type)))
      {
        continue;
      }
      /* Only test builtin socket types. This also skips extension sockets, which prevents adding
       * more sockets to the group. */
      if (socket_type->type == SOCK_CUSTOM) {
        continue;
      }

      for (const NodeSocketInterfaceStructureType structure_type : structure_types[tree_type_i]) {
        ++num_socket_types;
        bNodeTreeInterfaceSocket *io_input = group_tree->tree_interface.add_socket(
            "Socket", "", socket_type->idname, NODE_INTERFACE_SOCKET_INPUT, nullptr);
        ASSERT_NE(io_input, nullptr);
        bNodeTreeInterfaceSocket *io_output = group_tree->tree_interface.add_socket(
            "Socket", "", socket_type->idname, NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
        ASSERT_NE(io_output, nullptr);

        io_input->structure_type = structure_type;
        io_output->structure_type = structure_type;
      }
    }
    /* Make sure the group node is updated. */
    BKE_ntree_update(*bmain, Span{group_tree, tree});
    ASSERT_EQ(BLI_listbase_count(&group_node->inputs), num_socket_types);
    ASSERT_EQ(BLI_listbase_count(&group_node->outputs), num_socket_types);

    /* Generate and execute all link operations for the tree type. */
    for (bNodeSocket &socket : group_node->inputs) {
      const nodes::SocketDeclaration *decl = socket.runtime->declaration;
      const StringRef socket_type = socket.idname;
      const StringRef structure_type = structure_type_names[int(decl->structure_type)];
      Vector<SocketLinkOperation> search_link_ops;
      {
        std::string msg = fmt::format("{}: Gather link operations for input socket type {} ({})",
                                      tree_idname,
                                      socket_type,
                                      structure_type);
        SCOPED_TRACE(msg);
        // std::cout << msg << std::endl;
        gather_socket_link_operations(*C, *tree, socket, search_link_ops);
      }
      for (const SocketLinkOperation &link_op : search_link_ops) {
        std::string msg = fmt::format("link operation {} for input socket type {} ({})",
                                      link_op.name,
                                      socket_type,
                                      structure_type);
        if (get_skipped_link_ops().contains({tree_idname, link_op.name, ""}) ||
            get_skipped_link_ops().contains({tree_idname, link_op.name, socket_type}))
        {
          std::cout << tree_idname << ": Skip " << msg << std::endl;
          continue;
        }
        SCOPED_TRACE(tree_idname + ": Execute " + msg);
        std::cout << tree_idname << ": Execute " << msg << std::endl;
        Vector<bNode *> added_nodes;
        nodes::LinkSearchOpParams params{*C, *tree, *group_node, socket, added_nodes};
        link_op.fn(params);

        /* Remove added nodes again (prevents deteriorating performance after executing many link
         * operations). */
        for (bNode *node : added_nodes) {
          bke::node_remove_node(bmain, *tree, *node, false);
        }
      }
    }
    for (bNodeSocket &socket : group_node->outputs) {
      const nodes::SocketDeclaration *decl = socket.runtime->declaration;
      const StringRef socket_type = socket.idname;
      const StringRef structure_type = structure_type_names[int(decl->structure_type)];
      Vector<SocketLinkOperation> search_link_ops;
      {
        std::string msg = fmt::format("{}: Gather link operations for output socket type {} ({})",
                                      tree_idname,
                                      socket_type,
                                      structure_type);
        SCOPED_TRACE(msg);
        // std::cout << msg << std::endl;
        gather_socket_link_operations(*C, *tree, socket, search_link_ops);
      }
      for (const SocketLinkOperation &link_op : search_link_ops) {
        std::string msg = fmt::format("link operation {} for output socket type {} ({})",
                                      link_op.name,
                                      socket_type,
                                      structure_type);
        if (get_skipped_link_ops().contains({tree_idname, link_op.name, ""}) ||
            get_skipped_link_ops().contains({tree_idname, link_op.name, socket_type}))
        {
          std::cout << tree_idname << ": Skip " << msg << std::endl;
          continue;
        }
        SCOPED_TRACE(tree_idname + ": Execute " + msg);
        std::cout << tree_idname << ": Execute " << msg << std::endl;
        Vector<bNode *> added_nodes;
        nodes::LinkSearchOpParams params{*C, *tree, *group_node, socket, added_nodes};
        link_op.fn(params);

        /* Remove added nodes again (prevents deteriorating performance after executing many link
         * operations). */
        for (bNode *node : added_nodes) {
          bke::node_remove_node(bmain, *tree, *node, false);
        }
      }
    }

    BKE_id_free(bmain, tree);
    BKE_id_free(bmain, group_tree);
  }

  CTX_free(C);
}

}  // namespace blender::ed::space_node::tests
