/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"

#include "BLI_listbase.h"
#include "BLI_sys_types.h"

#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"

#include "readfile.hh"

#include "versioning_common.hh"

// #include "CLG_log.h"
// static CLG_LogRef LOG = {"blo.readfile.doversion"};

static void do_version_split_node_rotation(bNodeTree *node_tree, bNode *node)
{
  bNodeSocket *factor_input = blender::bke::node_find_socket(*node, SOCK_IN, "Factor");
  float fac = factor_input->default_value_typed<bNodeSocketValueFloat>()->value;
  printf("factor from versioning: %f\n", fac);

  CMPNodeSplitAxis axis = static_cast<CMPNodeSplitAxis>(node->custom2);
  // todo(habib): relative to pixel (Idea: image info node + divide by relevant dimension/axis)
}

void do_versions_after_linking_500(FileData * /*fd*/, Main * /*bmain*/)
{
  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

void blo_do_versions_500(FileData * /*fd*/, Library * /*lib*/, Main * /*bmain*/)
{
  // Todo(habib): proper versioning
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 405, 86)) {
    FOREACH_NODETREE_BEGIN (bmain, node_tree, id) {
      if (node_tree->type == NTREE_COMPOSIT) {
        LISTBASE_FOREACH (bNode *, node, &node_tree->nodes) {
          if (node->type_legacy == CMP_NODE_SPLIT) {
            printf("versioning split node...\n");
            do_version_split_node_rotation(node_tree, node);
            printf("\n");
          }
        }
      }
    }
    FOREACH_NODETREE_END;
  }

  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}
