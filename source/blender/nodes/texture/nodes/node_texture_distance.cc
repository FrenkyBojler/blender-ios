/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include "BLI_math_vector.h"
#include "node_texture_util.hh"

namespace blender {

static bke::bNodeSocketTemplate inputs[] = {
    {.type = SOCK_VECTOR,
     .name = N_("Coordinate 1"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = -1.0f,
     .max = 1.0f,
     .subtype = PROP_NONE},
    {.type = SOCK_VECTOR,
     .name = N_("Coordinate 2"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = -1.0f,
     .max = 1.0f,
     .subtype = PROP_NONE},
    {.type = -1, .name = ""},
};

static bke::bNodeSocketTemplate outputs[] = {
    {.type = SOCK_FLOAT, .name = N_("Value")},
    {.type = -1, .name = ""},
};

static void valuefn(float *out, TexParams *p, bNode * /*node*/, bNodeStack **in, short thread)
{
  float co1[3], co2[3];

  tex_input_vec(co1, in[0], p, thread);
  tex_input_vec(co2, in[1], p, thread);

  *out = len_v3v3(co2, co1);
}

static void exec(void *data,
                 int /*thread*/,
                 bNode *node,
                 bNodeExecData *execdata,
                 bNodeStack **in,
                 bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &valuefn, static_cast<TexCallData *>(data));
}

void register_node_type_tex_distance()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeDistance", TEX_NODE_DISTANCE);
  ntype.ui_name = "Distance";
  ntype.enum_name_legacy = "DISTANCE";
  ntype.nclass = NODE_CLASS_CONVERTER;
  bke::node_type_socket_templates(&ntype, inputs, outputs);
  ntype.exec_fn = exec;

  bke::node_register_type(ntype);
}

}  // namespace blender
