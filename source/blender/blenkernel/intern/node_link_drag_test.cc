/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_path_utils.hh"

#include "BKE_addon.h"
#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_keyconfig.h"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_screen.hh"
#include "BKE_shader_fx.hh"

#include "BLO_readfile.hh"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "NOD_composite.hh"
#include "NOD_geometry.hh"
#include "NOD_shader.h"
#include "NOD_socket_search_link.hh"

#include "ED_space_api.hh"

#include "WM_api.hh"
#include "gizmo/WM_gizmo_types.hh"
#include "wm.hh"
#include "wm_event_system.hh"

#include "tests/blendfile_loading_base_test.h"

#include "testing/testing.h"

namespace blender::bke::tests {

class NodeLinkDragTest : public BlendfileLoadingBaseTest {
 public:
  static void SetUpTestCase()
  {
    BlendfileLoadingBaseTest::SetUpTestCase();
    BKE_shaderfx_init();
    ED_spacetypes_init();
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

  const Vector<std::string> tree_idnames = {
      ntreeType_Composite->idname, ntreeType_Geometry->idname, ntreeType_Shader->idname};
  const Vector<std::string> group_node_idnames = {ntreeType_Composite->group_idname,
                                                  ntreeType_Geometry->group_idname,
                                                  ntreeType_Shader->group_idname};

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
    for (const bNodeSocketType *socket_type : node_socket_types_get()) {
      if (tree_type->valid_socket_type &&
          !tree_type->valid_socket_type(const_cast<bNodeTreeType *>(tree_type),
                                        const_cast<bNodeSocketType *>(socket_type)))
      {
        continue;
      }

      ++num_socket_types;
      group_tree->tree_interface.add_socket(
          "Socket", "", socket_type->idname, NODE_INTERFACE_SOCKET_INPUT, nullptr);
      group_tree->tree_interface.add_socket(
          "Socket", "", socket_type->idname, NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
    }
    /* Make sure the group node is updated. */
    BKE_ntree_update(*bmain, Span{group_tree, tree});

    ASSERT_EQ(BLI_listbase_count(&group_node->inputs), num_socket_types);
    ASSERT_EQ(BLI_listbase_count(&group_node->outputs), num_socket_types);
    // for (bNodeSocket *socket : group_node->inputs) {
    //   gather_socket_link_operations(*C, *tree, )
    // }

    BKE_id_free(bmain, tree);
    BKE_id_free(bmain, group_tree);
  }

  CTX_free(C);
}

}  // namespace blender::bke::tests
