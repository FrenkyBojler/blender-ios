/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include "BLI_math_color.h"
#include "node_texture_util.hh"
#include "node_util.hh"

namespace blender {

static bke::bNodeSocketTemplate inputs[] = {
    {.type = SOCK_FLOAT,
     .name = N_("Red"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = 0.0f,
     .max = 1.0f,
     .subtype = PROP_FACTOR},
    {.type = SOCK_FLOAT,
     .name = N_("Green"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = 0.0f,
     .max = 1.0f,
     .subtype = PROP_FACTOR},
    {.type = SOCK_FLOAT,
     .name = N_("Blue"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = 0.0f,
     .max = 1.0f,
     .subtype = PROP_FACTOR},
    {.type = SOCK_FLOAT,
     .name = N_("Alpha"),
     .val1 = 1.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = 0.0f,
     .max = 1.0f,
     .subtype = PROP_FACTOR},
    {.type = -1, .name = ""},
};
static bke::bNodeSocketTemplate outputs[] = {
    {.type = SOCK_RGBA, .name = N_("Color")},
    {.type = -1, .name = ""},
};

static void colorfn(float *out, TexParams *p, bNode *node, bNodeStack **in, short thread)
{
  int i;
  for (i = 0; i < 4; i++) {
    out[i] = tex_input_value(in[i], p, thread);
  }
  /* Apply color space if required. */
  switch (node->custom1) {
    case NODE_COMBSEP_COLOR_RGB: {
      /* Pass */
      break;
    }
    case NODE_COMBSEP_COLOR_HSV: {
      hsv_to_rgb_v(out, out);
      break;
    }
    case NODE_COMBSEP_COLOR_HSL: {
      hsl_to_rgb_v(out, out);
      break;
    }
    default: {
      BLI_assert_unreachable();
      break;
    }
  }
}

static void update(bNodeTree * /*ntree*/, bNode *node)
{
  node_combsep_color_label(&node->inputs, NodeCombSepColorMode(node->custom1));
}

static void exec(void *data,
                 int /*thread*/,
                 bNode *node,
                 bNodeExecData *execdata,
                 bNodeStack **in,
                 bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &colorfn, static_cast<TexCallData *>(data));
}

void register_node_type_tex_combine_color()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeCombineColor", TEX_NODE_COMBINE_COLOR);
  ntype.ui_name = "Combine Color";
  ntype.enum_name_legacy = "COMBINE_COLOR";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  bke::node_type_socket_templates(&ntype, inputs, outputs);
  ntype.exec_fn = exec;
  ntype.updatefunc = update;

  bke::node_register_type(ntype);
}

}  // namespace blender
