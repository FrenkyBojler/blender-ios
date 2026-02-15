/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include "BKE_material.hh"
#include "BLI_math_vector.h"
#include "DNA_material_types.h"
#include "node_texture_util.hh"

#include <cmath>

namespace blender {

static bke::bNodeSocketTemplate inputs[] = {
    {.type = SOCK_RGBA,
     .name = N_("Bricks 1"),
     .val1 = 0.596f,
     .val2 = 0.282f,
     .val3 = 0.0f,
     .val4 = 1.0f},
    {.type = SOCK_RGBA,
     .name = N_("Bricks 2"),
     .val1 = 0.632f,
     .val2 = 0.504f,
     .val3 = 0.05f,
     .val4 = 1.0f},
    {.type = SOCK_RGBA,
     .name = N_("Mortar"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 1.0f},
    {.type = SOCK_FLOAT,
     .name = N_("Thickness"),
     .val1 = 0.02f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = 0.0f,
     .max = 1.0f,
     .subtype = PROP_UNSIGNED},
    {.type = SOCK_FLOAT,
     .name = N_("Bias"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = -1.0f,
     .max = 1.0f,
     .subtype = PROP_NONE},
    {.type = SOCK_FLOAT,
     .name = N_("Brick Width"),
     .val1 = 0.5f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = 0.001f,
     .max = 99.0f,
     .subtype = PROP_UNSIGNED},
    {.type = SOCK_FLOAT,
     .name = N_("Row Height"),
     .val1 = 0.25f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = 0.001f,
     .max = 99.0f,
     .subtype = PROP_UNSIGNED},
    {.type = -1, .name = ""},
};
static bke::bNodeSocketTemplate outputs[] = {
    {.type = SOCK_RGBA, .name = N_("Color")},
    {.type = -1, .name = ""},
};

static void init(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom3 = 0.5; /* offset */
  node->custom4 = 1.0; /* squash */
}

static float noise(int n) /* fast integer noise */
{
  int nn;
  n = (n >> 13) ^ n;
  nn = (n * (n * n * 60493 + 19990303) + 1376312589) & 0x7fffffff;
  return 0.5f * (float(nn) / 1073741824.0f);
}

static void colorfn(float *out, TexParams *p, bNode *node, bNodeStack **in, short thread)
{
  const float *co = p->co;

  float x = co[0];
  float y = co[1];

  int bricknum, rownum;
  float offset = 0;
  float ins_x, ins_y;
  float tint;

  float bricks1[4];
  float bricks2[4];
  float mortar[4];

  float mortar_thickness = tex_input_value(in[3], p, thread);
  float bias = tex_input_value(in[4], p, thread);
  float brick_width = tex_input_value(in[5], p, thread);
  float row_height = tex_input_value(in[6], p, thread);

  tex_input_rgba(bricks1, in[0], p, thread);
  tex_input_rgba(bricks2, in[1], p, thread);
  tex_input_rgba(mortar, in[2], p, thread);

  rownum = int(floor(y / row_height));

  if (node->custom1 && node->custom2) {
    brick_width *= (rownum % node->custom2) ? 1.0f : node->custom4;        /* squash */
    offset = (rownum % node->custom1) ? 0 : (brick_width * node->custom3); /* offset */
  }

  bricknum = int(floor((x + offset) / brick_width));

  ins_x = (x + offset) - brick_width * bricknum;
  ins_y = y - row_height * rownum;

  tint = noise((rownum << 16) + (bricknum & 0xFFFF)) + bias;
  CLAMP(tint, 0.0f, 1.0f);

  if (ins_x < mortar_thickness || ins_y < mortar_thickness ||
      ins_x > (brick_width - mortar_thickness) || ins_y > (row_height - mortar_thickness))
  {
    copy_v4_v4(out, mortar);
  }
  else {
    copy_v4_v4(out, bricks1);
    ramp_blend(MA_RAMP_BLEND, out, tint, bricks2);
  }
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

void register_node_type_tex_bricks()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeBricks", TEX_NODE_BRICKS);
  ntype.ui_name = "Bricks";
  ntype.enum_name_legacy = "BRICKS";
  ntype.nclass = NODE_CLASS_PATTERN;
  bke::node_type_socket_templates(&ntype, inputs, outputs);
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Middle);
  ntype.initfunc = init;
  ntype.exec_fn = exec;
  ntype.flag |= NODE_PREVIEW;

  bke::node_register_type(ntype);
}

}  // namespace blender
