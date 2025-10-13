/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_sys_types.h"

#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"

#include "readfile.hh"

#include "versioning_common.hh"

// #include "CLG_log.h"
// static CLG_LogRef LOG = {"blend.doversion"};

/* The Mix mode of the Mix node previously assumed the alpha of the first input as opposed to
 * mixing the alpha as well. So we add a separate color node to get the alpha of the first input
 * and set it to the result using a set alpha node. */
static void do_version_mix_node_mix_mode_compositor(bNodeTree &node_tree, bNode &node)
{
  const NodeShaderMix *data = reinterpret_cast<NodeShaderMix *>(node.storage);
  if (data->data_type != SOCK_RGBA) {
    return;
  }

  if (data->blend_type != MA_RAMP_BLEND) {
    return;
  }

  bNodeSocket *first_input = blender::bke::node_find_socket(node, SOCK_IN, "A_Color");
  bNodeSocket *output = blender::bke::node_find_socket(node, SOCK_OUT, "Result_Color");

  /* Find the link going into the inputs of the node. */
  bNodeLink *first_link = nullptr;
  LISTBASE_FOREACH (bNodeLink *, link, &node_tree.links) {
    if (link->tosock == first_input) {
      first_link = link;
    }
  }

  bNode &separate_node = version_node_add_empty(node_tree, "CompositorNodeSeparateColor");
  separate_node.parent = node.parent;
  separate_node.location[0] = node.location[0] - 10.0f;
  separate_node.location[1] = node.location[1];
  NodeCMPCombSepColor *storage = MEM_callocN<NodeCMPCombSepColor>(__func__);
  storage->mode = CMP_NODE_COMBSEP_COLOR_RGB;
  separate_node.storage = storage;

  bNodeSocket &separate_input = version_node_add_socket(
      node_tree, separate_node, SOCK_IN, "NodeSocketColor", "Image");
  bNodeSocket &separate_alpha_output = version_node_add_socket(
      node_tree, separate_node, SOCK_OUT, "NodeSocketFloat", "Alpha");

  copy_v4_v4(separate_input.default_value_typed<bNodeSocketValueRGBA>()->value,
             first_input->default_value_typed<bNodeSocketValueRGBA>()->value);
  if (first_link) {
    version_node_add_link(
        node_tree, *first_link->fromnode, *first_link->fromsock, separate_node, separate_input);
  }

  bNode &set_alpha_node = version_node_add_empty(node_tree, "CompositorNodeSetAlpha");
  set_alpha_node.parent = node.parent;
  set_alpha_node.location[0] = node.location[0] - 10.0f;
  set_alpha_node.location[1] = node.location[1];
  set_alpha_node.storage = MEM_callocN<NodeCMPCombSepColor>(__func__);

  bNodeSocket &set_alpha_image_input = version_node_add_socket(
      node_tree, set_alpha_node, SOCK_IN, "NodeSocketColor", "Image");
  bNodeSocket &set_alpha_alpha_input = version_node_add_socket(
      node_tree, set_alpha_node, SOCK_IN, "NodeSocketFloat", "Alpha");
  bNodeSocket &set_alpha_type_input = version_node_add_socket(
      node_tree, set_alpha_node, SOCK_IN, "NodeSocketMenu", "Type");
  bNodeSocket &set_alpha_output = version_node_add_socket(
      node_tree, set_alpha_node, SOCK_OUT, "NodeSocketColor", "Image");

  set_alpha_type_input.default_value_typed<bNodeSocketValueMenu>()->value =
      CMP_NODE_SETALPHA_MODE_REPLACE_ALPHA;
  version_node_add_link(node_tree, node, *output, set_alpha_node, set_alpha_image_input);
  version_node_add_link(
      node_tree, separate_node, separate_alpha_output, set_alpha_node, set_alpha_alpha_input);

  LISTBASE_FOREACH_BACKWARD_MUTABLE (bNodeLink *, link, &node_tree.links) {
    if (link->fromsock == output && link->tonode != &set_alpha_node) {
      version_node_add_link(
          node_tree, set_alpha_node, set_alpha_output, *link->tonode, *link->tosock);
      blender::bke::node_remove_link(&node_tree, *link);
    }
  }
}

