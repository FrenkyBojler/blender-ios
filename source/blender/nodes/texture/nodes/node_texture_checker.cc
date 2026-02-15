/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include "node_texture_util.hh"
#include <cmath>

namespace blender {

static bke::bNodeSocketTemplate inputs[] = {
    {.type = SOCK_RGBA,
     .name = N_("Color1"),
     .val1 = 1.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 1.0f},
    {.type = SOCK_RGBA,
     .name = N_("Color2"),
     .val1 = 1.0f,
     .val2 = 1.0f,
     .val3 = 1.0f,
     .val4 = 1.0f},
    {.type = SOCK_FLOAT,
     .name = N_("Size"),
     .val1 = 0.5f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = 0.0f,
     .max = 100.0f,
     .subtype = PROP_UNSIGNED},
    {.type = -1, .name = ""},
};
static bke::bNodeSocketTemplate outputs[] = {
    {.type = SOCK_RGBA, .name = N_("Color")},
    {.type = -1, .name = ""},
};

static void colorfn(float *out, TexParams *p, bNode * /*node*/, bNodeStack **in, short thread)
{
  float x = p->co[0];
  float y = p->co[1];
  float z = p->co[2];
  float sz = tex_input_value(in[2], p, thread);

  /* 0.00001  because of unit sized stuff */
  int xi = int(fabs(floor(0.00001f + x / sz)));
  int yi = int(fabs(floor(0.00001f + y / sz)));
  int zi = int(fabs(floor(0.00001f + z / sz)));

  if ((xi % 2 == yi % 2) == (zi % 2)) {
    tex_input_rgba(out, in[0], p, thread);
  }
  else {
    tex_input_rgba(out, in[1], p, thread);
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

void register_node_type_tex_checker()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeChecker", TEX_NODE_CHECKER);
  ntype.ui_name = "Checker";
  ntype.enum_name_legacy = "CHECKER";
  ntype.nclass = NODE_CLASS_PATTERN;
  bke::node_type_socket_templates(&ntype, inputs, outputs);
  ntype.exec_fn = exec;
  ntype.flag |= NODE_PREVIEW;

  bke::node_register_type(ntype);
}

}  // namespace blender
