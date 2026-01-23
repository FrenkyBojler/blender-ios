/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"

#include "NOD_socket_search_link.hh"

#include "CLG_log.h"

#include "testing/testing.h"

namespace blender::bke::tests {

class NodeLinkDragTest : public testing::Test {
 public:
  Main *bmain;

  static void SetUpTestSuite()
  {
    CLG_init();
    BKE_idtype_init();
  }

  static void TearDownTestSuite()
  {
    CLG_exit();
  }

  void SetUp() override
  {
    bmain = BKE_main_new();
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }
};

TEST_F(NodeLinkDragTest, NodeLinkDrag)
{
  nodes::GatherLinkSearchOpParams params{*node_type, snode, node_tree, socket, search_link_ops};
  node_type->gather_link_search_ops(params);
}

}  // namespace blender::bke::tests