/* The Mix mode of the Mix node previously assumed the alpha of the first input as opposed to
 * mixing the alpha as well. So we add a separate color node to get the alpha of the first input
 * and set it to the result using a pair of separate and combine color nodes. */
static void do_version_mix_node_mix_mode_geometry(bNodeTree &node_tree, bNode &node)
{
  const NodeShaderMix *data = reinterpret_cast<NodeShaderMix *>(node.storage);
  if (data->data_type != SOCK_RGBA) {
    return;
  }

  if (data->blend_type != MA_RAMP_BLEND) {
    return;
  }

  bNodeSocket *first_input = blender::bke::node_find_socket(node, SOCK_IN, "A_Color");
  bNodeSocket *output = blender::bke::node_find_socket(node, SOCK_OUT, "Result_Color");

  /* Find the link going into the inputs of the node. */
  bNodeLink *first_link = nullptr;
  LISTBASE_FOREACH (bNodeLink *, link, &node_tree.links) {
    if (link->tosock == first_input) {
      first_link = link;
    }
  }

  bNode &separate_alpha_node = version_node_add_empty(node_tree, "FunctionNodeSeparateColor");
  separate_alpha_node.parent = node.parent;
  separate_alpha_node.location[0] = node.location[0] - 10.0f;
  separate_alpha_node.location[1] = node.location[1];
  NodeCombSepColor *separate_alpha_storage = MEM_callocN<NodeCombSepColor>(__func__);
  separate_alpha_storage->mode = NODE_COMBSEP_COLOR_RGB;
  separate_alpha_node.storage = separate_alpha_storage;

  bNodeSocket &separate_alpha_input = version_node_add_socket(
      node_tree, separate_alpha_node, SOCK_IN, "NodeSocketColor", "Color");
  bNodeSocket &separate_alpha_output = version_node_add_socket(
      node_tree, separate_alpha_node, SOCK_OUT, "NodeSocketFloat", "Alpha");

  copy_v4_v4(separate_alpha_input.default_value_typed<bNodeSocketValueRGBA>()->value,
             first_input->default_value_typed<bNodeSocketValueRGBA>()->value);
  if (first_link) {
    version_node_add_link(node_tree,
                          *first_link->fromnode,
                          *first_link->fromsock,
                          separate_alpha_node,
                          separate_alpha_input);
  }

  bNode &separate_color_node = version_node_add_empty(node_tree, "FunctionNodeSeparateColor");
  separate_color_node.parent = node.parent;
  separate_color_node.location[0] = node.location[0] - 10.0f;
  separate_color_node.location[1] = node.location[1];
  NodeCombSepColor *separate_color_storage = MEM_callocN<NodeCombSepColor>(__func__);
  separate_color_storage->mode = NODE_COMBSEP_COLOR_RGB;
  separate_color_node.storage = separate_color_storage;

  bNodeSocket &separate_color_input = version_node_add_socket(
      node_tree, separate_color_node, SOCK_IN, "NodeSocketColor", "Color");
  bNodeSocket &separate_color_red_output = version_node_add_socket(
      node_tree, separate_color_node, SOCK_OUT, "NodeSocketFloat", "Red");
  bNodeSocket &separate_color_green_output = version_node_add_socket(
      node_tree, separate_color_node, SOCK_OUT, "NodeSocketFloat", "Green");
  bNodeSocket &separate_color_blue_output = version_node_add_socket(
      node_tree, separate_color_node, SOCK_OUT, "NodeSocketFloat", "Blue");

  version_node_add_link(node_tree, node, *output, separate_color_node, separate_color_input);

  bNode &combine_color_node = version_node_add_empty(node_tree, "FunctionNodeCombineColor");
  combine_color_node.parent = node.parent;
  combine_color_node.location[0] = node.location[0] - 10.0f;
  combine_color_node.location[1] = node.location[1];
  NodeCombSepColor *combine_color_storage = MEM_callocN<NodeCombSepColor>(__func__);
  combine_color_storage->mode = NODE_COMBSEP_COLOR_RGB;
  combine_color_node.storage = combine_color_storage;

  bNodeSocket &combine_color_red_input = version_node_add_socket(
      node_tree, combine_color_node, SOCK_IN, "NodeSocketFloat", "Red");
  bNodeSocket &combine_color_green_input = version_node_add_socket(
      node_tree, combine_color_node, SOCK_IN, "NodeSocketFloat", "Green");
  bNodeSocket &combine_color_blue_input = version_node_add_socket(
      node_tree, combine_color_node, SOCK_IN, "NodeSocketFloat", "Blue");
  bNodeSocket &combine_color_alpha_input = version_node_add_socket(
      node_tree, combine_color_node, SOCK_IN, "NodeSocketFloat", "Alpha");
  bNodeSocket &combine_color_output = version_node_add_socket(
      node_tree, combine_color_node, SOCK_OUT, "NodeSocketColor", "Color");

  version_node_add_link(node_tree,
                        separate_color_node,
                        separate_color_red_output,
                        combine_color_node,
                        combine_color_red_input);
  version_node_add_link(node_tree,
                        separate_color_node,
                        separate_color_green_output,
                        combine_color_node,
                        combine_color_green_input);
  version_node_add_link(node_tree,
                        separate_color_node,
                        separate_color_blue_output,
                        combine_color_node,
                        combine_color_blue_input);
  version_node_add_link(node_tree,
                        separate_alpha_node,
                        separate_alpha_output,
                        combine_color_node,
                        combine_color_alpha_input);

  LISTBASE_FOREACH_BACKWARD_MUTABLE (bNodeLink *, link, &node_tree.links) {
    if (link->fromsock == output && link->tonode != &separate_color_node) {
      version_node_add_link(
          node_tree, combine_color_node, combine_color_output, *link->tonode, *link->tosock);
      blender::bke::node_remove_link(&node_tree, *link);
    }
  }
}

