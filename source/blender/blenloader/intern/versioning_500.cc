/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_sys_types.h"

#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"

#include "readfile.hh"

#include "versioning_common.hh"

// #include "CLG_log.h"
// static CLG_LogRef LOG = {"blo.readfile.doversion"};

static void versioning_replace_legacy_combined_and_separate_color_nodes(bNodeTree *ntree)
{
  /* In geometry nodes, replace shader combine/separate color nodes with function nodes */
  if (ntree->type == NTREE_GEOMETRY) {
    version_node_input_socket_name(ntree, SH_NODE_COMBRGB_LEGACY, "R", "Red");
    version_node_input_socket_name(ntree, SH_NODE_COMBRGB_LEGACY, "G", "Green");
    version_node_input_socket_name(ntree, SH_NODE_COMBRGB_LEGACY, "B", "Blue");
    version_node_output_socket_name(ntree, SH_NODE_COMBRGB_LEGACY, "Image", "Color");

    version_node_output_socket_name(ntree, SH_NODE_SEPRGB_LEGACY, "R", "Red");
    version_node_output_socket_name(ntree, SH_NODE_SEPRGB_LEGACY, "G", "Green");
    version_node_output_socket_name(ntree, SH_NODE_SEPRGB_LEGACY, "B", "Blue");
    version_node_input_socket_name(ntree, SH_NODE_SEPRGB_LEGACY, "Image", "Color");

    LISTBASE_FOREACH (bNode *, node, &ntree->nodes) {
      switch (node->type_legacy) {
        case SH_NODE_COMBRGB_LEGACY: {
          node->type_legacy = FN_NODE_COMBINE_COLOR;
          NodeCombSepColor *storage = MEM_callocN<NodeCombSepColor>(__func__);
          storage->mode = NODE_COMBSEP_COLOR_RGB;
          STRNCPY(node->idname, "FunctionNodeCombineColor");
          node->storage = storage;
          break;
        }
        case SH_NODE_SEPRGB_LEGACY: {
          node->type_legacy = FN_NODE_SEPARATE_COLOR;
          NodeCombSepColor *storage = MEM_callocN<NodeCombSepColor>(__func__);
          storage->mode = NODE_COMBSEP_COLOR_RGB;
          STRNCPY(node->idname, "FunctionNodeSeparateColor");
          node->storage = storage;
          break;
        }
      }
    }
  }

  /* In compositing nodes, replace combine/separate RGBA/HSVA/YCbCrA/YCCA nodes with
   * combine/separate color */
  if (ntree->type == NTREE_COMPOSIT) {
    version_node_input_socket_name(ntree, CMP_NODE_COMBRGBA_LEGACY, "R", "Red");
    version_node_input_socket_name(ntree, CMP_NODE_COMBRGBA_LEGACY, "G", "Green");
    version_node_input_socket_name(ntree, CMP_NODE_COMBRGBA_LEGACY, "B", "Blue");
    version_node_input_socket_name(ntree, CMP_NODE_COMBRGBA_LEGACY, "A", "Alpha");

    version_node_input_socket_name(ntree, CMP_NODE_COMBHSVA_LEGACY, "H", "Red");
    version_node_input_socket_name(ntree, CMP_NODE_COMBHSVA_LEGACY, "S", "Green");
    version_node_input_socket_name(ntree, CMP_NODE_COMBHSVA_LEGACY, "V", "Blue");
    version_node_input_socket_name(ntree, CMP_NODE_COMBHSVA_LEGACY, "A", "Alpha");

    version_node_input_socket_name(ntree, CMP_NODE_COMBYCCA_LEGACY, "Y", "Red");
    version_node_input_socket_name(ntree, CMP_NODE_COMBYCCA_LEGACY, "Cb", "Green");
    version_node_input_socket_name(ntree, CMP_NODE_COMBYCCA_LEGACY, "Cr", "Blue");
    version_node_input_socket_name(ntree, CMP_NODE_COMBYCCA_LEGACY, "A", "Alpha");

    version_node_input_socket_name(ntree, CMP_NODE_COMBYUVA_LEGACY, "Y", "Red");
    version_node_input_socket_name(ntree, CMP_NODE_COMBYUVA_LEGACY, "U", "Green");
    version_node_input_socket_name(ntree, CMP_NODE_COMBYUVA_LEGACY, "V", "Blue");
    version_node_input_socket_name(ntree, CMP_NODE_COMBYUVA_LEGACY, "A", "Alpha");

    version_node_output_socket_name(ntree, CMP_NODE_SEPRGBA_LEGACY, "R", "Red");
    version_node_output_socket_name(ntree, CMP_NODE_SEPRGBA_LEGACY, "G", "Green");
    version_node_output_socket_name(ntree, CMP_NODE_SEPRGBA_LEGACY, "B", "Blue");
    version_node_output_socket_name(ntree, CMP_NODE_SEPRGBA_LEGACY, "A", "Alpha");

    version_node_output_socket_name(ntree, CMP_NODE_SEPHSVA_LEGACY, "H", "Red");
    version_node_output_socket_name(ntree, CMP_NODE_SEPHSVA_LEGACY, "S", "Green");
    version_node_output_socket_name(ntree, CMP_NODE_SEPHSVA_LEGACY, "V", "Blue");
    version_node_output_socket_name(ntree, CMP_NODE_SEPHSVA_LEGACY, "A", "Alpha");

    version_node_output_socket_name(ntree, CMP_NODE_SEPYCCA_LEGACY, "Y", "Red");
    version_node_output_socket_name(ntree, CMP_NODE_SEPYCCA_LEGACY, "Cb", "Green");
    version_node_output_socket_name(ntree, CMP_NODE_SEPYCCA_LEGACY, "Cr", "Blue");
    version_node_output_socket_name(ntree, CMP_NODE_SEPYCCA_LEGACY, "A", "Alpha");

    version_node_output_socket_name(ntree, CMP_NODE_SEPYUVA_LEGACY, "Y", "Red");
    version_node_output_socket_name(ntree, CMP_NODE_SEPYUVA_LEGACY, "U", "Green");
    version_node_output_socket_name(ntree, CMP_NODE_SEPYUVA_LEGACY, "V", "Blue");
    version_node_output_socket_name(ntree, CMP_NODE_SEPYUVA_LEGACY, "A", "Alpha");

    LISTBASE_FOREACH (bNode *, node, &ntree->nodes) {
      switch (node->type_legacy) {
        case CMP_NODE_COMBRGBA_LEGACY: {
          node->type_legacy = CMP_NODE_COMBINE_COLOR;
          NodeCMPCombSepColor *storage = MEM_callocN<NodeCMPCombSepColor>(__func__);
          storage->mode = CMP_NODE_COMBSEP_COLOR_RGB;
          STRNCPY(node->idname, "CompositorNodeCombineColor");
          node->storage = storage;
          break;
        }
        case CMP_NODE_COMBHSVA_LEGACY: {
          node->type_legacy = CMP_NODE_COMBINE_COLOR;
          NodeCMPCombSepColor *storage = MEM_callocN<NodeCMPCombSepColor>(__func__);
          storage->mode = CMP_NODE_COMBSEP_COLOR_HSV;
          STRNCPY(node->idname, "CompositorNodeCombineColor");
          node->storage = storage;
          break;
        }
        case CMP_NODE_COMBYCCA_LEGACY: {
          node->type_legacy = CMP_NODE_COMBINE_COLOR;
          NodeCMPCombSepColor *storage = MEM_callocN<NodeCMPCombSepColor>(__func__);
          storage->mode = CMP_NODE_COMBSEP_COLOR_YCC;
          storage->ycc_mode = node->custom1;
          STRNCPY(node->idname, "CompositorNodeCombineColor");
          node->storage = storage;
          break;
        }
        case CMP_NODE_COMBYUVA_LEGACY: {
          node->type_legacy = CMP_NODE_COMBINE_COLOR;
          NodeCMPCombSepColor *storage = MEM_callocN<NodeCMPCombSepColor>(__func__);
          storage->mode = CMP_NODE_COMBSEP_COLOR_YUV;
          STRNCPY(node->idname, "CompositorNodeCombineColor");
          node->storage = storage;
          break;
        }
        case CMP_NODE_SEPRGBA_LEGACY: {
          node->type_legacy = CMP_NODE_SEPARATE_COLOR;
          NodeCMPCombSepColor *storage = MEM_callocN<NodeCMPCombSepColor>(__func__);
          storage->mode = CMP_NODE_COMBSEP_COLOR_RGB;
          STRNCPY(node->idname, "CompositorNodeSeparateColor");
          node->storage = storage;
          break;
        }
        case CMP_NODE_SEPHSVA_LEGACY: {
          node->type_legacy = CMP_NODE_SEPARATE_COLOR;
          NodeCMPCombSepColor *storage = MEM_callocN<NodeCMPCombSepColor>(__func__);
          storage->mode = CMP_NODE_COMBSEP_COLOR_HSV;
          STRNCPY(node->idname, "CompositorNodeSeparateColor");
          node->storage = storage;
          break;
        }
        case CMP_NODE_SEPYCCA_LEGACY: {
          node->type_legacy = CMP_NODE_SEPARATE_COLOR;
          NodeCMPCombSepColor *storage = MEM_callocN<NodeCMPCombSepColor>(__func__);
          storage->mode = CMP_NODE_COMBSEP_COLOR_YCC;
          storage->ycc_mode = node->custom1;
          STRNCPY(node->idname, "CompositorNodeSeparateColor");
          node->storage = storage;
          break;
        }
        case CMP_NODE_SEPYUVA_LEGACY: {
          node->type_legacy = CMP_NODE_SEPARATE_COLOR;
          NodeCMPCombSepColor *storage = MEM_callocN<NodeCMPCombSepColor>(__func__);
          storage->mode = CMP_NODE_COMBSEP_COLOR_YUV;
          STRNCPY(node->idname, "CompositorNodeSeparateColor");
          node->storage = storage;
          break;
        }
      }
    }
  }

  /* In texture nodes, replace combine/separate RGBA with combine/separate color */
  if (ntree->type == NTREE_TEXTURE) {
    LISTBASE_FOREACH (bNode *, node, &ntree->nodes) {
      switch (node->type_legacy) {
        case TEX_NODE_COMPOSE_LEGACY: {
          node->type_legacy = TEX_NODE_COMBINE_COLOR;
          node->custom1 = NODE_COMBSEP_COLOR_RGB;
          STRNCPY(node->idname, "TextureNodeCombineColor");
          break;
        }
        case TEX_NODE_DECOMPOSE_LEGACY: {
          node->type_legacy = TEX_NODE_SEPARATE_COLOR;
          node->custom1 = NODE_COMBSEP_COLOR_RGB;
          STRNCPY(node->idname, "TextureNodeSeparateColor");
          break;
        }
      }
    }
  }

  /* In shader nodes, replace combine/separate RGB/HSV with combine/separate color */
  if (ntree->type == NTREE_SHADER) {
    version_node_input_socket_name(ntree, SH_NODE_COMBRGB_LEGACY, "R", "Red");
    version_node_input_socket_name(ntree, SH_NODE_COMBRGB_LEGACY, "G", "Green");
    version_node_input_socket_name(ntree, SH_NODE_COMBRGB_LEGACY, "B", "Blue");
    version_node_output_socket_name(ntree, SH_NODE_COMBRGB_LEGACY, "Image", "Color");

    version_node_input_socket_name(ntree, SH_NODE_COMBHSV_LEGACY, "H", "Red");
    version_node_input_socket_name(ntree, SH_NODE_COMBHSV_LEGACY, "S", "Green");
    version_node_input_socket_name(ntree, SH_NODE_COMBHSV_LEGACY, "V", "Blue");

    version_node_output_socket_name(ntree, SH_NODE_SEPRGB_LEGACY, "R", "Red");
    version_node_output_socket_name(ntree, SH_NODE_SEPRGB_LEGACY, "G", "Green");
    version_node_output_socket_name(ntree, SH_NODE_SEPRGB_LEGACY, "B", "Blue");
    version_node_input_socket_name(ntree, SH_NODE_SEPRGB_LEGACY, "Image", "Color");

    version_node_output_socket_name(ntree, SH_NODE_SEPHSV_LEGACY, "H", "Red");
    version_node_output_socket_name(ntree, SH_NODE_SEPHSV_LEGACY, "S", "Green");
    version_node_output_socket_name(ntree, SH_NODE_SEPHSV_LEGACY, "V", "Blue");

    LISTBASE_FOREACH (bNode *, node, &ntree->nodes) {
      switch (node->type_legacy) {
        case SH_NODE_COMBRGB_LEGACY: {
          node->type_legacy = SH_NODE_COMBINE_COLOR;
          NodeCombSepColor *storage = MEM_callocN<NodeCombSepColor>(__func__);
          storage->mode = NODE_COMBSEP_COLOR_RGB;
          STRNCPY(node->idname, "ShaderNodeCombineColor");
          node->storage = storage;
          break;
        }
        case SH_NODE_COMBHSV_LEGACY: {
          node->type_legacy = SH_NODE_COMBINE_COLOR;
          NodeCombSepColor *storage = MEM_callocN<NodeCombSepColor>(__func__);
          storage->mode = NODE_COMBSEP_COLOR_HSV;
          STRNCPY(node->idname, "ShaderNodeCombineColor");
          node->storage = storage;
          break;
        }
        case SH_NODE_SEPRGB_LEGACY: {
          node->type_legacy = SH_NODE_SEPARATE_COLOR;
          NodeCombSepColor *storage = MEM_callocN<NodeCombSepColor>(__func__);
          storage->mode = NODE_COMBSEP_COLOR_RGB;
          STRNCPY(node->idname, "ShaderNodeSeparateColor");
          node->storage = storage;
          break;
        }
        case SH_NODE_SEPHSV_LEGACY: {
          node->type_legacy = SH_NODE_SEPARATE_COLOR;
          NodeCombSepColor *storage = MEM_callocN<NodeCombSepColor>(__func__);
          storage->mode = NODE_COMBSEP_COLOR_HSV;
          STRNCPY(node->idname, "ShaderNodeSeparateColor");
          node->storage = storage;
          break;
        }
      }
    }
  }
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

void blo_do_versions_500(FileData * /*fd*/, Library * /*lib*/, Main *bmain)
{
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 500, 1)) {
    FOREACH_NODETREE_BEGIN (bmain, ntree, id) {
      versioning_replace_legacy_combined_and_separate_color_nodes(ntree);
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
