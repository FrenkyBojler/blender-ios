/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase_iterator.hh"
#include "BLI_path_utils.hh"

#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"

#include "BLO_readfile.hh"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "NOD_socket_search_link.hh"

#include "CLG_log.h"

#include "tests/blendfile_loading_base_test.h"

#include "testing/testing.h"

namespace blender::bke::tests {

class NodeLinkDragTest : public BlendfileLoadingBaseTest {
 public:
  // Main *bmain;

  // static void SetUpTestSuite()
  // {
  //   CLG_init();
  //   BKE_idtype_init();
  // }

  // static void TearDownTestSuite()
  // {
  //   CLG_exit();
  // }

  void SetUp() override
  {
    BlendfileLoadingBaseTest::SetUp();
  }

  void TearDown() override
  {
    BlendfileLoadingBaseTest::TearDown();
  }
};

static SpaceNode *find_space_node(const Main &bmain, const StringRef tree_idname)
{
  for (bScreen &screen : bmain.screens) {
    for (ScrArea &area : screen.areabase) {
      for (SpaceLink &sl : area.spacedata) {
        if (sl.spacetype == SPACE_NODE) {
          SpaceNode &snode = reinterpret_cast<SpaceNode &>(sl);
          if (tree_idname == snode.tree_idname) {
            return &snode;
          }
        }
      }
    }
  }
  return nullptr;
}

TEST_F(NodeLinkDragTest, NodeLinkDrag)
{
  /* Load the test blend file. */
  if (!blendfile_load("node_group" SEP_STR "link_drag_search.blend")) {
    return;
  }

  SpaceNode *snode = find_space_node(*bfile->main, )

  // Main *bmain;
  // nodes::GatherLinkSearchOpParams params{*node_type, snode, node_tree, socket,
  // search_link_ops}; node_type->gather_link_search_ops(params);
}

}  // namespace blender::bke::tests