/* The Start Frame, Cyclic, and Offset options were removed from the Image node and a frame input
 * was added. So we reproduce the hold behavior by subtracting the start frame minus 1, modulo with
 * the number of frames if cyclic, and add the offset. */
static void do_version_image_node_frame(bNodeTree *node_tree, bNode *node)
{
  /* Already versioned. */
  if (blender::bke::node_find_socket(*node, SOCK_IN, "Frame")) {
    return;
  }

  bNodeSocket *frame_input = blender::bke::node_add_static_socket(
      *node_tree, *node, SOCK_IN, SOCK_INT, PROP_NONE, "Frame", "Frame");
  frame_input->display_shape = SOCK_DISPLAY_SHAPE_LINE;
  frame_input->flag |= SOCK_HIDE_VALUE;

  /* Not animated. */
  Image *image = reinterpret_cast<Image *>(node->id);
  if (!image || !ELEM(image->source, IMA_SRC_SEQUENCE, IMA_SRC_MOVIE)) {
    return;
  }

  /* The frame is used as is, no need to version anything. */
  ImageUser &image_user = *static_cast<ImageUser *>(node->storage);
  if (image_user.sfra == 1 && !image_user.cycl && image_user.offset == 0) {
    return;
  }

  /* Get the frame number. */
  bNode *frame_node = blender::bke::node_add_node(nullptr, *node_tree, "CompositorNodeSceneTime");
  frame_node->flag |= NODE_COLLAPSED;
  frame_node->parent = node->parent;
  frame_node->location[0] = node->location[0] - 10.0f;
  frame_node->location[1] = node->location[1];

  bNodeSocket *frame_output = blender::bke::node_find_socket(*frame_node, SOCK_OUT, "Frame");

  bNode *last_node = frame_node;
  bNodeSocket *last_output = frame_output;

  /* Subtract the start frame if not 1. */
  if (image_user.sfra != 1) {
    bNode *start_frame_node = blender::bke::node_add_node(nullptr, *node_tree, "ShaderNodeMath");
    start_frame_node->custom1 = NODE_MATH_SUBTRACT;
    start_frame_node->flag |= NODE_COLLAPSED;
    start_frame_node->parent = node->parent;
    start_frame_node->location[0] = frame_node->location[0];
    start_frame_node->location[1] = frame_node->location[1];

    bNodeSocket *start_frame_a_input = blender::bke::node_find_socket(
        *start_frame_node, SOCK_IN, "Value");
    bNodeSocket *start_frame_b_input = blender::bke::node_find_socket(
        *start_frame_node, SOCK_IN, "Value_001");
    bNodeSocket *start_frame_output = blender::bke::node_find_socket(
        *start_frame_node, SOCK_OUT, "Value");

    static_cast<bNodeSocketValueFloat *>(start_frame_b_input->default_value)->value =
        image_user.sfra - 1;

    version_node_add_link(
        *node_tree, *frame_node, *frame_output, *start_frame_node, *start_frame_a_input);
    last_node = start_frame_node;
    last_output = start_frame_output;
  }

  /* Modulo with the length of the animation if Cyclic is enabled. */
  if (image_user.cycl) {
    bNode *modulo_node = blender::bke::node_add_node(nullptr, *node_tree, "ShaderNodeMath");
    modulo_node->custom1 = NODE_MATH_MODULO;
    modulo_node->flag |= NODE_COLLAPSED;
    modulo_node->parent = node->parent;
    modulo_node->location[0] = last_node->location[0];
    modulo_node->location[1] = last_node->location[1];

    bNodeSocket *modulo_a_input = blender::bke::node_find_socket(*modulo_node, SOCK_IN, "Value");
    bNodeSocket *modulo_b_input = blender::bke::node_find_socket(
        *modulo_node, SOCK_IN, "Value_001");
    bNodeSocket *modulo_output = blender::bke::node_find_socket(*modulo_node, SOCK_OUT, "Value");

    static_cast<bNodeSocketValueFloat *>(modulo_b_input->default_value)->value = image_user.frames;

    version_node_add_link(*node_tree, *last_node, *last_output, *modulo_node, *modulo_a_input);
    last_node = modulo_node;
    last_output = modulo_output;
  }

  /* Add the offset if not zero. */
  if (image_user.offset != 0) {
    bNode *offset_node = blender::bke::node_add_node(nullptr, *node_tree, "ShaderNodeMath");
    offset_node->custom1 = NODE_MATH_ADD;
    offset_node->flag |= NODE_COLLAPSED;
    offset_node->parent = node->parent;
    offset_node->location[0] = last_node->location[0];
    offset_node->location[1] = last_node->location[1];

    bNodeSocket *offset_a_input = blender::bke::node_find_socket(*offset_node, SOCK_IN, "Value");
    bNodeSocket *offset_b_input = blender::bke::node_find_socket(
        *offset_node, SOCK_IN, "Value_001");
    bNodeSocket *offset_output = blender::bke::node_find_socket(*offset_node, SOCK_OUT, "Value");

    static_cast<bNodeSocketValueFloat *>(offset_b_input->default_value)->value = image_user.offset;

    version_node_add_link(*node_tree, *last_node, *last_output, *offset_node, *offset_a_input);
    last_node = offset_node;
    last_output = offset_output;
  }
  version_node_add_link(*node_tree, *last_node, *last_output, *node, *frame_input);
}

void do_versions_after_linking_510(FileData * /*fd*/, Main *bmain)
{
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 501, 3)) {
    FOREACH_NODETREE_BEGIN (bmain, node_tree, id) {
      if (node_tree->type == NTREE_COMPOSIT) {
        LISTBASE_FOREACH_MUTABLE (bNode *, node, &node_tree->nodes) {
          if (node->type_legacy == CMP_NODE_IMAGE) {
            do_version_image_node_frame(node_tree, node);
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

void blo_do_versions_510(FileData * /*fd*/, Library * /*lib*/, Main *bmain)
{
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 501, 1)) {
    FOREACH_NODETREE_BEGIN (bmain, node_tree, id) {
      if (node_tree->type == NTREE_COMPOSIT) {
        LISTBASE_FOREACH (bNode *, node, &node_tree->nodes) {
          if (node->type_legacy == SH_NODE_MIX) {
            do_version_mix_node_mix_mode_compositor(*node_tree, *node);
          }
        }
      }
      else if (node_tree->type == NTREE_GEOMETRY) {
        LISTBASE_FOREACH (bNode *, node, &node_tree->nodes) {
          if (node->type_legacy == SH_NODE_MIX) {
            do_version_mix_node_mix_mode_geometry(*node_tree, *node);
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
